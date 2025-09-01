#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1

#include "resource_manager.hpp"
#include "layout_transitions.hpp"

#include "memory.h"

#include <vulkan_utils/debug_markers.hpp>
#include <vulkan_utils/memory_types.hpp>

#include <platform/gpu/pica.hpp>

#include <framework/exceptions.hpp>

#include <spdlog/sinks/null_sink.h>

#include <range/v3/algorithm/find.hpp>
#include <range/v3/algorithm/find_if.hpp>
#include <range/v3/algorithm/min_element.hpp>
#include <range/v3/algorithm/none_of.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/take.hpp>
#include <range/v3/view/zip.hpp>

#include <iostream>

#include "../debug_utils/debug_utils.h"

// TODO: Move to memory.h?
auto IterateMemoryPages(Memory::MemoryRange range) {
    struct It {
        It() = default;
        It(const It& it) : range(it.range) {}
        It(Memory::MemoryRange range_) : range(range_) {}

        Memory::MemoryRange range;
        uint32_t end_page_index = ((range.start & 0xfff) + 0x1000 + range.num_bytes - 1) >> 12;
        uint32_t page_index = 0;

        It& operator++() {
            ++page_index;
            return *this;
        }

        It operator++(int) {
            It it = *this;
            ++*this;
            return it;
        }

        bool operator==(const It& it) const {
            return page_index == it.page_index;
        }

        bool operator!=(const It& it) const {
            return page_index != it.page_index;
        }

        Memory::MemoryRange operator*() const {
            PAddr page_start = ((range.start >> 12) + page_index) << 12;
            PAddr hook_range_start = page_index == 0 ? range.start : page_start;
            PAddr hook_range_end = (page_index == end_page_index - 1) ? range.start + range.num_bytes : (page_start + 0x1000);
            return Memory::MemoryRange { hook_range_start, hook_range_end - hook_range_start };
        }
    };

    struct Ret {
        Memory::MemoryRange range;

        It begin() const {
            return { range };
        }

        It end() const {
            It it { range };
            it.page_index = it.end_page_index;
            return it;
        }
    };

    return Ret { range };
}

// stdlibc++ versions older than 10 don't provide contiguous_iterator_tag,
// which makes range-v3 fail to recognize that small_vector is contiguously
// iterable. As a workaround, define that tag type manually.
#if defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE < 10
namespace std {
struct contiguous_iterator_tag : public std::random_access_iterator_tag { };
}
#endif

