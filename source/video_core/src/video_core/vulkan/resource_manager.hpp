#pragma once

#include "awaitable.hpp"
#include "framework/image_format.hpp"
#include "memory.h"

#include <vulkan/vulkan.hpp>

#include <boost/container/stable_vector.hpp>
#include <boost/container/small_vector.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

using PAddr = uint32_t;

namespace Memory {
struct PhysicalMemory;
}

namespace Pica {

struct FullTextureConfig;

struct Context;

namespace Vulkan {

class MemoryTypeDatabase;

struct MemoryRange {
    PAddr start;
    uint32_t num_bytes;
};

struct Resource {
    MemoryRange range;

    // TODO: Move to Texture/RenderTarget
    enum class State {
        Invalidated,       // No hooks
        StagingUpToDate,   // Write hook only
        HostUpToDate,      // Read and Write hooks
        Synchronized,      // Write hook only

        // The host data of this resource is not up to date, but it references another host resource that is
//        OtherHostUpToDate, // Read and Write hooks
    } state = State::Invalidated;

    explicit Resource(MemoryRange range_) : range(range_) {

    }

    Resource(const Resource&) = delete;

    Resource& operator=(const Resource&) = delete;

    virtual ~Resource() = default;
};

struct TextureMemory {
    vk::MemoryAllocateInfo memory_info;
    vk::UniqueDeviceMemory memory;


    // Memory management state

    struct Partition {
        uint32_t start;
        uint32_t size;

        bool operator<(const Partition& oth) const {
            return start < oth.start;
        }

        uint32_t AlignedPartitionStart(uint32_t bits) const {
            uint32_t alignment = uint32_t { 1 } << bits;
            return (start + alignment) & ~(alignment - 1);
        }

        uint32_t AvailableSizeWithAlignmentOf(uint32_t bits) const {
            return size - (AlignedPartitionStart(bits) - start);
        }
    };

    std::set<Partition> free_partitions;

    Partition FindFreePartition(vk::MemoryRequirements memory_requirements);

    void ReleasePartition(); // TODO
};

struct StagedMemoryChunk {
    // TODO: Not needed
    vk::Fence upload_fence;
    vk::Fence download_fence;


    vk::DeviceSize start_offset; // Offset into staging_memory. TODO: Appears to be unused?
    vk::DeviceSize num_bytes;

    vk::UniqueDeviceMemory memory {};
    vk::UniqueBuffer buffer {};

    // Must be waited for before queueing any host operations that write the render target staging areas
    vk::UniqueFence staging_areas_host_writeable;
};

struct TextureResource : Resource {
    TextureResource(MemoryRange range_) : Resource(range_) {

    }

    vk::ImageCreateInfo info;
    vk::ImageLayout image_layout;
    vk::UniqueImage image;
    vk::UniqueImageView image_view;

    uint32_t stride;

    // Guards GPU read access to this resource
    CPUAwaitable access_guard;

    // Format of data resident in emulated memory
    GenericImageFormat source_format;

    TextureMemory::Partition memory_partition;

    StagedMemoryChunk staging_area;
};

class ResourceManager;

/**
 * Resource representing render targets used by emulated applications.
 *
 * Read hooks guard access to the backing emulated memory if a pending
 * render operation is in progress. The hooks must be enabled explicitly
 * by calling ProtectBackingEmulatedMemory and will deregister automatically.
 *
 * STATES:
 * * Synchronized:
 *   * host memory: Is up to date
 *     * Writing to host memory overlapping with this resource invalidates emulated memory (leading to either Host-up-to-date or Host-overwritten-by-host state)
 *   * staging memory: Is up to date
 *   * emulated memory: Is up to date
 *     * Writes will invalidate host and staging memory (leading to Invalidated state)
 * * Host-up-to-date:
 *   * host memory: Is up to date and the source of truth
 *     * Writing to host memory of a resource that has overlapping memory range with this one leads to Host-overwritten-by-host state
 *   * staging memory: Is out of date
 *   * emulated memory: Is out of date
 *     * Reads will trigger a synchronizing flush from host memory (leaving state unchanged)
 *       * TODO: Currently, Reads will actually *also* invalidate host and staging memory
 *     * Writes will flush the resource to emulated memory and then invalidate host and staging memory (leading to Invalidated state)
 *       * TODO: Currently, the flush will be skipped
 *  * Host-overwritten-by-host:
 *    * host memory: Is out of date, but the source of truth resides in host memory via a different Resource (i.e. a RenderTarget overlapping with this one)
 *      * Can be synchronized through host operations (leading to Host-up-to-date state)
 *    * otherwise like Host-up-to-date
 *  * Staging-up-to-date:
 *    * host memory: Is out of date but can be reloaded from staging memory (leading to Host-up-to-date state)
 *    * staging memory: Is in synch with emulated memory
 *    * emulated memory:
 *      * Writes will invalidate staging memory (leading to Invalidated state)
 *  * Invalidated:
 *    * host memory: Is out of date and cannot be synchronized without leaving this state first
 *    * staging memory: Is out of date.
 *      * Synchronizing with emulated memory brings the resource into Staging-up-to-date state
 *    * emulated memory: Is the source of truth
 *
 *
 *    TODO: Add support for rectangular invalidation. E.g. if the bottom-right quarter of an RT is invalidated, a subimage at the top-left can still be considered host-up-to-date!
 */
struct RenderTargetResource : TextureResource {
    explicit RenderTargetResource(MemoryRange range_, GenericImageFormat format)
        : TextureResource(range_) {
        source_format = format;
    }

