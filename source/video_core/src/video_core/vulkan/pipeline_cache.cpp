#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1

#include "resource_manager.hpp"
#include "pipeline_cache.hpp"
#include "state_mapping.hpp"

#include <platform/gpu/pica.hpp>

#include <framework/exceptions.hpp>
#include <framework/profiler.hpp>

#include <boost/container_hash/hash.hpp>

namespace Pica {

namespace Vulkan {

std::size_t PipelineStateHashHasher::operator()(const PipelineStateHash& data) const noexcept {
   auto* ptr = reinterpret_cast<const uint32_t*>(&data);
   return boost::hash_range(ptr, ptr + sizeof(data) / sizeof(uint32_t));
}

PipelineCache::PipelineCache(vk::Device device) : device(device) {
}

PipelineCache::~PipelineCache() = default;

static PipelineStateHash last {}; // TODO

extern bool preassembled_triangles;

vk::Pipeline PipelineCache::Lookup(Profiler::Activity& activity, LinkedRenderTargetResource& target, vk::PipelineLayout layout,
                                   vk::ShaderModule vertex_shader, vk::ShaderModule pixel_shader,
                                   vk::PipelineVertexInputStateCreateInfo& vertex_input_info, Regs& registers) {
    PipelineStateHash pipeline_state_hash {};

    pipeline_state_hash.viewport_data[0] = registers.viewport_size_x;
    pipeline_state_hash.viewport_data[1] = registers.viewport_size_y;
    pipeline_state_hash.viewport_data[2] = registers.viewport_depth_range;
    pipeline_state_hash.viewport_data[3] = registers.viewport_depth_far_plane;
    pipeline_state_hash.viewport_data[4] = registers.viewport_corner.raw;

    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_infos = {{
        { vk::PipelineShaderStageCreateFlags { }, vk::ShaderStageFlagBits::eVertex, vertex_shader, "main" },
        { vk::PipelineShaderStageCreateFlags { }, vk::ShaderStageFlagBits::eFragment, pixel_shader, "main" }
    }};

    pipeline_state_hash.shaders[0] = shader_infos[0].module;
    pipeline_state_hash.shaders[1] = shader_infos[1].module;

    pipeline_state_hash.vertex_input_mapping = registers.vs_input_register_map.low | (registers.vs_input_register_map.high << 32);
    if (registers.max_shader_input_attribute_index() != 15) {
        // Only hash the used attribute mappings
        uint64_t shift = (4 * registers.max_shader_input_attribute_index());
        uint64_t mask = ((uint64_t { 1 } << shift) - 1);
        pipeline_state_hash.vertex_input_mapping &= mask;
    }
    pipeline_state_hash.vertex_input_num_mappings = registers.max_shader_input_attribute_index() + 1;

    // TODO: Properly hash this...
    pipeline_state_hash.vertex_input_mapping ^= std::hash<uint32_t>{} ( vertex_input_info.vertexBindingDescriptionCount);
    pipeline_state_hash.vertex_input_mapping ^= std::hash<uint32_t>{} ( vertex_input_info.vertexAttributeDescriptionCount);
    pipeline_state_hash.vertex_input_mapping ^= boost::hash_range(  (char*)vertex_input_info.pVertexBindingDescriptions,
                                                                    (char*)vertex_input_info.pVertexBindingDescriptions + sizeof(vertex_input_info.pVertexBindingDescriptions[0]) * vertex_input_info.vertexBindingDescriptionCount);
    pipeline_state_hash.vertex_input_mapping ^= boost::hash_range(  (char*)vertex_input_info.pVertexAttributeDescriptions,
                                                                    (char*)vertex_input_info.pVertexAttributeDescriptions + sizeof(vertex_input_info.pVertexAttributeDescriptions[0]) * vertex_input_info.vertexAttributeDescriptionCount);

    auto& output_merger_regs = registers.output_merger;

    const bool depth_stencil_enabled = registers.framebuffer.depth_stencil_read_enabled();

    pipeline_state_hash.flags = static_cast<uint32_t>(registers.cull_mode.Value()); // 2 bits
    pipeline_state_hash.flags |= (output_merger_regs.alphablend_enable) << 2; // 1 bit
    if (depth_stencil_enabled) {
        pipeline_state_hash.flags |= output_merger_regs.depth_test_enable << 3; // 1 bit
        pipeline_state_hash.flags |= (registers.framebuffer.depth_stencil_write_enabled() && output_merger_regs.depth_write_enable) << 4; // 1 bit
        pipeline_state_hash.flags |= static_cast<uint32_t>(output_merger_regs.depth_test_func.Value()) << 5; // 3 bit
    }
    pipeline_state_hash.triangle_topology = Meta::to_underlying(preassembled_triangles ? Regs::TriangleTopology::List : registers.triangle_topology.Value()); // 2 bit

    if (output_merger_regs.alphablend_enable) {
        pipeline_state_hash.blend_config = output_merger_regs.alpha_blending.raw;
        pipeline_state_hash.blend_constant = output_merger_regs.blend_constant.storage;
    } else {
        pipeline_state_hash.blend_config = static_cast<uint32_t>(output_merger_regs.logic_op.op.Value());
    }

    if ((target.color_resource.info.extent.width | target.color_resource.info.extent.height) >= (1 << 16)) {
        throw std::runtime_error("Unexpectedly large framebuffer");
    }
    pipeline_state_hash.scissor = (target.color_resource.info.extent.width << 16) | target.color_resource.info.extent.height;
    // TODO: Render pass information

    const bool enable_stencil = registers.framebuffer.HasStencilBuffer() && output_merger_regs.stencil_test.enabled()();
    if (enable_stencil && !depth_stencil_enabled) {
        throw Mikage::Exceptions::NotImplemented("Tried to enable stencil without read-access enabled");
    }
    if (output_merger_regs.stencil_test.enabled() && !registers.framebuffer.HasStencilBuffer()) {
        throw std::runtime_error("Stencil test enabled but current framebuffer configuration has no stencil channel");
    }

    if (depth_stencil_enabled && output_merger_regs.stencil_test.enabled()() && !output_merger_regs.depth_test_enable) {
        // See Citra PR 8e6336d96bf7ee0e31ca51064c85f1b65913fe7a
        // SM3DL hits this on the title screen
//        throw std::runtime_error("Must check if we properly emulate this");
    }

    auto stencil_op_state = enable_stencil ? FromPicaState(output_merger_regs.stencil_test) : vk::StencilOpState { };
    // TODO: Factor in framebuffer.framebuffer.allow_depth_stencil_write !
    bool write_only = !output_merger_regs.depth_test_enable && registers.framebuffer.depth_stencil_write_enabled() && output_merger_regs.depth_write_enable;
    vk::PipelineDepthStencilStateCreateInfo depth_stencil_info {
        vk::PipelineDepthStencilStateCreateFlags { },
        depth_stencil_enabled && (output_merger_regs.depth_test_enable || write_only),
        depth_stencil_enabled && registers.framebuffer.depth_stencil_write_enabled() && output_merger_regs.depth_write_enable, // TODO: Also should check the "allow depth write" flag, see Citra PR 1624
        (depth_stencil_enabled && !write_only) ? FromPicaState(output_merger_regs.depth_test_func) : vk::CompareOp::eAlways,
        false, // Depth bounds test enable
        enable_stencil, // Stencil test enable
        stencil_op_state, // Front stencil op
        stencil_op_state, // Back stencil op (same as front stencil op; 3DS does not support two-sided stencil)
        0.f, // Min depth bounds
        0.f  // Max depth bounds
    };
    pipeline_state_hash.depthstencil[1] = depth_stencil_info.depthTestEnable << 12;
    if (depth_stencil_info.depthTestEnable) {
        pipeline_state_hash.depthstencil[1] |= depth_stencil_info.depthWriteEnable << 13;
        pipeline_state_hash.depthstencil[1] |= Meta::to_underlying(depth_stencil_info.depthCompareOp) << 14;
    }

    pipeline_state_hash.depthstencil[0] = enable_stencil;
    if (enable_stencil) {
        auto& stencil = output_merger_regs.stencil_test;
        pipeline_state_hash.depthstencil[0] |= static_cast<uint32_t>(stencil.compare_function()()) << 4;
        pipeline_state_hash.depthstencil[0] |= stencil.mask_out()() << 8;
        pipeline_state_hash.depthstencil[0] |= stencil.reference()() << 16;
        pipeline_state_hash.depthstencil[0] |= stencil.mask_in()() << 24;

        pipeline_state_hash.depthstencil[1] |= static_cast<uint32_t>(stencil.op_fail_stencil()());
        pipeline_state_hash.depthstencil[1] |= static_cast<uint32_t>(stencil.op_pass_stencil_fail_depth()()) << 4;
        pipeline_state_hash.depthstencil[1] |= static_cast<uint32_t>(stencil.op_pass_both()()) << 8;
    }

    auto existing_pipeline = cache.find(pipeline_state_hash);
    if (existing_pipeline != cache.end()) {
        return *existing_pipeline->second;
    }

    // TODO: Hash renderpass handle?

    activity.GetSubActivity("StateMapper").Resume();

    // TODO: Use vk::PrimitiveTopology::eTriangleList unless hardware-PA is enabled
    vk::PipelineInputAssemblyStateCreateInfo input_assembly_info {
        vk::PipelineInputAssemblyStateCreateFlags { },
        FromPicaState(preassembled_triangles ? Regs::TriangleTopology::List : registers.triangle_topology.Value()),
        false
    };

    auto viewport = FromPicaViewport(   registers.viewport_corner.x, registers.viewport_corner.y,
                                        float24::FromRawFloat(registers.viewport_size_x),
                                        float24::FromRawFloat(registers.viewport_size_y),
                                        float24::FromRawFloat(registers.viewport_depth_range),
                                        float24::FromRawFloat(registers.viewport_depth_far_plane));
    if (viewport.x + viewport.width > target.color_resource.info.extent.width ||
        viewport.y + viewport.height > target.color_resource.info.extent.height) {
        throw std::runtime_error(   fmt::format("Invalid viewport dimensions ({},{},{},{}): Exceeding framebuffer size {}x{}",
                                    viewport.x, viewport.y, viewport.width, viewport.height, target.color_resource.info.extent.width, target.color_resource.info.extent.height));
    }

    // TODO: Use register state instead
    // TODO: PLEASE DO THIS
    vk::Rect2D scissor { vk::Offset2D { 0, 0 }, vk::Extent2D { target.color_resource.info.extent.width, target.color_resource.info.extent.height } };
    vk::PipelineViewportStateCreateInfo viewport_info { vk::PipelineViewportStateCreateFlags { }, 1, &viewport, 1, &scissor };

    vk::PipelineRasterizationStateCreateInfo rasterization_info {   vk::PipelineRasterizationStateCreateFlags { },
                                                                    false, // depth clamp enable
                                                                    false, // rasterizer discard enable
                                                                    vk::PolygonMode::eFill,
                                                                    FromPicaState(registers.cull_mode).first,
                                                                    FromPicaState(registers.cull_mode).second,
                                                                    false, // depth bias + 3 float parameters
                                                                    0.f,
                                                                    0.f,
                                                                    0.f,
                                                                    1.f // line width
                                                                };

    vk::PipelineMultisampleStateCreateInfo multisample_info {   vk::PipelineMultisampleStateCreateFlags { },
                                                                vk::SampleCountFlagBits::e1,
                                                                false,
                                                                1.0f
                                                            };

    vk::ColorComponentFlags active_color_components {};
    if (registers.framebuffer.color_write_enabled()) {
        if (output_merger_regs.color_write_enable_r) {
            active_color_components |= vk::ColorComponentFlagBits::eR;
        }
        if (output_merger_regs.color_write_enable_g) {
            active_color_components |= vk::ColorComponentFlagBits::eG;
        }
        if (output_merger_regs.color_write_enable_b) {
            active_color_components |= vk::ColorComponentFlagBits::eB;
        }
        if (output_merger_regs.color_write_enable_a) {
            active_color_components |= vk::ColorComponentFlagBits::eA;
        }
    }
    vk::PipelineColorBlendAttachmentState attachment_blend_state {
        output_merger_regs.alphablend_enable,
        {}, {}, {}, {}, {}, {}, // Blending parameters set up below if needed
        active_color_components
    };
    if (attachment_blend_state.blendEnable) {
        // NOTE: These FromPicaState calls are conditional to prevent passing uninitialized Pica register values to it
        attachment_blend_state.srcColorBlendFactor = FromPicaState(output_merger_regs.alpha_blending.factor_source_rgb);
        attachment_blend_state.dstColorBlendFactor = FromPicaState(output_merger_regs.alpha_blending.factor_dest_rgb);
        attachment_blend_state.colorBlendOp = FromPicaState(output_merger_regs.alpha_blending.blend_equation_rgb);
        attachment_blend_state.srcAlphaBlendFactor = FromPicaState(output_merger_regs.alpha_blending.factor_source_a);
        attachment_blend_state.dstAlphaBlendFactor = FromPicaState(output_merger_regs.alpha_blending.factor_dest_a);
        attachment_blend_state.alphaBlendOp = FromPicaState(output_merger_regs.alpha_blending.blend_equation_a);
    }

    // TODO: Neither Adreno nor Mali advertise support for logic ops!!!
    // TODO: Check the PhysicalDeviceFeature logicOp at least before trying...
    vk::PipelineColorBlendStateCreateInfo blend_info {  vk::PipelineColorBlendStateCreateFlags { },
                                                        !output_merger_regs.alphablend_enable, // Enable logic op if alpha blending is disabled
                                                        {},
                                                        1, // attachment count
                                                        &attachment_blend_state
                                                     };

    // TODO: Only use these if they're used, otherwise we might pick up garbage values that produce redundant pipeline state hashes!
    blend_info.setBlendConstants({ output_merger_regs.blend_constant.r() / 255.f,
                                   output_merger_regs.blend_constant.g() / 255.f,
                                   output_merger_regs.blend_constant.b() / 255.f,
                                   output_merger_regs.blend_constant.a() / 255.f });

    if (blend_info.logicOpEnable) {
        // NOTE: This FromPicaState calls is conditional to prevent passing uninitialized Pica register values to it
        blend_info.logicOp = FromPicaState(output_merger_regs.logic_op.op);
    }

    // TODO: Dynamically submit the viewport and scissor rect

    vk::GraphicsPipelineCreateInfo info {   vk::PipelineCreateFlags { },
                                            static_cast<uint32_t>(shader_infos.size()),
                                            shader_infos.data(),
                                            &vertex_input_info,
                                            &input_assembly_info,
                                            nullptr,
                                            &viewport_info,
                                            &rasterization_info,
                                            &multisample_info,
                                            &depth_stencil_info,
                                            &blend_info,
                                            nullptr,
                                            layout,
                                            *target.renderpass,
                                            0
                                        };
    activity.GetSubActivity("StateMapper").Interrupt();
    activity.GetSubActivity("Creation").Resume();

    fmt::print("Compiling new pipeline due to state changes:\n");
    if (last.shaders[0] != pipeline_state_hash.shaders[0]) {
        fmt::print("  Vertex shader\n");
    }
    if (last.shaders[1] != pipeline_state_hash.shaders[1]) {
        fmt::print("  Fragment shader\n");
    }
    if (last.vertex_input_mapping != pipeline_state_hash.vertex_input_mapping || last.vertex_input_num_mappings != pipeline_state_hash.vertex_input_num_mappings) {
        fmt::print("  Vertex input mapping: {:#x} -> {:#x}\n", last.vertex_input_mapping, pipeline_state_hash.vertex_input_mapping);
    }
    if (last.flags != pipeline_state_hash.flags) {
        fmt::print("  Flags: {:#x} -> {:#x}\n", last.flags, pipeline_state_hash.flags);
    }

    if (last.flags != pipeline_state_hash.triangle_topology) {
        fmt::print("  Triangle topology: {:#x} -> {:#x}\n", last.triangle_topology, pipeline_state_hash.triangle_topology);
    }

    for (size_t i = 0; i < std::size(last.viewport_data); ++i) {
        if (last.viewport_data[i] != pipeline_state_hash.viewport_data[i]) {
            fmt::print("  Viewport data {}: {:#x} -> {:#x}\n", i, last.viewport_data[i], pipeline_state_hash.viewport_data[i]);
        }
    }

    if (last.blend_config != pipeline_state_hash.blend_config) {
        fmt::print("  Blend config: {:#x} -> {:#x}\n", last.blend_config, pipeline_state_hash.blend_config);
    }

    if (last.blend_constant != pipeline_state_hash.blend_constant) {
        fmt::print("  Blend constant: {:#x} -> {:#x}\n", last.blend_constant, pipeline_state_hash.blend_constant);
    }

    if (last.scissor != pipeline_state_hash.scissor) {
        fmt::print("  Scissor: {:#x} -> {:#x}\n", last.scissor, pipeline_state_hash.scissor);
    }

    // TODO: Stencil

    last = pipeline_state_hash;

    auto [it, inserted] = cache.emplace(pipeline_state_hash, device.createGraphicsPipelineUnique(vk::PipelineCache { }, info).value);
    activity.GetSubActivity("Creation").Interrupt();
    return *it->second;
}

} // namespace Vulkan

} // namespace Pica