namespace Pica {

namespace Vulkan {

// TODO: Detect at runtime
const bool requires_32bit_depth = true; /* True if device does not support 24-bit depth and instead needs 32-bit */
const vk::Format internal_depth_format = requires_32bit_depth ? vk::Format::eD32SfloatS8Uint : vk::Format::eD24UnormS8Uint;

void ResourceReadHandler::OnRead(PAddr read_addr, uint32_t read_size) {
    // Check if any resources overlap with the written data, if yes invalidate them
    auto overlaps = [=](Resource* resource) {
        return (read_addr + read_size > resource->range.start && read_addr < resource->range.start + resource->range.num_bytes);
    };
    auto& tracked_page = manager.GetTrackedMemoryPage(read_addr);
    auto resource_it = ranges::find_if(tracked_page.resources, overlaps, &TrackedMemoryPage::Entry::resource);
    if (resource_it != tracked_page.resources.end()) {
        auto next = std::next(resource_it);
        if (next != tracked_page.resources.end() && overlaps(next->resource)) {
            // NOTE: Since 3DS GPU resources are 8-byte aligned, this can only happen if several resources were to alias each other
            throw std::runtime_error("Read operation affects multiple resources");
        }

        // TODO: Call a virtual member function instead...
        if (auto render_target = dynamic_cast<RenderTargetResource*>(resource_it->resource)) {
            if (render_target->state != Resource::State::Synchronized) {
                fmt::print("Memory read from render target at {:#x}, triggering flush and deregistering read hooks\n", render_target->range.start);

                if (render_target->state != Resource::State::HostUpToDate) {
                    throw std::runtime_error(fmt::format("Read hook at {:#x} hit render target {:#x}-{:#x} that isn't in HostUpToDate state",
                                        read_addr, render_target->range.start, render_target->range.start + render_target->range.num_bytes));
                }

                manager.FlushRange(render_target->range.start, render_target->range.num_bytes);
            } else {
                // Synchronized render targets don't need read hooks,
                // so validate there is some other resource in this page that needs it
                // TODO: Check that there's a non-synchronized resource specifically
                ValidateContract(tracked_page.resources.size() > 1);
            }
        }
    }
}

void ResourceWriteHandler::OnWrite(PAddr write_addr, uint32_t write_size, uint32_t /*write_value*/) {
    // Check if any resources overlap with the written data, if yes invalidate them
    auto overlaps = [=](Resource* resource) {
        return (write_addr + write_size > resource->range.start && write_addr < resource->range.start + resource->range.num_bytes);
    };
    auto& tracked_page = manager.GetTrackedMemoryPage(write_addr);

    // TODO: Iterate over all resources that are overlapping (since the write might cross a double-word boundary)
    auto resource_it = ranges::find_if(tracked_page.resources, overlaps, &TrackedMemoryPage::Entry::resource);
    if (resource_it != tracked_page.resources.end()) {
        MemoryRange range = resource_it->resource->range;

        // Invalidate host GPU memory, and clear WriteHook
        // TODO: Should flush other parts of this resource to memory first!
        // TODO: Consider only invalidating the current page
        manager.InvalidateRange(range.start, static_cast<uint32_t>(range.num_bytes));
    }
}

static vk::ImageCreateInfo ToVkImageInfoForTextureCache(uint32_t width, uint32_t height) {
    // NOTE: All textures are created as possible render targets by default.
    //       To be used as a depth texture, a full copy to another resource is required though
    vk::ImageUsageFlags usage_flags = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled
                                        | vk::ImageUsageFlagBits::eColorAttachment;
    return    { vk::ImageCreateFlags { }, vk::ImageType::e2D,
                vk::Format::eR8G8B8A8Unorm, // TODO: Should we prefer any particular format here?
                vk::Extent3D { width, height, 1 },
                1, // TODO: Use proper number of mip levels!
                1,
                vk::SampleCountFlagBits::e1,
                vk::ImageTiling::eOptimal,
                usage_flags,
                vk::SharingMode::eExclusive,
                0, nullptr, // queue families
                vk::ImageLayout::eUndefined
              };
}

auto TextureMemory::FindFreePartition(vk::MemoryRequirements memory_requirements) -> Partition {
    if (memory_requirements.size > memory_info.allocationSize) {
        throw std::runtime_error("Texture size requirements exceed texture memory size");
    }

    auto allocate_partition = [&](uint32_t start, uint32_t size) {
        // Find the free partition containing the given region
        auto it = std::prev(free_partitions.lower_bound(Partition { start, { } }));
        if (it->start > start || it->start + it->size < start + size) {
            throw std::runtime_error("This partition cannot fit the requested amount of memory");
        }

        Partition left { it->start, start - it->start };
        Partition right { start + size, it->start + it->size - start - size };
        free_partitions.erase(it);
        if (left.size) {
            free_partitions.insert(left);
        }
        if (right.size) {
            free_partitions.insert(right);
        }

        return Partition { start, size };
    };

std::size_t count = 0;
    for (auto& partition : free_partitions) {
        // Round up to next page just to be safe with regards to alignment...
        // TODO: Pages are 12-bits in size though!!
        ++count;
        if (partition.AvailableSizeWithAlignmentOf(16) >= memory_requirements.size) {
        std::cerr << "Had to look through " << std::dec << count << " partitions" << std::endl;
            return allocate_partition(partition.AlignedPartitionStart(16), memory_requirements.size);
        }
    }

    throw std::runtime_error("Out of texture memory!");
#if 0
    printf("TRACE: Waiting for texture memory to be free again\n");
    // Wait until vertex buffer memory is free again.. TODO: Only wait for renders that affect the memory in the beginning!
    std::vector<vk::Fence> pending_renders;
    auto get_fence = [](TriangleBatchInFlight& batch) { return *batch.fence; };
    ranges::copy_if(triangle_batches | ranges::view::transform(get_fence),
                    ranges::back_inserter(pending_renders),
                    [](auto fence) -> bool {
                        return fence;
                    });
    if (!pending_renders.empty()) {
        auto result = device.waitForFences(pending_renders, true, std::numeric_limits<uint64_t>::max());
        if (result != vk::Result::eSuccess) {
            throw std::runtime_error(fmt::format("waitForFences failed in tex cache! {}", vk::to_string(result)));
        }
    }

    // Clear pending renders other than ourselves so that the image resources get freed (to avoid aliasing images!)
    for (auto& batch : triangle_batches) {
        batch.fence_awaitable.Wait(device);
    }
    triangle_batches.clear();
//            graphics_queue.waitIdle(); // TODO: batch fences don't include blits right now, so let's just wait for everything

    texture_memory_offset = 0;
#endif
}

ResourceManager::ResourceManager(Memory::PhysicalMemory& mem_, vk::Device device, vk::Queue queue, vk::CommandPool pool_, const MemoryTypeDatabase& memory_types)
        : mem(mem_), device(device), queue(queue), pool(pool_), memory_types(memory_types) {
    {
        // Create dummy texture so we can allocate the texture memory block using the right flags
        auto dummy_image_info = ToVkImageInfoForTextureCache(512, 512);
        auto dummy_image = device.createImageUnique(dummy_image_info);
        auto memory_requirements = device.getImageMemoryRequirements(*dummy_image);

        texture_memory.memory_info = memory_types.GetMemoryAllocateInfo<vk::MemoryPropertyFlagBits::eDeviceLocal>(1024 * 1024 * 4 * 200, memory_requirements);
        texture_memory.memory = device.allocateMemoryUnique(texture_memory.memory_info);
        texture_memory.free_partitions.insert(TextureMemory::Partition { 0, static_cast<uint32_t>(texture_memory.memory_info.allocationSize) });
    }
}

ResourceManager::ResourceManager(Memory::PhysicalMemory& mem_, const MemoryTypeDatabase& memory_types) : mem(mem_), memory_types(memory_types), test_mode(true) {

}

ResourceManager ResourceManager::CreateTestInstance(Memory::PhysicalMemory& mem) {
    // Dummy database that is never used
    // TODO: Clean this up
    MemoryTypeDatabase *db = nullptr;
    return ResourceManager { mem, *db };
}

ResourceManager::~ResourceManager() = default;

static StagedMemoryChunk CreateStagingArea(vk::Device device, const MemoryTypeDatabase& memory_types, vk::DeviceSize num_bytes) {
    StagedMemoryChunk chunk;
    chunk = StagedMemoryChunk {
        {},
        {},
        0,
        num_bytes,
        vk::UniqueDeviceMemory { },
        vk::UniqueBuffer { },
        device.createFenceUnique(vk::FenceCreateInfo { vk::FenceCreateFlagBits::eSignaled })
    };

    vk::BufferCreateInfo staging_buffer_create_info {
        vk::BufferCreateFlagBits { },
        chunk.num_bytes,
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
        vk::SharingMode::eExclusive,
    };
    chunk.buffer = device.createBufferUnique(staging_buffer_create_info);

    auto requirements = device.getBufferMemoryRequirements(*chunk.buffer);

    auto memory_info = memory_types.GetMemoryAllocateInfo<vk::MemoryPropertyFlagBits::eHostVisible, vk::MemoryPropertyFlagBits::eHostCoherent>(requirements);
    chunk.memory = device.allocateMemoryUnique(memory_info);
    device.bindBufferMemory(*chunk.buffer, *chunk.memory, 0);
    return chunk;
}

static bool Overlaps(const MemoryRange& range, PAddr start, uint32_t num_bytes) {
    return !(range.start + range.num_bytes <= start || start + num_bytes <= range.start);
}

static bool Overlaps(const MemoryRange& range, const MemoryRange& other) {
    return Overlaps(range, other.start, other.num_bytes);
}

TextureResource& ResourceManager::LookupTextureResource(const Pica::FullTextureConfig& config) {
    auto memory_range = MemoryRange { config.config.GetPhysicalAddress(), TextureSize(ToGenericFormat(config.format), config.config.width, config.config.height) };

    // First, check if a compatible render target is present at this location
    auto cached_target_is_compatible = [&config, &memory_range](const std::unique_ptr<RenderTargetResource>& target) {
        return  memory_range.start == target->range.start &&
                memory_range.num_bytes == target->range.num_bytes &&
                ToGenericFormat(config.format) == target->source_format &&
                config.config.width == target->info.extent.width &&
                config.config.height == target->info.extent.height;
    };
    auto resource_it = ranges::find_if(render_targets, cached_target_is_compatible);
    if (resource_it != render_targets.end()) {
        fmt::print("Found render target resource for texture at {:#x}-{:#x}\n", memory_range.start, memory_range.start + memory_range.num_bytes);
        return *resource_it->get();
    }

    auto image_info = ToVkImageInfoForTextureCache(config.config.width, config.config.height);
    auto entry_it = texture_cache.entries.find(config.config.GetPhysicalAddress());
    bool inserted_new_image = false;
    if (entry_it == texture_cache.entries.end()) {
        std::tie(entry_it, inserted_new_image) = texture_cache.entries.emplace(std::piecewise_construct, std::make_tuple(config.config.GetPhysicalAddress()), std::make_tuple(memory_range));
        entry_it->second.stride = TextureSize(ToGenericFormat(config.format), config.config.width, config.config.height) / config.config.height;
    }
    auto& entry = entry_it->second;

    const auto full_range = vk::ImageSubresourceRange { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 }; // TODO: Use proper number of mip levels

    // TODO: Invalidate overlapping render targets so they get written back to emulated memory

    if (inserted_new_image || entry.info != image_info || entry.source_format != ToGenericFormat(config.format)) {
        if (!test_mode) {
            entry.access_guard.Wait(device);

            entry.image = device.createImageUnique(image_info);
        }
        entry.info = image_info;
        entry.image_layout = image_info.initialLayout;
        entry.source_format = ToGenericFormat(config.format);
        if (!test_mode) {
            SetDebugName(device, *entry.image, fmt::format("{} texture from {:#x}", entry.source_format, memory_range.start).c_str());
        }
        entry_it->second.stride = TextureSize(ToGenericFormat(config.format), config.config.width, config.config.height) / config.config.height;

        entry.range = memory_range;

        // TODO: If image already existed before, wait until it's not in use anymore

        if (!test_mode) {
            // Allocate new memory
            auto memory_requirements = device.getImageMemoryRequirements(*entry.image);
            if (memory_requirements.size > texture_memory.memory_info.allocationSize / 3) {
                throw std::runtime_error("Underestimated maximum texture memory requirements");
            }

            // TODO: We must find an alternative way of allocating textures in a non-overlapping way such that they can still be cached...
            //       Since we already check for the physical address, we can just flat out allocate FRAM_SIZE+VRAM_SIZE (*8) and use that as our texture memory

            // TODO: Make sure alignment restrictions are met?
            entry.memory_partition = texture_memory.FindFreePartition(memory_requirements);
            device.bindImageMemory(*entry.image, *texture_memory.memory, entry.memory_partition.start);

            vk::ImageViewCreateInfo image_view_info {
                vk::ImageViewCreateFlags { },
                *entry.image,
                vk::ImageViewType::e2D,
                image_info.format,
                vk::ComponentMapping { },
                full_range
            };
            entry.image_view = device.createImageViewUnique(image_view_info);

            entry.staging_area = CreateStagingArea(device, memory_types, entry.info.extent.width * entry.info.extent.height * 4);
        }
        entry.state = Resource::State::Invalidated;
    }

    return entry;
}

auto ResourceManager::GetTrackedMemoryPage(PAddr addr) -> TrackedMemoryPage& {
    if (addr >= Memory::FCRAM::start && addr < Memory::FCRAM::start + Memory::FCRAM::size) {
        return fcram_pages[(addr - Memory::FCRAM::start) >> 12];
    }

    if (addr >= Memory::VRAM::start && addr < Memory::VRAM::start + Memory::VRAM::size) {
        return vram_pages[(addr - Memory::VRAM::start) >> 12];
    }

    throw std::runtime_error("ResourceManager attempted to read memory that is neither FCRAM nor VRAM");
}

template<typename F>
static void ForEachUncoveredRange(Memory::HookKind hook_kind, const TrackedMemoryPage& tracked_page, Memory::MemoryRange subrange, F&& callback) {
    while (true) {
        // Filter resources with WriteHooks that overlap with the remaining subrange
        auto filtered_range = tracked_page.resources | ranges::views::filter([&](const TrackedMemoryPage::Entry& entry) {
            if (!(Memory::HasWriteHook(hook_kind) && Memory::HasWriteHook(entry.hook_kind)) && !(Memory::HasReadHook(hook_kind) && Memory::HasReadHook(entry.hook_kind))) {
                return false;
            }

            return Overlaps(MemoryRange { subrange.start, subrange.num_bytes }, entry.resource->range);
        });

        // Pick the first resource starting on/after the remaining subrange
        auto remaining_resource_it =
                ranges::min_element(filtered_range,
                    [](const TrackedMemoryPage::Entry& a, const TrackedMemoryPage::Entry& b) {
                        return std::tie(a.resource->range.start, a.resource->range.num_bytes) < std::tie(b.resource->range.start, b.resource->range.num_bytes);
                    }
                );

        if (remaining_resource_it == ranges::end(filtered_range) || !subrange.num_bytes) {
            break;
        }

        // Clear hooks up until that resource, then skip past it
        if (remaining_resource_it->resource->range.start > subrange.start) {
            callback(Memory::MemoryRange { subrange.start, remaining_resource_it->resource->range.start - subrange.start });
        }
        auto diff = remaining_resource_it->resource->range.start + remaining_resource_it->resource->range.num_bytes - subrange.start;
        subrange.start += diff;
        subrange.num_bytes = std::max(subrange.num_bytes, diff) - diff;
    }

    // Clear remaining range
    if (subrange.num_bytes) {
        callback(subrange);
    }
}

static void ClearUnusedHooks(Memory::PhysicalMemory& mem, Memory::HookKind hook_kind, const TrackedMemoryPage& tracked_page, Memory::MemoryRange& subrange) {
    ForEachUncoveredRange(hook_kind, tracked_page, subrange, [&](const Memory::MemoryRange& chunk_range) { Memory::ClearHook(mem, hook_kind, chunk_range.start, chunk_range.num_bytes); });
}

void ResourceManager::ProtectFromWriteAccess(Resource& resource, const MemoryRange& range) {
    for (const auto subrange : IterateMemoryPages({ range.start, range.num_bytes })) {
        auto& tracked_page = GetTrackedMemoryPage(subrange.start);

        // Add WriteHooks for MemoryRanges in this resource that aren't already
        // covered by other resources
        ForEachUncoveredRange(Memory::HookKind::Write, tracked_page, subrange,
                            [&](const Memory::MemoryRange& chunk_range) {
                                Memory::SetHook<Memory::HookKind::Write>(mem, chunk_range.start, chunk_range.num_bytes, resource_write_handler);
                            });

        // If this Resource is already registered, add the WriteHook flag to it.
        // Otherwise, register it.
        auto resource_it = ranges::find(tracked_page.resources, &resource, &TrackedMemoryPage::Entry::resource);
        if (resource_it != tracked_page.resources.end()) {
            if (HasWriteHook(resource_it->hook_kind)) {
            // Encountered when: Running Home Menu, launching SM3DL, returning to Home Menu, switching to SM3DL manual, returning to Home Menu
                throw std::runtime_error(fmt::format(   "Resource at {:#x}-{:#x} was already linked to page at {:#x}-{:#x}",
                                                        resource.range.start, resource.range.start + resource.range.num_bytes,
                                                        subrange.start, subrange.start + subrange.num_bytes));
            }

            resource_it->hook_kind = AddWriteHook(resource_it->hook_kind);
        } else {
            // TODO: Upon resource destruction, actually remove the texture again...
            tracked_page.resources.push_back({ &resource, Memory::HookKind::Write });
            resource_it = std::prev(tracked_page.resources.end());
        }
    }
}

void ResourceManager::ProtectFromWriteAccess(Resource& resource) {
    fmt::print("Write-protecting resource from {:#x}-{:#x}\n", resource.range.start, resource.range.start + resource.range.num_bytes);
    ProtectFromWriteAccess(resource, resource.range);
}

void ResourceManager::RefreshTextureMemory(vk::CommandBuffer command_buffer, TextureResource& resource, Memory::PhysicalMemory& mem, const Pica::FullTextureConfig& config) {
    auto& chunk = resource.staging_area;

    // TODO: Test at call site instead?
    if (resource.state != Resource::State::Invalidated) {
        return;
    }

    if (!test_mode) {
        resource.access_guard.Wait(device);
    }

    RefreshStagedMemory(resource.range, chunk,
                        config.config.width, config.config.height, resource.stride,
                        ToGenericFormat(config.format), false);

    resource.state = Resource::State::Synchronized;
    if (!test_mode) {
        fmt::print("REFRESHING TEXTURE HOST MEMORY\n");

        vk::ImageAspectFlags aspect_flags = vk::ImageAspectFlagBits::eColor;
        vk::BufferImageCopy buffer_image_copy {
            chunk.start_offset,
            0,
            0,
            vk::ImageSubresourceLayers { aspect_flags, 0, 0, 1 },
            vk::Offset3D { },
            vk::Extent3D { resource.info.extent.width, resource.info.extent.height, 1 }
        };
        vk::ImageSubresourceRange full_image = { aspect_flags, 0, 1, 0, 1 };
        TransitionImageLayout(command_buffer, *resource.image, full_image, ImageLayoutTransitionPoint::From(resource.image_layout), ImageLayoutTransitionPoint::ToTransferDst());

        command_buffer.copyBufferToImage(*chunk.buffer, *resource.image, vk::ImageLayout::eTransferDstOptimal, { buffer_image_copy });

        auto target_transition_point = ImageLayoutTransitionPoint::ToShaderRead();
        TransitionImageLayout(command_buffer, *resource.image, full_image,
                              ImageLayoutTransitionPoint::FromTransferDst(), target_transition_point);
        resource.image_layout = target_transition_point.layout;

        // TODO: Get fence for operation?
    }

// TODO: If a 32-bit write crosses a page boundary, call write hooks for *both* pages!

    ProtectFromWriteAccess(resource);
}

void ResourceManager::FlushRenderTarget(RenderTargetResource& target) {
    fmt::print( "PERFORMANCE WARNING: FlushRenderTarget {:#x}-{:#x} ({}x{} {})\n",
                target.range.start, target.range.start + target.range.num_bytes,
                target.info.extent.width, target.info.extent.height, target.source_format);

    auto& chunk = target.staging_area;

    vk::UniqueFence fence;
    vk::UniqueCommandBuffer command_buffer;
    if (!test_mode) {
        if (chunk.download_fence) {
            // TODO: Wait for the fence
        }

        vk::CommandBufferAllocateInfo command_buffer_info { pool, vk::CommandBufferLevel::ePrimary, 1 };
        command_buffer = std::move(device.allocateCommandBuffersUnique({ command_buffer_info })[0]);

        vk::CommandBufferBeginInfo begin_info { vk::CommandBufferUsageFlagBits::eOneTimeSubmit };
        command_buffer->begin(begin_info);
        target.PrepareFlush(*command_buffer, *target.staging_area.buffer, target.staging_area.start_offset, target.staging_area.num_bytes);
        command_buffer->end();

        vk::PipelineStageFlags submit_wait_flags { };
        vk::SubmitInfo submit_info {
            0, nullptr,
            &submit_wait_flags, 1, &*command_buffer, 0, nullptr };

        fence = device.createFenceUnique({});
        queue.submit({ submit_info }, *fence);
    }

    target.state = Resource::State::Synchronized;

    // Unregister resource from tracked page and clear hooks that now refer to no resources
    for (auto subrange : IterateMemoryPages({ target.range.start, target.range.num_bytes })) {
        auto& tracked_page = GetTrackedMemoryPage(subrange.start);

        auto it = ranges::find(tracked_page.resources, &target, &TrackedMemoryPage::Entry::resource);
        if (it == tracked_page.resources.end()) {
            throw std::runtime_error("Resource not contained in tracked page even though it should be");
        }
        ValidateContract(HasReadHook(it->hook_kind));
        if (it->hook_kind == Memory::HookKind::ReadWrite) {
            it->hook_kind = Memory::HookKind::Write;
        } else {
            tracked_page.resources.erase(it);
        }

        ClearUnusedHooks(mem, Memory::HookKind::Read, tracked_page, subrange);
    }

    InvalidateOverlappingResources(target);

    if (!test_mode) {
        // Wait for flush operation to complete
        // TODO: Instead, move ownership of these objects into the current batch so we don't need to stall here
        auto result = device.waitForFences({ *fence }, false, -1);
        if (result != vk::Result::eSuccess) {
            throw std::runtime_error("Failed to wait for fence in ResourceReadHook");
        }
    }

    // TODO: Actually, the encoding should have happened in the shader already. What follows should just be a plain memcpy!

    char* const database = test_mode ? nullptr : reinterpret_cast<char*>(device.mapMemory(*chunk.memory, chunk.start_offset, chunk.num_bytes, vk::MemoryMapFlagBits { }));

    printf("FlushRenderTarget: Flushing %#010x\n", target.range.start);

    uint32_t bpp = 4; // TODO: Get from actual framebuffer format!
    auto target_bpp = NibblesPerPixel(target.source_format) / 2;

    // Temporary unregister write hooks in the target range, since we're writing to emulated memory with the up-to-date data from host GPU memory
    // TODO: Add a better Memory interface that allows an exception for one specific render target
    MemoryRange temporarily_disabled_hooks_range = target.range;
    for (auto subrange : IterateMemoryPages({ temporarily_disabled_hooks_range.start, temporarily_disabled_hooks_range.num_bytes })) {
        auto& tracked_page = GetTrackedMemoryPage(subrange.start);
        // TODO: Check there are no resources overlapping
//        // We just invalidated overlapping resources, so this render target should be the only tracked resource left
//        ValidateContract(tracked_page.resources.size() == 1);
        auto it = ranges::find(tracked_page.resources, &target, &TrackedMemoryPage::Entry::resource);
        ValidateContract(it != tracked_page.resources.end());
        ValidateContract(HasWriteHook(it->hook_kind));
        tracked_page.resources.erase(it);

        ClearHook(mem, Memory::HookKind::Write, subrange.start, subrange.num_bytes);
    }

    auto target_memory = Memory::LookupContiguousMemoryBackedPage<Memory::HookKind::Write>(mem, target.range.start, target.range.num_bytes);

    // Re-protect resource with write hooks
    ProtectFromWriteAccess(target, temporarily_disabled_hooks_range);

    if (test_mode) {
        return;
    }

    const auto width = target.info.extent.width;
    const auto height = target.info.extent.height;
    const auto stride = target.stride;

    for (unsigned y = 0; y < height; ++y) {
        auto off = (height - y - 1) * stride;
        char* data = database + y * width * bpp;
        for (unsigned x = 0; x < width; ++x) {
            switch (target.source_format) {
            case GenericImageFormat::RGBA8:
            {
                uint32_t value;
                memcpy(&value, data, sizeof(value));
                value = ((value & 0xff) << 24) | ((value & 0xff00) << 8) | ((value & 0xff0000) >> 8) | (value >> 24);
                Memory::Write<uint32_t>(target_memory, off, value);
                data += bpp;
                off += target_bpp;
                break;
            }

            case GenericImageFormat::RGB8:
            {
                auto [r, g, b] = std::array<uint8_t, 3> {
                    static_cast<uint8_t>(*data++),
                    static_cast<uint8_t>(*data++),
                    static_cast<uint8_t>(*data++)
                };
                ++data; // Skip alpha channel
                Memory::Write<uint8_t>(target_memory, off++, b);
                Memory::Write<uint8_t>(target_memory, off++, g);
                Memory::Write<uint8_t>(target_memory, off++, r);
                break;
            }

            case GenericImageFormat::RGB565:
            {
                auto [r, g, b] = std::array<uint16_t, 3> {
                    static_cast<uint8_t>(*data++),
                    static_cast<uint8_t>(*data++),
                    static_cast<uint8_t>(*data++)
                };
                ++data; // Skip alpha channel
                Memory::Write<uint16_t>(target_memory, off, ((r >> 3) << 11u) | ((g >> 2) << 5u) | (b >> 3));
                off += target_bpp;
                break;
            }

            case GenericImageFormat::RGBA5551:
            {
                auto [r, g, b, a] = std::array<uint16_t, 4> {
                    static_cast<uint8_t>(*data++),
                    static_cast<uint8_t>(*data++),
                    static_cast<uint8_t>(*data++),
                    static_cast<uint8_t>(*data++)
                };
                Memory::Write<uint16_t>(target_memory, off, ((r >> 3) << 11u) | ((g >> 2) << 5u) | ((b >> 3) << 1u) | (a >> 7));
                off += target_bpp;
                break;
            }

            case GenericImageFormat::RGBA4:
            {
                auto [r, g, b, a] = std::array<uint16_t, 4> {
                    static_cast<uint8_t>(*data++),
                    static_cast<uint8_t>(*data++),
                    static_cast<uint8_t>(*data++),
                    static_cast<uint8_t>(*data++)
                };
                Memory::Write<uint16_t>(target_memory, off, ((r >> 4) << 12u) | ((g >> 4) << 8u) | ((b >> 4) << 4u) | (a >> 4));
                off += target_bpp;
                break;
            }

            case GenericImageFormat::D16:
            {
                // TODO: Needs fixing for 32-bit host depth buffers
                // TODO: Encode properly!
                uint32_t value;
                memcpy(&value, data, sizeof(value));
                value = ((value & 0xff) << 24) | ((value & 0xff00) << 8) | ((value & 0xff0000) >> 8) | (value >> 24);
                uint16_t val16 = static_cast<uint16_t>((value & 0xffffff) >> 8);
                Memory::Write<uint16_t>(target_memory, off, val16);
                data += bpp;
                off += target_bpp;
                break;
            }

            case GenericImageFormat::D24:
            {
                // TODO: Encode properly!
                uint32_t value;
                memcpy(&value, data, sizeof(value));
                value = ((value & 0xff) << 24) | ((value & 0xff00) << 8) | ((value & 0xff0000) >> 8) | (value >> 24);
                Memory::Write<uint8_t>(target_memory, off++, value & 0xff);
                Memory::Write<uint8_t>(target_memory, off++, (value >> 8) & 0xff);
                Memory::Write<uint8_t>(target_memory, off++, (value >> 16) & 0xff);
                data += bpp;
                break;
            }

            case GenericImageFormat::D24S8:
            {
                // TODO: Support float32 host formats
                // TODO: Encode properly!
                // TODO: We really need some tests here...
                // TODO: Fix for stencil/depth split
                uint32_t value;
                memcpy(&value, data, sizeof(value));
                value = ((value & 0xff) << 24) | ((value & 0xff00) << 8) | ((value & 0xff0000) >> 8) | (value >> 24);
//                auto prev = Memory::Read<uint32_t>(target_memory, off);
                Memory::Write<uint32_t>(target_memory, off, value);
                data += bpp;
                off += target_bpp;
                break;
            }

            default:
                throw std::runtime_error(fmt::format("Unrecognized frame buffer format {}", target.source_format));
            }
        }
    }

    device.unmapMemory(*chunk.memory);
}

void ResourceManager::FlushRange(PAddr start, uint32_t num_bytes) {
    for (auto& target : render_targets) {
        if (!Overlaps(target->range, start, num_bytes)) {
            continue;
        }

        if (target->state != Resource::State::HostUpToDate) {
            continue;
        }

        FlushRenderTarget(*target);
    }
}

void ResourceManager::InvalidateOverlappingResources(Resource& resource) {
    for (auto& target : render_targets) {
        if (target.get() == &resource || !Overlaps(target->range, resource.range)) {
            continue;
        }

        // TODO: Flush non-overlapping parts of this resource
        InvalidateResource(*target);
    }

    for (auto& entry : texture_cache.entries) {
        if (&entry.second == &resource ||
            entry.second.state == Resource::State::Invalidated ||
            !Overlaps(entry.second.range, resource.range)) {
            continue;
        }

        fmt::print( "InvalidateOverlappingResources: Marking texture@{:#x}-{:#x} with state {} dirty\n",
                    entry.second.range.start, entry.second.range.start + entry.second.range.num_bytes, static_cast<int>(entry.second.state));
        InvalidateResource(entry.second);
    }
}

void ResourceManager::InvalidateResource(Resource& resource) {
    auto previous_state = std::exchange(resource.state, Resource::State::Invalidated);
    if (previous_state == Resource::State::Invalidated) {
        // Nothing to do
        return;
    }

    // Un-track this resource from all pages it's contained in,
    // and unset WriteHooks which now refer to no resources.
    for (auto subrange : IterateMemoryPages({ resource.range.start, resource.range.num_bytes })) {
        auto& tracked_page = GetTrackedMemoryPage(subrange.start);

        auto it = ranges::find(tracked_page.resources, &resource, &TrackedMemoryPage::Entry::resource);
        if (it == tracked_page.resources.end()) {
            throw std::runtime_error(fmt::format("Resource {:#x}-{:#x} not contained in tracked page {:#x} even though it should be",
                                            resource.range.start, resource.range.start + resource.range.num_bytes,
                                            subrange.start));
        }
        ValidateContract(HasWriteHook(it->hook_kind));

        // If there is a ReadHook associated, just remove
        // the WriteHook flag. Otherwise, unregister the
        // resource from this page
        if (it->hook_kind == Memory::HookKind::ReadWrite) {
            it->hook_kind = Memory::HookKind::Read;
        } else {
            tracked_page.resources.erase(it);
        }

        ClearUnusedHooks(mem, Memory::HookKind::Write, tracked_page, subrange);
    }

    // For render targets, also unset ReadHooks (which are only active in HostUpToDate state)
    if (previous_state == Resource::State::HostUpToDate && dynamic_cast<RenderTargetResource*>(&resource)) {
        for (auto subrange : IterateMemoryPages({ resource.range.start, resource.range.num_bytes })) {
            auto& tracked_page = GetTrackedMemoryPage(subrange.start);

            auto it = ranges::find(tracked_page.resources, &resource, &TrackedMemoryPage::Entry::resource);
            if (it == tracked_page.resources.end()) {
                throw std::runtime_error("Resource not contained in tracked page with associated ReadHook even though it should be");
            }
            ValidateContract(HasReadHook(it->hook_kind));

            tracked_page.resources.erase(it);

            ClearUnusedHooks(mem, Memory::HookKind::Read, tracked_page, subrange);
        }
    }
}

void ResourceManager::InvalidateRange(PAddr start, uint32_t num_bytes) {
    fmt::print("ResourceManager::InvalidateRange: {:#x}-{:#x}\n", start, start + num_bytes);

    for (auto& target : render_targets) {
        if (!Overlaps(target->range, start, num_bytes)) {
            continue;
        }

        // TODO: Flush resources that are partially outside of the invalidated range

        fmt::print("Invalidated host memory for RT at {:#x} in state {} -> will refresh from guest memory on next use\n", target->range.start, static_cast<int>(target->state));
        InvalidateResource(*target);
    }

    for (auto& entry : texture_cache.entries) {
        if (Overlaps(entry.second.range, start, num_bytes)) {
            fmt::print("InvalidateRange: Marking texture@{:#x}-{:#x} with state {} dirty\n", entry.second.range.start, entry.second.range.start + entry.second.range.num_bytes, static_cast<int>(entry.second.state));
            InvalidateResource(entry.second);
        }
    }
}

void ResourceManager::RefreshStagedMemory(
        MemoryRange range, StagedMemoryChunk& chunk,
        uint32_t width, uint32_t height, uint32_t stride, GenericImageFormat source_format,
        bool is_render_target /* TODO: Get rid of this parameter, but we need it to determine swizzling layout for now */) {
    if (chunk.upload_fence) {
        // TODO: Wait for the fence
    }

    FlushRange(range.start, range.num_bytes);

    fmt::print( "PERFORMANCE WARNING: RefreshStagedMemory: {} {}x{} @ {:#x}\n", source_format,
                width, height, range.start);

    // TODO: Assert that start_offset actually suits nonCoherentAtomSize...
    auto staging_data = test_mode ? nullptr : reinterpret_cast<unsigned char*>(device.mapMemory(*chunk.memory, chunk.start_offset, chunk.num_bytes, vk::MemoryMapFlags { }));

    const bool is_etc = (source_format == GenericImageFormat::ETC1 || source_format == GenericImageFormat::ETC1A4);
    auto nibbles_per_pixel = is_etc ? 0 : NibblesPerPixel(source_format);
    auto target_bpp = 4; // TODO

    auto source_memory = Memory::LookupContiguousMemoryBackedPage<Memory::HookKind::Read>(mem, range.start, TextureSize(source_format, width, height));

    if (test_mode) {
        return;
    }

    // TODO: Unify texture and render target code paths
    if (!is_render_target) {
        DebugUtils::TextureInfo info {
            range.start, static_cast<int>(width), static_cast<int>(height),
            static_cast<int>(stride),
            FromGenericFormat<TextureFormat>(source_format)
        };
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                auto color = DebugUtils::LookupTexture(source_memory, x, height - y - 1, info);
                memcpy(staging_data++, &color.x, sizeof(color.x));
                memcpy(staging_data++, &color.y, sizeof(color.y));
                memcpy(staging_data++, &color.z, sizeof(color.z));
                memcpy(staging_data++, &color.w, sizeof(color.w));
            }
        }
    } else {
        for (uint32_t y = 0; y < height; ++y) {
            auto fb_data_ptr = staging_data + (height - y - 1) * width * target_bpp;
            for (uint32_t x = 0; x < width; ++x) {
                switch (source_format) {
                case GenericImageFormat::RGBA8:
                 {
                    auto value = Memory::Read<uint32_t>(source_memory, x * nibbles_per_pixel / 2 + y * stride);
                    value = ((value & 0xff) << 24) | ((value & 0xff00) << 8) | ((value & 0xff0000) >> 8) | (value >> 24);
                    memcpy(fb_data_ptr, &value, sizeof(value));
                    fb_data_ptr += sizeof(value);
                    break;
                }

                case GenericImageFormat::RGB8:
                {
                    auto value = Memory::Read<uint32_t>(source_memory, x * nibbles_per_pixel / 2 + y * stride);
                    *fb_data_ptr++ = (value >> 16) & 0xff;
                    *fb_data_ptr++ = (value >> 8) & 0xff;
                    *fb_data_ptr++ = value & 0xff;
                    std::memset(fb_data_ptr++, 0xff, 1);
                    break;
                }

                case GenericImageFormat::RGBA5551:
                {
                    auto value = Memory::Read<uint16_t>(source_memory, x * nibbles_per_pixel / 2 + y * stride);
                    *fb_data_ptr++ = ((value >> 11) & 0x1f) * 255 / 31; // R
                    *fb_data_ptr++ = ((value >> 6) & 0x1f) * 255 / 31;  // G
                    *fb_data_ptr++ = ((value >> 1) & 0x1f) * 255 / 31;  // B
                    *fb_data_ptr++ = ((value) & 0x1) * 255;             // A
                    break;
                }

                case GenericImageFormat::RGB565:
                {
                    auto value = Memory::Read<uint16_t>(source_memory, x * nibbles_per_pixel / 2 + y * stride);
                    *fb_data_ptr++ = ((value >> 11) & 0x1f) * 255 / 31; // R
                    *fb_data_ptr++ = ((value >> 5) & 0x3f) * 255 / 63;  // G
                    *fb_data_ptr++ = ((value) & 0x1f) * 255 / 31;       // B
                    std::memset(fb_data_ptr++, 0xff, 1);                // A
                    break;
                }

                case GenericImageFormat::RGBA4:
                {
                    auto value = Memory::Read<uint16_t>(source_memory, x * nibbles_per_pixel / 2 + y * stride);
                    auto extend_4_to_8 = [](uint8_t val) { return static_cast<uint8_t>((val << 4) | val); };
                    *fb_data_ptr++ = extend_4_to_8((value >> 12) & 0xf);
                    *fb_data_ptr++ = extend_4_to_8((value >> 8) & 0xf);
                    *fb_data_ptr++ = extend_4_to_8((value >> 4) & 0xf);
                    *fb_data_ptr++ = extend_4_to_8(value & 0xf);
                    break;
                }

                case GenericImageFormat::D16:
                {
                    auto val = Memory::Read<uint16_t>(source_memory, x * nibbles_per_pixel / 2 + y * stride);
                    uint32_t value = (static_cast<uint32_t>(val) << 8) | (val >> 8);
                    if (internal_depth_format == vk::Format::eD32SfloatS8Uint) {
                        // Convert to 0.0..1.0 range
                        float float_val = double(value) / double { 0xffff };
                        memcpy(&value, &float_val, sizeof(float_val));
                    }
                    memcpy(staging_data, &value, sizeof(value));
                    staging_data += sizeof(value);
                    break;
                }

                case GenericImageFormat::D24:
                case GenericImageFormat::D24S8:
                {
                    // TODO: Fix for stencil/depth split
                    // Lower 24 bit are depth, upper 8 bit are stencil (if present)
                    auto val = Memory::Read<uint32_t>(source_memory, x * nibbles_per_pixel / 2 + y * stride);
                    uint32_t value = val;
                    // TODO: Is the extra byteswap needed??
                    value = ((value & 0xff00) << 8) | ((value & 0xff0000) >> 8) | (value >> 24);
                    if (internal_depth_format == vk::Format::eD32SfloatS8Uint) {
                        // Convert to 0.0..1.0 range
                        float float_val = double(value) / double { 0xffffff };
                        memcpy(&value, &float_val, sizeof(float_val));
                    }
                    memcpy(staging_data, &value, sizeof(value));
                    staging_data += sizeof(value);
                    break;
                }

                default:
                    throw std::runtime_error("TODO: Implement format for RefreshStagedMemory");
                }
            }
        }
    }

