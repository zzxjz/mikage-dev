#include <thread>
#include "pica.hpp"
#include "../video_core/src/video_core/vulkan/renderer.hpp" // TODO: Get rid of this
#include "../video_core/src/video_core/vulkan/layout_transitions.hpp" // TODO: Get rid of this
#include "../video_core/src/video_core/vulkan/awaitable.hpp" // TODO: Get rid of this

#include <vulkan_utils/device_manager.hpp>

#include <vulkan/vulkan.hpp>

#include <spdlog/logger.h>

#include <SDL_render.h>
#include <SDL_video.h>
#include <SDL_vulkan.h>

#include <vulkan/vulkan.hpp>

#include <optional>
#include <vector>

#include <iostream> // TODO
#include <iomanip> // TODO

#include <range/v3/algorithm/fill.hpp>
#include <range/v3/view/iota.hpp>

// Must be included after range-v3 because of colliding definitions
#include <SDL_syswm.h>

inline constexpr bool enable_framedump = false;

// Layout of 3DS screens as arranged on the host display
struct Layout {
    bool enabled;
    unsigned x, y; // top-left corner
    unsigned width, height; // Displayed size (stretched if needed)
};

class SDLVulkanDisplay : public VulkanInstanceManager, public VulkanDeviceManager, public EmuDisplay::EmuDisplay {
public: // ... TODO
    std::shared_ptr<spdlog::logger> logger;

    SDL_Window& window;
    std::optional<VkSurfaceKHR> surface; // TODO: Make this a UniqueHandle instead, and point it to the instance (otherwise its destructor will crash, apparently). ALSO, REORDER TO AFTER THE INSTANCE!!!

    const std::array<Layout, 3>& layouts; // TODO: use num_screen_ids instead of a hardcoded constant

    vk::UniqueSwapchainKHR swapchain;
    std::vector<vk::Image> swapchain_images;
    std::vector<vk::ImageLayout> swapchain_image_layouts;

    vk::UniqueCommandPool command_pool;
    vk::CommandBuffer* command_buffer = nullptr; // TODO: Get rid of this

    struct FrameData {
        static constexpr uint32_t num_screen_ids = 3;
        static auto ScreenIds() { return ranges::view::iota(uint32_t { 0 }, num_screen_ids); }

        vk::UniqueSemaphore render_finished_semaphore;
        vk::UniqueFence render_finished_fence;
        CPUAwaitable render_finished_fence_awaitable {}; // TODO: Have a helper type that manages the lifetime of the managed fence!

        std::array<bool, num_screen_ids> screen_active {};

        vk::UniqueCommandBuffer command_buffer;

        uint32_t image_index; // swap chain image index
        vk::UniqueSemaphore image_available_semaphore;

        vk::UniqueBuffer framedump_buffer;
        vk::UniqueDeviceMemory framedump_memory;
    };

//    // Data currently shown to the screen. Kept around so we can display it again if the emulation core doesn't provide another image in time
//    std::array<std::shared_ptr<::EmuDisplay::VulkanDataStream>, FrameData::num_screen_ids> active_images;

    std::vector<FrameData> frame_data;
    std::vector<FrameData>::iterator current_frame;

    static std::vector<const char*> GetRequiredExtensions(SDL_Window& window) {
        unsigned count;
        if (!SDL_Vulkan_GetInstanceExtensions(&window, &count, nullptr)) {
            throw std::runtime_error("Failed to query Vulkan extensions required by SDL");
        }

        std::vector<const char*> extensions(count);
        if (!SDL_Vulkan_GetInstanceExtensions(&window, &count, extensions.data())) {
            throw std::runtime_error("Failed to query Vulkan extensions required by SDL");
        }
        extensions.push_back("VK_EXT_debug_report");

        return extensions;
    }

    struct UniqueSurfaceKHR {
        vk::Instance instance;
        vk::SurfaceKHR surface { };

        ~UniqueSurfaceKHR() {
            if (surface) {
                instance.destroySurfaceKHR(surface);
            }
        }
    };

    static UniqueSurfaceKHR CreatePresentSurface(vk::Instance instance, SDL_Window& window) {
        VkSurfaceKHR surface;
        if (!SDL_Vulkan_CreateSurface(&window, instance, &surface)) {
            throw std::runtime_error("Failed to create SDL surface");
        }
        return { instance, surface };
    }

