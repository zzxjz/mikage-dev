#pragma once

#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Profiler {
class Activity;
}

namespace Pica {

struct Regs;

namespace Vulkan {

struct LinkedRenderTargetResource;

struct PipelineStateHash {
    // TODO: vertex_binding_descs, vertex_attribute_descs

    vk::ShaderModule shaders[2];

    uint64_t vertex_input_mapping;

    uint32_t vertex_input_num_mappings;

    uint8_t flags;
    uint8_t triangle_topology;
    uint16_t pad;

    uint32_t viewport_data[5]; // TODO: Could compactify this into 3 uint32_ts

    uint32_t blend_config;

    uint32_t blend_constant;

    uint32_t scissor;

    uint32_t depthstencil[2];

    bool operator==(const PipelineStateHash& other) const {
        return (0 == memcmp(this, &other, sizeof(*this)));
    }
};
static_assert(sizeof(PipelineStateHash) == 72);

struct PipelineStateHashHasher {
    size_t operator()(const PipelineStateHash& data) const noexcept;
};

class PipelineCache {
    vk::Device device;

    std::unordered_map<PipelineStateHash, vk::UniquePipeline, PipelineStateHashHasher> cache;

public:
    PipelineCache(vk::Device device);
    ~PipelineCache();

    vk::Pipeline Lookup(Profiler::Activity&, LinkedRenderTargetResource&, vk::PipelineLayout, vk::ShaderModule vertex_shader,
                        vk::ShaderModule pixel_shader, vk::PipelineVertexInputStateCreateInfo&, Regs&);

    // Creates an uncached pipeline object for fullscreen effects
    vk::UniquePipeline FullscreenEffect(uint32_t fb_width, uint32_t fb_height, vk::RenderPass renderpass, vk::PipelineLayout layout,
                                        vk::ShaderModule vertex_shader, vk::ShaderModule pixel_shader);
};

} // namespace Vulkan

} // namespace Pica