    device.unmapMemory(*chunk.memory);
}

static std::unique_ptr<RenderTargetResource> CreateRenderTargetResource(
        vk::Device device, const MemoryTypeDatabase& memory_types,
        TextureMemory& texture_memory, const MemoryRange& range,
        uint32_t width, uint32_t height,
        GenericImageFormat source_format, uint32_t stride, bool test_mode) {
    auto ret = std::make_unique<RenderTargetResource>(MemoryRange { range.start, range.num_bytes }, source_format);

    // TODO: Should we prefer any particular format here?
    // TODO: Check format support, also support fallbacks
    const bool is_depth_stencil = ret->IsDepthStencil();
    vk::Format format = is_depth_stencil ? internal_depth_format : vk::Format::eR8G8B8A8Unorm;

    vk::ImageUsageFlags usage_flags = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    if (is_depth_stencil) {
        usage_flags |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
    } else {
        usage_flags |= vk::ImageUsageFlagBits::eColorAttachment;
    }

    ret->image_layout = vk::ImageLayout::eUndefined;
    ret->info = {
        vk::ImageCreateFlags { },
        vk::ImageType::e2D,
        format,
        vk::Extent3D { width, height, 1 },
        1,
        1,
        vk::SampleCountFlagBits::e1,
        vk::ImageTiling::eOptimal,
        usage_flags,
        vk::SharingMode::eExclusive,
        0, nullptr, // queue families
        ret->image_layout
    };
    ret->stride = stride;
    ret->state = Resource::State::Invalidated;

    if (test_mode) {
        return ret;
    }

    ret->image = device.createImageUnique(ret->info);

    auto memory_requirements = device.getImageMemoryRequirements(*ret->image);
    ret->memory_partition = texture_memory.FindFreePartition(memory_requirements);
    device.bindImageMemory(*ret->image, *texture_memory.memory, ret->memory_partition.start);

    vk::ImageAspectFlags aspect_flags = (is_depth_stencil ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor);

    vk::ImageViewCreateInfo image_view_info {   vk::ImageViewCreateFlags { },
                                                *ret->image,
                                                vk::ImageViewType::e2D,
                                                ret->info.format,
                                                vk::ComponentMapping { },
                                                vk::ImageSubresourceRange { aspect_flags, 0, 1, 0, 1 }
                                            };
    ret->image_view = device.createImageViewUnique(image_view_info);
    static int id = 0;
    ++id;
    auto rt_format = vk::to_string(ret->info.format);
    SetDebugName(device, ret->image, fmt::format("{} RT image (id {}/{:#x}, {})", !is_depth_stencil ? "Color" : "DS", id, range.start, rt_format).c_str());
    SetDebugName(device, ret->image_view, !is_depth_stencil ? "Color RT image view" : "DS RT image view");

    // Allocate staging memory
    // Stencil needs space for 32-bit depth and 8-bit stencil
    ret->staging_area = CreateStagingArea(device, memory_types, ret->info.extent.width * ret->info.extent.height * (is_depth_stencil ? 5 : 4));
    SetDebugName(device, ret->staging_area.memory, !is_depth_stencil ? "Color RT staging memory" : "DS RT staging memory");
    SetDebugName(device, ret->staging_area.buffer, fmt::format("{} RT staging buffer (id {}, addr {:#x}, {})", !is_depth_stencil ? "Color" : "DS", id, range.start, rt_format).c_str());
    SetDebugName(device, ret->staging_area.staging_areas_host_writeable, !is_depth_stencil ? "Color RT staging buffer writeable fence" : "DS RT staging buffer writeable fence");

    return ret;
}