    // Load staging resource
    void Refresh(vk::CommandBuffer, vk::Buffer, vk::DeviceSize source_offset, vk::DeviceSize num_bytes);

    // Copy data to staging resource
    void PrepareFlush(vk::CommandBuffer, vk::Buffer, vk::DeviceSize target_offset, vk::DeviceSize num_bytes);

    // To be called when rendering to a RenderTarget is being rendered to.
    //
    // If necessary, this will register ReadHooks that enforce pending host GPU
    // rendering operations on this resource to be flushed and resulting data
    // to be made visible to emulated memory.
    void ProtectBackingEmulatedMemory(Memory::PhysicalMemory&, ResourceManager&);

    bool IsDepthStencil() const noexcept {
        return (source_format == GenericImageFormat::D16 ||
                source_format == GenericImageFormat::D24 ||
                source_format == GenericImageFormat::D24S8);
    }
};

class LinkedRenderTargetResource {
public:
    RenderTargetResource& color_resource;
    RenderTargetResource* ds_resource; // depth stencil resource

    vk::UniqueRenderPass renderpass;
    vk::UniqueFramebuffer fb;
};

// List of resources referring to an emulated memory page
struct TrackedMemoryPage {
    struct Entry {
        Resource* resource;
        Memory::HookKind hook_kind;
    };

    /**
     * List of resources overlapping with this page, each paired with the
     * memory HookKinds that are active on them.
     *
     * These Resources are mutually non-overlapping with regards to their
     * corresponding emulated memory regions
     */
    boost::container::small_vector<Entry, 2> resources;

    // TODO: Also keep around a mirror of the original data
};

struct ResourceReadHandler : public Memory::ReadHandler {
    ResourceReadHandler(Memory::PhysicalMemory& mem_, ResourceManager& manager_) : mem(mem_), manager(manager_) {}

    Memory::PhysicalMemory& mem;
    ResourceManager& manager;

    void OnRead(PAddr read_addr, uint32_t read_size) override;
};

struct ResourceWriteHandler : public Memory::WriteHandler {
    ResourceWriteHandler(Memory::PhysicalMemory& mem_, ResourceManager& manager_) : mem(mem_), manager(manager_) {}

    Memory::PhysicalMemory& mem;
    ResourceManager& manager;

    void OnWrite(PAddr write_addr, uint32_t write_size, uint32_t /*write_value*/) override;
};

class ResourceManager {
    Memory::PhysicalMemory& mem;

public:
    ResourceReadHandler resource_read_handler { mem, *this };
    ResourceWriteHandler resource_write_handler { mem, *this };

// TODO: Un-public-ize
//private:
    vk::Device device;

    vk::Queue queue;

    vk::CommandPool pool;

    const MemoryTypeDatabase& memory_types;

    // NOTE: LinkedRenderTargetResource holds references to the elements in
    //       this container, so we use a stable_vector to make sure elements
    //       are not relocated upon expansion
    boost::container::stable_vector<std::unique_ptr<RenderTargetResource>> render_targets;

    // Combination of color+depth RenderTargetResource and the corresponding vk::UniqueFramebuffer
    boost::container::stable_vector<LinkedRenderTargetResource> linked_render_targets;

    TextureMemory texture_memory;

    // NOTE: The 3DS GPU only has access to these two busses
    std::array<TrackedMemoryPage, Memory::FCRAM::size / 0x1000> fcram_pages;
    std::array<TrackedMemoryPage, Memory::VRAM::size / 0x1000> vram_pages;

    /**
     * Gets the TrackedMemoryPage corresponding to the given address.
     *
     * The adress must refer to either FCRAM or VRAM, otherwise an exception
     * is thrown.
     */
    TrackedMemoryPage& GetTrackedMemoryPage(PAddr);