    void ProcessDataSource(::EmuDisplay::Frame& frame, ::EmuDisplay::DataStreamId stream_id) {
//        {
//            auto& active_image = active_images[static_cast<size_t>(input->stream_id)];
//            if (active_image && active_image != input) {
//                // Get previous frame, then link its semaphore against the VulkanDataStream fence
//                // TODO: Find a cleaner way of signaling these.
//                // TODO: Does this work when only a single swapchain image is used?

//                active_image->in_use_by_gpu = GetPreviousFrame()->render_finished_fence_awaitable;

//                active_image->in_use_by_cpu = false;
//            }
//            active_image = input;
//        }

// TODO: Don't hardcode
uint32_t image_width = stream_id == ::EmuDisplay::DataStreamId::BottomScreen ? 320 : 400;
uint32_t image_height = 240;

        if (enable_framedump) { // TODO: Hide behind setting
            // TODO: Synch presentation mode only (?)
            vk::BufferImageCopy buffer_image_copy {
                400 * 240 * 4 * static_cast<size_t>(stream_id), // TODO: proper offset
                0,
                0,
                vk::ImageSubresourceLayers { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
                vk::Offset3D { },
                vk::Extent3D { image_width, image_height, 1 }
            };

            // TODO: Pipeline barrier to wait for rendering to finish

//            vk::ImageSubresourceRange full_image = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
//            TransitionImageLayout(command_buffer, input->image, full_image, ImageLayoutTransitionPoint::From(vk::ImageLayout::eGeneral), ImageLayoutTransitionPoint::ToTransferSrc());

            command_buffer->copyImageToBuffer(*frame.image, vk::ImageLayout::eGeneral/*eTransferSrcOptimal*/, *current_frame->framedump_buffer, { buffer_image_copy });

//            auto target_transition_point = ImageLayoutTransitionPoint::ToColorAttachmentOutput();
//            TransitionImageLayout(command_buffer, input->image, full_image,
//                                  ImageLayoutTransitionPoint::FromTransferSrc(), target_transition_point);
        }

        {
            auto& layout = layouts[static_cast<size_t>(stream_id)];

            auto full_image = vk::ImageSubresourceLayers { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
            auto offset = vk::Offset3D { static_cast<int32_t>(layout.x), static_cast<int32_t>(layout.y), 0 };
            vk::ImageBlit copy_region {
                full_image,
                { vk::Offset3D { 0, 0, 0}, vk::Offset3D { static_cast<int32_t>(image_width), static_cast<int32_t>(image_height), 1 } },
                full_image,
                { offset, vk::Offset3D { static_cast<int32_t>(layout.x + layout.width), static_cast<int32_t>(layout.y + layout.height), 1 } }
            };

            auto swapchain_image = swapchain_images[current_frame->image_index];
            auto& swapchain_image_layout = swapchain_image_layouts[current_frame->image_index];
            if (swapchain_image_layout != vk::ImageLayout::eTransferDstOptimal) {
                throw std::runtime_error("Image should be in transfer dst layout");
            }
            if (layouts[static_cast<size_t>(stream_id)].enabled) {
                command_buffer->blitImage(*frame.image, vk::ImageLayout::eGeneral, swapchain_image, vk::ImageLayout::eTransferDstOptimal, { copy_region }, vk::Filter::eLinear);
            }
        }
        current_frame->screen_active[static_cast<size_t>(stream_id)] = true;
    }

    static bool IsRunningOnWayland(SDL_Window& window) {
        SDL_SysWMinfo info {};
        SDL_VERSION(&info.version);
        SDL_GetWindowWMInfo(&window, &info);
        return (info.subsystem == SDL_SYSWM_WAYLAND);
    }

public:
    SDLVulkanDisplay(std::shared_ptr<spdlog::logger> logger, SDL_Window& window, const std::array<Layout, FrameData::num_screen_ids>& layouts)
        : VulkanInstanceManager(*logger, app_name, GetRequiredExtensions(window)),
        VulkanDeviceManager(*this, *logger, CreatePresentSurface(*instance, window).surface, IsRunningOnWayland(window)),
        EmuDisplay(*logger, physical_device, *device, graphics_queue_index),
        logger(logger), window(window), layouts(layouts) {

            CreateSwapchain();

            // NOTE: eTransient will be very interesting for us, but in the actual rendering thread

            {
                vk::CommandPoolCreateInfo info { vk::CommandPoolCreateFlagBits::eResetCommandBuffer, graphics_queue_index };
                command_pool = device->createCommandPoolUnique(info);
            }

            frame_data.resize(swapchain_images.size());
            for (auto& frame : frame_data) {
                vk::CommandBufferAllocateInfo info { *command_pool, vk::CommandBufferLevel::ePrimary, FrameData::num_screen_ids };
                frame.command_buffer = std::move(device->allocateCommandBuffersUnique(info)[0]);

                frame.image_available_semaphore = device->createSemaphoreUnique(vk::SemaphoreCreateInfo { });

                frame.render_finished_semaphore = device->createSemaphoreUnique(vk::SemaphoreCreateInfo { });

                frame.render_finished_fence = device->createFenceUnique(vk::FenceCreateInfo { vk::FenceCreateFlagBits::eSignaled });

                vk::BufferCreateInfo buffer_info {
                    vk::BufferCreateFlagBits { },
                    400 * 240 * 4 * FrameData::num_screen_ids, /* TODO: Proper size */
                    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
                    vk::SharingMode::eExclusive,
                };
                frame.framedump_buffer = device->createBufferUnique(buffer_info);

                // TODO: Generalize memory type selection
                frame.framedump_memory = device->allocateMemoryUnique(vk::MemoryAllocateInfo { buffer_info.size, /*1*/ 2 });
                device->bindBufferMemory(*frame.framedump_buffer, *frame.framedump_memory, 0);
            }
            current_frame = frame_data.begin();
    }

    void ResetSwapchainResources() {
        swapchain = vk::UniqueSwapchainKHR{};
        swapchain_images.clear();

        if (surface) { // TODO: Having this as an optional is kind of redundant. Instead, use a UniqueSurfaceKHR?
            logger->info("Destroying SDL surface");
            instance->destroySurfaceKHR(*surface);
            surface = {};
        }
    }

    void CreateSwapchain() {
        ResetSwapchainResources();

        VkSurfaceKHR surface_handle;

        // TODO: Use a vk::UniqueHandle here instead
        if (!SDL_Vulkan_CreateSurface(&window, *instance, &surface_handle)) {
            throw std::runtime_error(fmt::format("Failed to create Vulkan SDL surface: {}", SDL_GetError()));
        }
        surface = surface_handle;
        if (!physical_device.getSurfaceSupportKHR(present_queue_index, vk::SurfaceKHR { surface_handle })) {
            throw std::runtime_error("New SDL surface does not support presentation on the previous present queue");
        }

        auto surface_caps = physical_device.getSurfaceCapabilitiesKHR(surface_handle);
        logger->info("Surface width:  {} - {}", surface_caps.minImageExtent.width, surface_caps.maxImageExtent.width);
        logger->info("Surface height: {} - {}", surface_caps.minImageExtent.height, surface_caps.maxImageExtent.height);
        logger->info("Surface image count: {} - {}", surface_caps.minImageCount, surface_caps.maxImageCount);
        if (surface_caps.minImageExtent != surface_caps.maxImageExtent) {
            logger->warn("Expected image extent bounds to be equal");
            // throw std::runtime_error("Expected image extent bounds to be equal");
        }
        logger->info("Surface supported usage flags: {}", vk::to_string(surface_caps.supportedUsageFlags));
        // TODO: Verify the surface has the eTransferDst flag

        auto surface_formats = physical_device.getSurfaceFormatsKHR(surface_handle);
        for (auto& format : surface_formats) {
            logger->info("Surface format: {}, {}", vk::to_string(format.format), vk::to_string(format.colorSpace));
        }

        auto surface_present_modes = physical_device.getSurfacePresentModesKHR(surface_handle);
        for (auto& mode : surface_present_modes) {
            logger->info("Present mode: {}", vk::to_string(mode));
        }

        const uint32_t min_image_count = surface_caps.minImageCount;
        const uint32_t image_array_layers = 1;

        // TODO: Assert this is between minImageExtent and maxImageExtent
        int window_width, window_height;
        SDL_GetWindowSize(&window, &window_width, &window_height);
        logger->info("Swap chain size: {}x{}", window_width, window_height);
        vk::Extent2D swapchain_size = { static_cast<uint32_t>(window_width), static_cast<uint32_t>(window_height) };
        vk::Format swapchain_format = vk::Format::eB8G8R8A8Unorm; // TODO: Don't hardcode. Check against surface_formats instead

        swapchain = std::invoke([&]() {
            vk::SwapchainCreateInfoKHR info { vk::SwapchainCreateFlagBitsKHR { },
                                              *surface,
                                              min_image_count,
                                              swapchain_format,
                                              vk::ColorSpaceKHR::eSrgbNonlinear, // TODO: Check against surface_formats
                                              swapchain_size,
                                              image_array_layers,
                                              vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst,
                                              vk::SharingMode::eExclusive, // TODO: If the present and graphics queues are different, we should use concurrent mode here
                                              1, // queue family count
                                              &graphics_queue_index,
                                              vk::SurfaceTransformFlagBitsKHR::eIdentity,
                                              vk::CompositeAlphaFlagBitsKHR::eOpaque,
                                              vk::PresentModeKHR::eFifo,
                                              1, // clipped
                                              vk::SwapchainKHR { } }; // TODO: Use oldswapchain here on reset?
            return device->createSwapchainKHRUnique(info);
        });

        swapchain_images = device->getSwapchainImagesKHR(*swapchain);

        logger->info("Got {} swapchain images", swapchain_images.size());

        auto full_image_range = vk::ImageSubresourceRange { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };

        swapchain_image_layouts.resize(swapchain_images.size(), vk::ImageLayout::eUndefined);

        if (frame_data.size() && frame_data.size() != swapchain_images.size()) {
            throw std::runtime_error("TODO: Swapchain size changed, not implemented.");
        }
    }

    void BeginFrame() {
//        using clock = std::chrono::steady_clock;
//        static std::array<decltype(clock::now()), 60> last_frames;

//        static auto BEGIN = clock::now();

//        auto this_frame = clock::now();

        // TODO: Commit this change!
        (void)device->waitForFences({*current_frame->render_finished_fence}, true, std::numeric_limits<uint64_t>::max());

        auto [result, next_image_index] = device->acquireNextImageKHR(*swapchain, std::numeric_limits<uint64_t>::max(), *current_frame->image_available_semaphore, vk::Fence { });
        if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
            throw std::runtime_error(fmt::format("Unexpected error in vkAcquireNextImageKHR: {}", vk::to_string(result)));
        }
        if (result == vk::Result::eSuboptimalKHR) {
            device->waitIdle();
            CreateSwapchain();
            BeginFrame();
            return;
        }

        if (!current_frame->render_finished_fence_awaitable.IsReady(*device)) { // Reset CPUAwaitable
            throw std::runtime_error("Couldn't wait on previous UI frame");
        }
//        std::chrono::duration frame_duration = std::chrono::milliseconds { 45 };
//        // TODO: Only sleep if the difference is larger than some specific amount
//        fprintf(stderr, "SLEEPING FOR %d ms (time points %4d %4d %4d %4d %4d %4d %4d %4d %4d %4d)\n",
//                std::chrono::duration_cast<std::chrono::milliseconds>(last_frames.front() + last_frames.size() * frame_duration - this_frame).count(),
//                std::chrono::duration_cast<std::chrono::milliseconds>(last_frames[0] - BEGIN).count(),
//                std::chrono::duration_cast<std::chrono::milliseconds>(last_frames[1] - BEGIN).count(),
//                std::chrono::duration_cast<std::chrono::milliseconds>(last_frames[2] - BEGIN).count(),
//                std::chrono::duration_cast<std::chrono::milliseconds>(last_frames[3] - BEGIN).count(),
//                std::chrono::duration_cast<std::chrono::milliseconds>(last_frames[4] - BEGIN).count(),
//                std::chrono::duration_cast<std::chrono::milliseconds>(last_frames[5] - BEGIN).count(),
//                std::chrono::duration_cast<std::chrono::milliseconds>(last_frames[6] - BEGIN).count(),
//                std::chrono::duration_cast<std::chrono::milliseconds>(last_frames[7] - BEGIN).count(),
//                std::chrono::duration_cast<std::chrono::milliseconds>(last_frames[8] - BEGIN).count(),
//                std::chrono::duration_cast<std::chrono::milliseconds>(last_frames[9] - BEGIN).count());
//        std::this_thread::sleep_until(last_frames.front() + last_frames.size() * frame_duration);

//        std::rotate(std::begin(last_frames), std::begin(last_frames) + 1, std::end(last_frames));
//        last_frames.back() = this_frame;

        device->resetFences({*current_frame->render_finished_fence});
        current_frame->image_index = next_image_index;

        // TODO: Somehow this is still active? Should we wait for the nextImageAcquire fence?
        command_buffer = &*current_frame->command_buffer;
        vk::CommandBufferBeginInfo info { /*vk::CommandBufferUsageFlagBits::eSimultaneousUse | */vk::CommandBufferUsageFlagBits::eOneTimeSubmit };
        command_buffer->begin(info);

        auto& swapchain_image_layout = swapchain_image_layouts[current_frame->image_index];
        swapchain_image_layout = vk::ImageLayout::eUndefined; // TODO: Verify this is what acquireNextImageKHR resets the layout to
        auto full_image_range = vk::ImageSubresourceRange { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
        auto swapchain_image = swapchain_images[current_frame->image_index];
        TransitionImageLayout(*command_buffer, swapchain_image, full_image_range, ImageLayoutTransitionPoint::From(swapchain_image_layout), ImageLayoutTransitionPoint::ToTransferDst());
        swapchain_image_layout = vk::ImageLayout::eTransferDstOptimal;
        command_buffer->clearColorImage(swapchain_image, swapchain_image_layout, vk::ClearColorValue {}, { full_image_range });
    }

    void EndFrame() {
        // TODO: Un-static-fy these
        static int counted_frames[FrameData::num_screen_ids] = {};
        static std::chrono::time_point<std::chrono::high_resolution_clock> start = std::chrono::high_resolution_clock::now();
        bool new_frame_received[FrameData::num_screen_ids] {};
        static int frame_count[FrameData::num_screen_ids] {};

//        for (size_t screen_id = 0; screen_id < FrameData::num_screen_ids; ++screen_id) {
//            if (!active_images[screen_id]) {
//                // Ignore if no frame has ever been submitted for this screen id
//                continue;
//            }

//            if (!current_frame->screen_active[screen_id]) {
//                // Resubmit frames for which no new images have been provided (don't count this towards the displayed FPS, though)
//                // TODO: By allowing one frame to be "dropped" like this, we allow e.g. the right-eye frame to lag behind the left-eye frame. Instead, give dropped parts a chance to catch up!

//                // Reset ready semaphore because we already waited for it
////                active_images[screen_id]->is_ready.semaphore = vk::Semaphore { };
//// TODO: Re-enable
////                ProcessDataSource(active_images[screen_id]);
//            } else {
//                // TODO: Not reliable...
////                auto prev_max = (counted_frames[screen_id] == std::max({counted_frames[0], counted_frames[1], counted_frames[2]}));
//                ++counted_frames[screen_id];
////                new_frame_received = (counted_frames[screen_id] > prev_max);
//new_frame_received[screen_id] = enable_framedump;
//            }
//            current_frame->screen_active[screen_id] = {};
//        }

        auto& swapchain_image_layout = swapchain_image_layouts[current_frame->image_index];
        auto full_image_range = vk::ImageSubresourceRange { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
        auto swapchain_image = swapchain_images[current_frame->image_index];
        TransitionImageLayout(*command_buffer, swapchain_image, full_image_range, ImageLayoutTransitionPoint::FromTransferDst(), ImageLayoutTransitionPoint::ToPresent());
        swapchain_image_layout = vk::ImageLayout::ePresentSrcKHR;
        current_frame->command_buffer->end();

        {
            // TODO: Wait for inputs' readiness GPUAwaitables
            std::array<vk::PipelineStageFlags, 1 + FrameData::num_screen_ids> wait_stages;
            ranges::fill(wait_stages, vk::PipelineStageFlagBits::eColorAttachmentOutput);
            std::vector<vk::Semaphore> wait_semaphores = { *current_frame->image_available_semaphore };
//            for (size_t screen_id = 0; screen_id < FrameData::num_screen_ids; ++screen_id) {
//                if (active_images[screen_id] && active_images[screen_id]->is_ready.semaphore) {
//                    wait_semaphores.push_back(active_images[screen_id]->is_ready.semaphore);
//                }
//            }
            vk::SubmitInfo info {   static_cast<uint32_t>(wait_semaphores.size()), wait_semaphores.data(),
                                    wait_stages.data(),
                                    1, &*command_buffer,
                                    1, &*current_frame->render_finished_semaphore };

            (void)graphics_queue.submit(1, &info, *current_frame->render_finished_fence);
            current_frame->render_finished_fence_awaitable = CPUAwaitable(*current_frame->render_finished_fence);
        }

        // TODO: Consider using an Euler average here: https://github.com/dolphin-emu/dolphin/pull/10982#issuecomment-1214448160
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start);
//        if (dur.count() >= 1'000'000 * 61 / (1 + *std::max_element(std::begin(counted_frames), std::end(counted_frames))) &&
//                std::any_of(active_images.begin(), active_images.end(), [](auto& ptr) { return ptr != nullptr; })) {
//            std::string message = "Frames per second: ";
//            for (size_t screen_id = 0; screen_id < FrameData::num_screen_ids; ++screen_id) {
//                message += fmt::format("{:.3} / ", counted_frames[screen_id] * 1'000'000.f / dur.count());
//                counted_frames[screen_id] = 0;
//            }
//            message.resize(message.size() - 3); // Drop last " / "
//            logger->info(message);
//            std::cerr << message << std::endl;

//            start = std::chrono::high_resolution_clock::now();
//        }

        vk::PresentInfoKHR info { 1, &*current_frame->render_finished_semaphore, 1, &*swapchain, &current_frame->image_index };
        auto result = present_queue.presentKHR(info);
        if (result == vk::Result::eSuboptimalKHR) {
            // TODO: Other drivers might actually return other error codes on resize
            device->waitIdle();
            CreateSwapchain();
        } else if (result != vk::Result::eSuccess) {
            // TODO: On VK_ERROR_OUT_OF_DATE_KHR, we should recreate the swap chain!
            logger->error("Error in vkQueuePresentKHR: {}", vk::to_string(result));
            throw std::runtime_error("Error in vkQueuePresentKHR");
        }
//        std::this_thread::sleep_for(std::chrono::milliseconds { 1000 } / 30);

        if (new_frame_received[0] || new_frame_received[2]) { // TODO: Hide frame dumping behind setting
            (void)device->waitForFences({*current_frame->render_finished_fence}, true, std::numeric_limits<uint64_t>::max());
            device->waitIdle();

            auto data = reinterpret_cast<char*>(device->mapMemory(*current_frame->framedump_memory, 0, 400 * 240 * 4 * FrameData::num_screen_ids));


            if (new_frame_received[0]) {
                auto filename = fmt::format("framedump/top/framebuffer_dump_{:05}.top.data", frame_count[0]++);
                std::ofstream file(filename);
                for (unsigned y = 0; y < 240; ++y) {
                    auto ptr = data + 4 * (400 * y/* + x*/);
                    file.write(ptr, 400 * 4);
                }
            }

            if (new_frame_received[2]) {
                auto filename = fmt::format("framedump/bottom/framebuffer_dump_{:05}.bottom.data", frame_count[2]++);
                std::ofstream file(filename);
                for (unsigned y = 0; y < 240; ++y) {
                    auto ptr = data + 400 * 240 * 4 * 2 + 4 * (320 * y/* + x*/);
                    file.write(ptr, 320 * 4);
                }
            }

            device->unmapMemory(*current_frame->framedump_memory);
        }


        if (++current_frame == frame_data.end()) {
            current_frame = frame_data.begin();
        }
    }

    ~SDLVulkanDisplay() override {
        ResetSwapchainResources();
        // All other resources are cleaned up automatically
    }
};