void RenderTargetResource::ProtectBackingEmulatedMemory(Memory::PhysicalMemory& mem, ResourceManager& manager) {
    ValidateContract(state == Resource::State::HostUpToDate || state == Resource::State::Synchronized);

    // If state is HostUpToDate, the Write hook has already been set up before.
    // Otherwise, do so now:

    if (state == Resource::State::Synchronized) {
        state = Resource::State::HostUpToDate;

        // Set up ReadHook to download data to staging buffer
        Memory::GenerateHooks<Memory::HookKind::Read>(
                                    mem, range.start, range.num_bytes,
                                    [&](PAddr page_addr) -> Memory::ReadHandler* {
                                        auto& tracked_page = manager.GetTrackedMemoryPage(page_addr);

                                        const bool is_first_read_hook =
                                            (tracked_page.resources.end() == ranges::find_if(tracked_page.resources, Memory::HasReadHook, &TrackedMemoryPage::Entry::hook_kind));

                                        // If this Resource is already registered,
                                        // add the ReadHook flag to it. Otherwise,
                                        // register it.
                                        auto resource_it = ranges::find(tracked_page.resources, this, &TrackedMemoryPage::Entry::resource);
                                        if (resource_it != tracked_page.resources.end()) {
                                            // Make sure the tracked resource has at least ReadHook flags set
                                            resource_it->hook_kind = AddReadHook(resource_it->hook_kind);
                                        } else {
                                            // TODO: Upon resource destruction, actually remove the texture again...
                                            tracked_page.resources.push_back({ this, Memory::HookKind::Read });
                                        }

                                        // TODO: Only do this for MemoryRanges that aren't covered already by aliasing resources (e.g. render-to-texture)
                                        return &manager.resource_read_handler;
                                    });
    }
}