    struct TextureCache {
        std::unordered_map<uint32_t, TextureResource> entries;
    };

    TextureCache texture_cache;

    // If true, no Vulkan API calls are used
    bool test_mode = false;

    void RefreshStagedMemory(   MemoryRange,
                                StagedMemoryChunk&,
                                uint32_t width, uint32_t height, uint32_t stride,
                                GenericImageFormat, bool is_render_target);

    void ProtectFromWriteAccess(Resource&, const MemoryRange&);
    void ProtectFromWriteAccess(Resource& resource);

    void FlushRenderTarget(RenderTargetResource&);

    void InvalidateResource(Resource&);

    ResourceManager(Memory::PhysicalMemory&, const MemoryTypeDatabase&);

public:
    static ResourceManager CreateTestInstance(Memory::PhysicalMemory&);

    ResourceManager(Memory::PhysicalMemory&, vk::Device, vk::Queue, vk::CommandPool, const MemoryTypeDatabase&);
    ~ResourceManager();

    TextureResource& LookupTextureResource(const Pica::FullTextureConfig&);

    void RefreshTextureMemory(vk::CommandBuffer, TextureResource&, Memory::PhysicalMemory&, const Pica::FullTextureConfig&);

    /**
     * Wait until pending render operations that overlap with the given
     * memory range are finished and encode their results back to
     * emulated memory.
     *
     * Resources that were not in HostUpToDate or Synchronized state will be
     * invalidated.
     *
     * Some flushed resources may extend past the given range. In that case,
     * the entire resource is flushed regardless, and consequently resources
     * fully outside of the given range may be invalidated.
     */
    void FlushRange(PAddr start, uint32_t num_bytes);

    /**
     * Invalidate host GPU resources that were initialized based
     * on emulated memory contents overlapping with the given range.
     *
     * Invalidation does not cause immediate reloading of the resource data;
     * this is deferred to the next point of use.
     */
    void InvalidateRange(PAddr start, uint32_t num_bytes);

    void InvalidateOverlappingResources(Resource&);

    void UploadStagedDataToHost(vk::CommandBuffer);

    /**
     * Creates a color or depth/stencil target from the given memory area
     *
     * The GPU-side resources are either created on-the-fly or reused from
     * previous invocations of this call.
     *
     * When parameters (format, dimensions, ...) used for a particular render
     * target address change compared to the previous call to this function
     * with that address, the previously allocated resource is destroyed and
     * recreated with the new set of parameters. When that happens, data from
     * the previous resource is automatically transferred to the new resource.
     *
     * TODO: Add hints for:
     * - Renderer::BlitImage: Allow for quick depth => target copies. Currently, depth buffers will be destroyed and reencoded into a color buffer (and later on reencoded back into a depth buffer) when a direct copy would suffice
     * - Renderer::ProduceFrame: Allow vkBuffer-based textures, used for uploading to other textures
     *
     * TODO: Keep record of memory fills even if they don't touch a render target. That way, we can do a quick-fill on render target creation instead of loading known data from memory!
     */
    RenderTargetResource& PrepareRenderTarget(
            const MemoryRange&, uint32_t width, uint32_t height,
            GenericImageFormat, uint32_t stride = 0);

    struct RenderTargetResourceArea {
        RenderTargetResource& resource;
        uint32_t start_y;
        uint32_t end_y; // exclusive bound
    };

    /**
     * Retrieves a color or depth/stencil target from the given area, similar
     * to PrepareRenderTarget, with less strict criteria for reusing existing
     * resources than the latter.
     *
     * This functions returns an existing render target if the general image
     * properties (format, size, ...) match *and* the resource subrange is a
     * superset of the given range. Matching resources are paired with a
     * subrect area that must be applied to any operation consuming the
     * resource.
     *
     * The typical use case of this function over PrepareRenderTarget is
     * partial image copies.
     */
    RenderTargetResourceArea PrepareToCopyRenderTargetArea(
            const MemoryRange&, uint32_t width, uint32_t height,
            GenericImageFormat, uint32_t stride);

    /**
     * Link a color render target with an optional depth stencil buffer for
     * use in a Vulkan framebuffer.
     */
    LinkedRenderTargetResource& LinkRenderTargets(
            RenderTargetResource& color, RenderTargetResource* depth_stencil);
};

inline vk::ImageSubresourceRange GetVkImageSubresourceRange(const RenderTargetResource& target) {
    vk::ImageAspectFlags aspect_flags = (target.IsDepthStencil() ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor);
    return { aspect_flags, 0, 1, 0, 1 };
}

} // namespace Vulkan

} // namespace Pica
