#pragma once

#include "framework/image_format.hpp"

#include <boost/lockfree/spsc_queue.hpp>

#include <array>

#include <vulkan/vulkan.hpp>

namespace spdlog {
class logger;
}

namespace EmuDisplay {

enum class DataStreamId {
    TopScreenLeftEye, // or top screen image when 3D is disabled
    TopScreenRightEye,
    BottomScreen
};

struct Format {
    uint32_t raw;

    static constexpr std::array<GenericImageFormat, 5> format_map = {{
        GenericImageFormat::RGBA8,
        GenericImageFormat::RGB8,
        GenericImageFormat::RGB565,
        GenericImageFormat::RGBA5551,
        GenericImageFormat::RGBA4
    }};
};

struct Frame {
    vk::UniqueImage image;
    vk::UniqueImageView image_view;
    vk::UniqueSampler image_sampler;
    vk::UniqueFramebuffer framebuffer;
    vk::UniqueDeviceMemory buffer_memory;
    vk::DescriptorSet desc_set; // Descriptor sets for source image copied onto "image"

    vk::UniqueCommandBuffer command_buffer;
//    vk::UniqueSemaphore buffer_ready_semaphore;

    uint64_t timestamp;
};

// TODO: Add custom timestamp type that is robust against overflows when comparing

struct EmuDisplay {
    struct ForceUnwind {};

    EmuDisplay(spdlog::logger&, vk::PhysicalDevice, vk::Device, uint32_t graphics_queue_index);

    virtual ~EmuDisplay();

    // TODO: Drop need for this interface
    void ClearResources() {
        image_blitter[0] = {};
        image_blitter[1] = {};
        image_blitter[2] = {};

        screen_parts[0][0] = {};
        screen_parts[0][1] = {};
        screen_parts[1][0] = {};
        screen_parts[1][1] = {};
        screen_parts[2][0] = {};
        screen_parts[2][1] = {};

        desc_pool = {};
        command_pool = {};
    }

    vk::UniqueCommandPool command_pool;
    vk::UniqueDescriptorPool desc_pool;

    // One for each stream
    struct ImageBlitter {
        vk::UniqueRenderPass renderpass {};
        vk::UniquePipelineLayout pipeline_layout {};
        vk::UniquePipeline pipeline {};
        vk::UniqueDescriptorSetLayout tex_binding {};
        vk::UniqueShaderModule vertex_shader {};
        vk::UniqueShaderModule fragment_shader {};

        static constexpr vk::Format target_format = vk::Format::eR8G8B8A8Unorm;
    } image_blitter[3];

    static constexpr int frames_in_flight = 2;

    // For each stream, store 2 images
    std::array<std::array<Frame, frames_in_flight>, 3> screen_parts;

    // Frames ready to be re-filled by renderer
    std::array<boost::lockfree::spsc_queue<Frame*, boost::lockfree::capacity<frames_in_flight>>, 3> retired_frames;

    // Frames ready to be used by consumer (to be moved to ready_frames)
    std::array<boost::lockfree::spsc_queue<Frame*, boost::lockfree::capacity<frames_in_flight>>, 3> incoming_frames;

    // Frames ready for use by consumer, sorted by increasing timestamp
    std::array<std::vector<Frame*>, 3> ready_frames;

    // Set to true by consumer on exit. Renderer should check for this while waiting for retired frames to avoid deadlocks
    // TODO: Add public interface for this, ideally a RAII based client class
    std::atomic<bool> exit_requested = false;

    // Renderer interface

    Frame& PrepareImage(DataStreamId stream_id, uint64_t timestamp);

    void PushImage(DataStreamId stream_id, Frame& frame, uint64_t timestamp);

    // Consumer interface

    /**
     * Update the current frame set selection for the given target timestamp.
     * This is the set of two top screen images and one bottom screen image
     * that the frontend should display in the next host frame.
     *
     * The ideal frame set is characterized by a number of criteria:
     * - The timestamp of each image should be close to the target timestamp
     * - Rendered screen images should not be displayed out-of-sync, so the timestamps of the images should match up closely among themselves
     * - Each screen image should be displayed at least once
     * - The host display should update at a regular interval even if emulation speed fluctuates slightly
     *
     * This function uses heuristics to balance these criteria against each
     * other. Frames that won't be used anymore are retired, which allows them
     * to be used for new images by the renderer.
     */
    void SeekTo(uint64_t timestamp);

    /**
     * Update the current frame set selection for the given target timestamp.
     *
     * This frame is selected based on internal heuristics (see SeekTo for
     * details).
     */
    Frame& GetCurrent(DataStreamId stream_id);
};

} // namespace EmuDisplay