RenderTargetResource& ResourceManager::PrepareRenderTarget(
        const MemoryRange& target_range, uint32_t width, uint32_t height, GenericImageFormat format, uint32_t stride) {
    // TODO
    if (stride == 0) stride = TextureSize(format, width, height) / height;

    // Find render target resources used previously in this location, if any
    auto cached_target_is_compatible = [target_range, width, height, format](const std::unique_ptr<RenderTargetResource>& target) {
        return  target_range.start == target->range.start &&
                target_range.num_bytes == target->range.num_bytes &&
                format == target->source_format &&
                width == target->info.extent.width &&
                height == target->info.extent.height;
    };

    RenderTargetResource* compatible_target = nullptr;
    for (auto render_target_it = render_targets.begin(); render_target_it != render_targets.end(); ++render_target_it) {
        if (!Overlaps(render_target_it->get()->range, target_range)) {
            continue;
        }

        if (cached_target_is_compatible(*render_target_it)) {
            if (!compatible_target) {
                compatible_target = render_target_it->get();
            } else {
                // If there are multiple compatible render targets, prefer one that is host backed or synchronized.

                // Only one render target may be active (i.e. host backed) for a given memory range at a time
                ValidateContract(compatible_target->state != Resource::State::HostUpToDate && compatible_target->state != Resource::State::Synchronized);

                if (render_target_it->get()->state == Resource::State::HostUpToDate ||
                    render_target_it->get()->state != Resource::State::Synchronized) {
                    compatible_target = render_target_it->get();
                }
            }
        } else if (render_target_it->get()->state == Resource::State::HostUpToDate) {
            // Incompatible render target in the requested MemoryRange => flush host data back to emulated memory
            // TODO: Provide a fast host-to-host conversion path
            fmt::print( "PERFORMANCE WARNING: Flushing resource {:#x}-{:#x} to bring resource {:#x}-{:#x} up to date\n",
                        render_target_it->get()->range.start, render_target_it->get()->range.start + render_target_it->get()->range.num_bytes,
                        target_range.start, target_range.start + target_range.num_bytes);
            FlushRenderTarget(**render_target_it);
        }
    }

    if (!compatible_target) {
        render_targets.push_back(CreateRenderTargetResource(device, memory_types, texture_memory, target_range, width, height, format, stride, test_mode));
        compatible_target = std::prev(render_targets.end())->get();
    }

    auto& target = compatible_target;

    // Allocate staging memory for the resources
    auto& staging = target->staging_area;
    if (target->state == Resource::State::Invalidated) {
        // TODO: If we know this is e.g. the target of a full-sized blit operation, we don't need to load the memory contents first!

        fmt::print("Guest memory for RT at {:#x} is dirty -> refreshing staging memory and scheduling upload to host\n", target->range.start);
        if (!test_mode) {
            auto result = device.waitForFences({*staging.staging_areas_host_writeable}, false, 0xffff'ffff'ffff'ffff);
            if (result != vk::Result::eSuccess) {
                throw std::runtime_error("Failed to wait for render target staging area to be writeable");
            }
        }
        RefreshStagedMemory(target->range, staging, width, height, target->stride, format, true);
        target->state = Resource::State::StagingUpToDate;

        ProtectFromWriteAccess(*compatible_target);
    }

    return *compatible_target;
}

ResourceManager::RenderTargetResourceArea ResourceManager::PrepareToCopyRenderTargetArea(
        const MemoryRange& target_range, uint32_t width, uint32_t height, GenericImageFormat format, uint32_t stride) {
    // Find render target resources used previously in this location, if any
    auto cached_target_is_compatible = [target_range, width, height, format](const std::unique_ptr<RenderTargetResource>& target) {
        return  target_range.start >= target->range.start &&
                target_range.start + target_range.num_bytes <= target->range.start + target->range.num_bytes &&
                format == target->source_format &&
                width == target->info.extent.width &&
                height <= target->info.extent.height &&
                (target->state == Resource::State::HostUpToDate || target->state == Resource::State::Synchronized);
    };

    const auto row_stride = TextureSize(format, width, height) / height;

    RenderTargetResource* compatible_target = nullptr;
    for (auto render_target_it = render_targets.begin(); render_target_it != render_targets.end(); ++render_target_it) {
        if (!Overlaps(render_target_it->get()->range, target_range)) {
            continue;
        }

        if (cached_target_is_compatible(*render_target_it)) {
            if ((target_range.start - render_target_it->get()->range.start) % row_stride != 0) {
                throw Mikage::Exceptions::NotImplemented("Cannot copy from non-row-aligned subareas of render target at {:#x}-{:#x}",
                                                         render_target_it->get()->range.start, render_target_it->get()->range.start + render_target_it->get()->range.num_bytes);
            }

            // Take any compatible render target, but prefer host backed or synchronized ones.
            if (!compatible_target) {
                compatible_target = render_target_it->get();
            } else if ( render_target_it->get()->state == Resource::State::HostUpToDate ||
                        render_target_it->get()->state != Resource::State::Synchronized) {
                compatible_target = render_target_it->get();
            }
        }
    }

    if (!compatible_target) {
        // Fall back to generic code path
        auto& target = PrepareRenderTarget(target_range, width, height, format, stride);
        return { target, 0, target.info.extent.height };
    } else {
        // Run the target we found through PrepareRenderTarget to ensure consistent semantics,
        // then compute the offset into the target and return it

        auto& same_target = PrepareRenderTarget(compatible_target->range, compatible_target->info.extent.width, compatible_target->info.extent.height, compatible_target->source_format, stride);
        ValidateContract(compatible_target == &same_target);
        // Subtract difference in start address from the height, and difference in the end address to the start.
        // This inversion (start<->end) is due to how 3DS textures are laid out vertically mirrored in memory.
        auto offset_start = same_target.range.start + same_target.range.num_bytes - target_range.start - target_range.num_bytes;
        auto offset_end = target_range.start - same_target.range.start;
        ResourceManager::RenderTargetResourceArea ret {
                    same_target,
                    static_cast<uint32_t>(offset_start / row_stride),
                    same_target.info.extent.height - static_cast<uint32_t>((offset_end) / row_stride)
        };
        assert(ret.end_y - ret.start_y == height);

        return ret;
    }
}

LinkedRenderTargetResource& ResourceManager::LinkRenderTargets(RenderTargetResource& color, RenderTargetResource* depth_stencil) {
    // Look up linked render target resource (create new one only if it doesn't already exist)
    auto match_linked_render_target = [&](const LinkedRenderTargetResource& linked) {
        return &linked.color_resource == &color &&
               (!depth_stencil || linked.ds_resource == depth_stencil);
    };
    auto linked_render_target_it = ranges::find_if(linked_render_targets, match_linked_render_target);
    if (linked_render_target_it == linked_render_targets.end()) {
        linked_render_target_it = linked_render_targets.emplace(linked_render_targets.end(),
                                                                LinkedRenderTargetResource {
                                                                    color, depth_stencil,
                                                                    vk::UniqueRenderPass { },
                                                                    vk::UniqueFramebuffer { }
                                                                });
        // Create framebuffer and render pass
        vk::AttachmentReference attachment_ref { 0, vk::ImageLayout::eColorAttachmentOptimal };
        vk::AttachmentReference depth_stencil_attachment_ref { 1, vk::ImageLayout::eDepthStencilAttachmentOptimal};

        std::vector<vk::AttachmentDescription> attachment_desc = {{
            vk::AttachmentDescriptionFlags { },
            color.info.format,
            vk::SampleCountFlagBits::e1,
            vk::AttachmentLoadOp::eLoad,
            vk::AttachmentStoreOp::eStore,
            vk::AttachmentLoadOp::eDontCare,
            vk::AttachmentStoreOp::eDontCare,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::eColorAttachmentOptimal
        }};
        std::vector<vk::ImageView> framebuffer_attachments = {{ *color.image_view }};
        if (depth_stencil) {
            const bool has_stencil = (depth_stencil->source_format == GenericImageFormat::D24S8);

            attachment_desc.emplace_back(
                vk::AttachmentDescriptionFlags { },
                depth_stencil->info.format,
                vk::SampleCountFlagBits::e1,
                vk::AttachmentLoadOp::eLoad,
                vk::AttachmentStoreOp::eStore,
                has_stencil ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eDontCare,
                has_stencil ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eDontCare,
                vk::ImageLayout::eDepthStencilAttachmentOptimal,
                vk::ImageLayout::eDepthStencilAttachmentOptimal
            );
            framebuffer_attachments.emplace_back(*depth_stencil->image_view);
        }

        vk::SubpassDescription subpass_desc {
            vk::SubpassDescriptionFlags { },
            vk::PipelineBindPoint::eGraphics,
            0, nullptr, // Input attachments
            1, &attachment_ref, // color attachments
            nullptr, // resolve attachments
            depth_stencil ? &depth_stencil_attachment_ref : nullptr, // depth stencil attachment
            0, nullptr // preserve attachments
        };

        vk::RenderPassCreateInfo renderpass_info {
            vk::RenderPassCreateFlags { },
            static_cast<uint32_t>(attachment_desc.size()),
            attachment_desc.data(), 1, &subpass_desc, 0, nullptr };
        linked_render_target_it->renderpass = device.createRenderPassUnique(renderpass_info);

        vk::FramebufferCreateInfo info {
            vk::FramebufferCreateFlags { },
            *linked_render_target_it->renderpass,
            static_cast<uint32_t>(framebuffer_attachments.size()),
            framebuffer_attachments.data(),
            color.info.extent.width,
            color.info.extent.height,
            1 };
        linked_render_target_it->fb = device.createFramebufferUnique(info);
    }

    return *linked_render_target_it;
}

// TODO: Add a hint for using this as an input for a transfer operation (to set the target image layout)
void ResourceManager::UploadStagedDataToHost(vk::CommandBuffer command_buffer) {
    for (auto& target : render_targets) {
        auto& chunk = target->staging_area;

        if (target->state != Resource::State::StagingUpToDate) {
            continue;
        }

        target->state = Resource::State::Synchronized;
        if (!test_mode) {
            target->Refresh(command_buffer, *chunk.buffer, chunk.start_offset, chunk.num_bytes);
        }
    }
}

void RenderTargetResource::Refresh(vk::CommandBuffer command_buffer, vk::Buffer source_buffer, vk::DeviceSize source_offset, vk::DeviceSize num_bytes) {
    vk::ImageAspectFlags aspect_flags[] = { vk::ImageAspectFlagBits::eNone, vk::ImageAspectFlagBits::eNone };
    if (source_format == GenericImageFormat::D24) {
        // Copy just depth
        aspect_flags[0] = vk::ImageAspectFlagBits::eDepth;
    } else if (source_format == GenericImageFormat::D24S8) {
        // Copy depth and stencil (separately, as required by Vulkan)
        aspect_flags[0] = vk::ImageAspectFlagBits::eDepth;
        aspect_flags[1] = vk::ImageAspectFlagBits::eStencil;
    } else {
        // Copy color
        aspect_flags[0] = vk::ImageAspectFlagBits::eColor;
    }

    // TODO: Combine image layout transitions
    auto input_layout = image_layout;

    for (auto aspect_flag : aspect_flags) {
        if (aspect_flag == vk::ImageAspectFlagBits::eNone) {
            continue;
        }

        vk::BufferImageCopy buffer_image_copy {
            source_offset,
            0,
            0,
            vk::ImageSubresourceLayers { aspect_flag, 0, 0, 1 },
            vk::Offset3D { },
            vk::Extent3D { info.extent.width, info.extent.height, 1 }
        };
        vk::ImageSubresourceRange full_image = { aspect_flag, 0, 1, 0, 1 };
        TransitionImageLayout(command_buffer, *image, full_image, ImageLayoutTransitionPoint::From(input_layout), ImageLayoutTransitionPoint::ToTransferDst());

        command_buffer.copyBufferToImage(source_buffer, *image, vk::ImageLayout::eTransferDstOptimal, { buffer_image_copy });

        auto target_transition_point = IsDepthStencil() ? ImageLayoutTransitionPoint::ToDepthStencilAttachmentOutput() : ImageLayoutTransitionPoint::ToColorAttachmentOutput();
        TransitionImageLayout(command_buffer, *image, full_image,
                              ImageLayoutTransitionPoint::FromTransferDst(), target_transition_point);
        image_layout = target_transition_point.layout;

        source_offset += info.extent.width * info.extent.height * 4;
    }
}

void RenderTargetResource::PrepareFlush(vk::CommandBuffer command_buffer, vk::Buffer target_buffer, vk::DeviceSize target_offset, vk::DeviceSize num_bytes) {
    vk::ImageAspectFlags aspect_flags[] = { vk::ImageAspectFlagBits::eNone, vk::ImageAspectFlagBits::eNone };
    if (source_format == GenericImageFormat::D24) {
        // Copy just depth
        aspect_flags[0] = vk::ImageAspectFlagBits::eDepth;
    } else if (source_format == GenericImageFormat::D24S8) {
        // Copy depth and stencil (separately, as required by Vulkan)
        aspect_flags[0] = vk::ImageAspectFlagBits::eDepth;
        aspect_flags[1] = vk::ImageAspectFlagBits::eStencil;
    } else {
        // Copy color
        aspect_flags[0] = vk::ImageAspectFlagBits::eColor;
    }

    auto input_layout = image_layout;

    for (auto aspect_flag : aspect_flags) {
        if (aspect_flag == vk::ImageAspectFlagBits::eNone) {
            continue;
        }

        vk::BufferImageCopy buffer_image_copy {
            target_offset,
            0,
            0,
            vk::ImageSubresourceLayers { aspect_flag, 0, 0, 1 },
            vk::Offset3D { },
            vk::Extent3D { info.extent.width, info.extent.height, 1 }
        };

        // TODO: Pipeline barrier to wait for rendering to finish

        GuardedDebugMarker debug_guard(command_buffer, (aspect_flag == vk::ImageAspectFlagBits::eDepth) ? "Flush depth to staging"
                                                        : (aspect_flag == vk::ImageAspectFlagBits::eStencil) ? "Flush stencil to staging" : "Flush color to staging");
        vk::ImageSubresourceRange full_image = { aspect_flag, 0, 1, 0, 1 };
        TransitionImageLayout(command_buffer, *image, full_image, ImageLayoutTransitionPoint::From(input_layout), ImageLayoutTransitionPoint::ToTransferSrc());

    if (!IsDepthStencil()) { // Workaround for Pokemon
        command_buffer.copyImageToBuffer(*image, vk::ImageLayout::eTransferSrcOptimal, target_buffer, { buffer_image_copy });
    }

        auto target_transition_point = IsDepthStencil() ? ImageLayoutTransitionPoint::ToDepthStencilAttachmentOutput() : ImageLayoutTransitionPoint::ToColorAttachmentOutput();
        TransitionImageLayout(command_buffer, *image, full_image,
                              ImageLayoutTransitionPoint::FromTransferSrc(), target_transition_point);
        image_layout = target_transition_point.layout;

        target_offset += info.extent.width * info.extent.height * 4;
    }
}

} // namespace Vulkan

} // namespace Pica
