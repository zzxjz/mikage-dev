#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1

#include "display.hpp"

#include "framework/meta_tools.hpp"

#include <vulkan_utils/debug_markers.hpp>
#include <vulkan_utils/glsl_helper.hpp>
#include <vulkan_utils/memory_types.hpp>

#include <spdlog/logger.h>

#include <thread>

namespace EmuDisplay {

static vk::UniquePipeline CreateGraphicsPipelineForFullscreenEffect(
        uint32_t fb_width, uint32_t fb_height, vk::Device device, vk::RenderPass renderpass,
        vk::PipelineLayout layout, vk::ShaderModule vertex_shader, vk::ShaderModule pixel_shader) {
    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_infos = {{
        { vk::PipelineShaderStageCreateFlags { }, vk::ShaderStageFlagBits::eVertex, vertex_shader, "main" },
        { vk::PipelineShaderStageCreateFlags { }, vk::ShaderStageFlagBits::eFragment, pixel_shader, "main" }
    }};

    vk::PipelineInputAssemblyStateCreateInfo input_assembly_info { vk::PipelineInputAssemblyStateCreateFlags { }, vk::PrimitiveTopology::eTriangleStrip, false };

    auto viewport = vk::Viewport { 0.f, 0.f, static_cast<float>(fb_width), static_cast<float>(fb_height), 0.f, 1.f };

    vk::Rect2D scissor { vk::Offset2D { 0, 0 }, vk::Extent2D { fb_width, fb_height } };
    vk::PipelineViewportStateCreateInfo viewport_info { vk::PipelineViewportStateCreateFlags { }, 1, &viewport, 1, &scissor };

    vk::PipelineRasterizationStateCreateInfo rasterization_info {   vk::PipelineRasterizationStateCreateFlags { },
                                                                    false, // depth clamp enable
                                                                    false, // rasterizer discard enable
                                                                    vk::PolygonMode::eFill,
                                                                    vk::CullModeFlagBits::eNone,
                                                                    vk::FrontFace::eClockwise,
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

    vk::PipelineColorBlendAttachmentState attachment_blend_state {
        false, {}, {}, {}, {}, {}, {}, // No blending enabled
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
    };

    vk::PipelineDepthStencilStateCreateInfo depth_stencil_info { };

    vk::PipelineColorBlendStateCreateInfo blend_info {  vk::PipelineColorBlendStateCreateFlags { },
                                                        false, {}, // logic op
                                                        1, // attachment count
                                                        &attachment_blend_state
                                                     };


    vk::PipelineVertexInputStateCreateInfo vertex_input_info { };

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
                                            renderpass,
                                            0
                                        };
    return device.createGraphicsPipelineUnique(vk::PipelineCache { }, info).value;
}

EmuDisplay::EmuDisplay(spdlog::logger& logger, vk::PhysicalDevice physical_device, vk::Device device, uint32_t graphics_queue_index) {
    for (auto stream_id : { DataStreamId::TopScreenLeftEye, DataStreamId::TopScreenRightEye, DataStreamId::BottomScreen }) {
        auto& screen_part = screen_parts[Meta::to_underlying(stream_id)];
        retired_frames[Meta::to_underlying(stream_id)].push(&screen_part[0]);
        retired_frames[Meta::to_underlying(stream_id)].push(&screen_part[1]);
    }

    vk::CommandPoolCreateInfo command_pool_info { vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer, graphics_queue_index };
    command_pool = device.createCommandPoolUnique(command_pool_info);

    vk::DescriptorPoolSize pool_size {
                        vk::DescriptorType::eCombinedImageSampler,
                        static_cast<uint32_t>(screen_parts.size() * std::size(image_blitter)) // descriptor count
    };
    vk::DescriptorPoolCreateInfo pool_info {
        {},
        static_cast<uint32_t>(screen_parts.size() * std::size(image_blitter)), // number of sets
        1, // number of pool sizes
        &pool_size
    };
    desc_pool = device.createDescriptorPoolUnique(pool_info);

    for (auto& stream_id : { DataStreamId::TopScreenLeftEye, DataStreamId::TopScreenRightEye, DataStreamId::BottomScreen }) {
        auto& image_blitter = this->image_blitter[Meta::to_underlying(stream_id)];

        {
            std::vector<vk::AttachmentDescription> attachment_desc = {{
                vk::AttachmentDescriptionFlags { },
                image_blitter.target_format,
                vk::SampleCountFlagBits::e1,
                vk::AttachmentLoadOp::eDontCare,
                vk::AttachmentStoreOp::eStore,
                vk::AttachmentLoadOp::eDontCare,
                vk::AttachmentStoreOp::eDontCare,
                vk::ImageLayout::eUndefined,
                vk::ImageLayout::eGeneral
            }};

            vk::AttachmentReference attachment_ref { 0, vk::ImageLayout::eGeneral };
            vk::SubpassDescription subpass_desc {
                vk::SubpassDescriptionFlags { },
                vk::PipelineBindPoint::eGraphics,
                0, nullptr, // Input attachments
                1, &attachment_ref, // color attachments
                nullptr, // resolve attachments
                nullptr, // depth stencil attachment
                0, nullptr // preserve attachments
            };
            vk::RenderPassCreateInfo renderpass_info { vk::RenderPassCreateFlags { }, static_cast<uint32_t>(attachment_desc.size()), attachment_desc.data(), 1, &subpass_desc, 0, nullptr };
            image_blitter.renderpass = device.createRenderPassUnique(renderpass_info);
        }

        {
            vk::DescriptorSetLayoutBinding binding {
                0,
                vk::DescriptorType::eCombinedImageSampler,
                1,
                vk::ShaderStageFlagBits::eFragment,
                nullptr
            };

            vk::DescriptorSetLayoutCreateInfo info {
                vk::DescriptorSetLayoutCreateFlags { },
                1, &binding
            };
            image_blitter.tex_binding = device.createDescriptorSetLayoutUnique(info);

            vk::PipelineLayoutCreateInfo layout_info {
                vk::PipelineLayoutCreateFlags { },
                1, &*image_blitter.tex_binding, // descriptor set layouts
                0, nullptr, // push constant ranges
            };
            image_blitter.pipeline_layout = device.createPipelineLayoutUnique(layout_info);
        }

        uint32_t width = (stream_id == DataStreamId::BottomScreen) ? 320 : 400;
        uint32_t height = 240;

        auto& screen_frames = screen_parts[Meta::to_underlying(stream_id)];
        for (size_t buffer_index = 0; buffer_index < screen_frames.size(); ++buffer_index) {
            auto& image = screen_frames[buffer_index].image;

            vk::ImageCreateInfo info {
                vk::ImageCreateFlags { },
                vk::ImageType::e2D,
                image_blitter.target_format, // TODO: Check format support. Any 8-bit RGB format will suffice
                vk::Extent3D { width, height, 1 },
                1,
                1,
                vk::SampleCountFlagBits::e1,
                vk::ImageTiling::eOptimal,
                vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eColorAttachment,
                vk::SharingMode::eExclusive, // TODO: Probably should be sharing this?
                0, nullptr, // queue families
                vk::ImageLayout::eUndefined
            };
            image = device.createImageUnique(info);
            Pica::Vulkan::SetDebugName(device, image, fmt::format("EmuDisplay image {} for screen {}", buffer_index, static_cast<int>(stream_id)).c_str());
            auto requirements = device.getImageMemoryRequirements(*image);

            auto properties = physical_device.getMemoryProperties();
            for (size_t type_index = 0; type_index < properties.memoryTypeCount; ++type_index) {
                auto& type = properties.memoryTypes[type_index];
                auto& heap = properties.memoryHeaps[type.heapIndex];
                logger.info("Memory type {}: {}, using heap {} ({} bytes, {})", type_index, vk::to_string(type.propertyFlags), type.heapIndex, heap.size, vk::to_string(heap.flags));
            }

            // TODO: Review which flags are needed
            // TODO: Observed sporadic crashes on startup when using this without eHostCoherent
            Pica::Vulkan::MemoryTypeDatabase memory_types { logger, physical_device };
            auto memory_info = memory_types.GetMemoryAllocateInfo<vk::MemoryPropertyFlagBits::eDeviceLocal>(requirements);
            screen_frames[buffer_index].buffer_memory = device.allocateMemoryUnique(memory_info);

            device.bindImageMemory(*image, *screen_frames[buffer_index].buffer_memory, 0);



            vk::ImageViewCreateInfo image_view_info {
                vk::ImageViewCreateFlags { },
                *image,
                vk::ImageViewType::e2D,
                image_blitter.target_format,
                vk::ComponentMapping { },
                vk::ImageSubresourceRange { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 }
            };
            screen_frames[buffer_index].image_view = device.createImageViewUnique(image_view_info);

            vk::SamplerCreateInfo sampler_info {
                vk::SamplerCreateFlags { },
                vk::Filter::eNearest,
                vk::Filter::eNearest,
                vk::SamplerMipmapMode::eNearest,
                vk::SamplerAddressMode::eRepeat,
                vk::SamplerAddressMode::eRepeat,
                vk::SamplerAddressMode::eRepeat,
                0.f, // LOD bias
                false, // anisotropic filter
                0.f, // anisotropic filter
                false, // compare enable
                vk::CompareOp::eNever,
                0.f, // min LOD
                0.f, // max LOD
                vk::BorderColor::eFloatTransparentBlack,
                false // unnormalized coordinates
            };
            screen_frames[buffer_index].image_sampler = device.createSamplerUnique(sampler_info);

            vk::FramebufferCreateInfo fb_info { vk::FramebufferCreateFlags { }, *image_blitter.renderpass, 1, &*screen_frames[buffer_index].image_view, width, height, 1 };
            screen_frames[buffer_index].framebuffer = device.createFramebufferUnique(fb_info);
            {
                vk::DescriptorSetAllocateInfo desc_set_info {
                    *desc_pool,
                    1,
                    &*image_blitter.tex_binding
                };
                screen_frames[buffer_index].desc_set = std::move(device.allocateDescriptorSets(desc_set_info)[0]);
            }
        }

        {
            const char* code =  "#version 450\n"
                                "#extension GL_ARB_separate_shader_objects : enable\n"
                                "\n"
                                "void main() {"
                                "  if (gl_VertexIndex == 0) {\n"
                                "    gl_Position = vec4(-1.0, -1.0, 0.0, 1.0);\n"
                                "  } else if (gl_VertexIndex == 1) {\n"
                                "    gl_Position = vec4(-1.0, 1.0, 0.0, 1.0);\n"
                                "  } else if (gl_VertexIndex == 2) {\n"
                                "    gl_Position = vec4(1.0, -1.0, 0.0, 1.0);\n"
                                "  } else if (gl_VertexIndex == 3) {\n"
                                "    gl_Position = vec4(1.0, 1.0, 0.0, 1.0);\n"
                                "  }\n"
                                "}\n";
            auto shader_spv = CompileShader(EShLangVertex, code, "main");

            vk::ShaderModuleCreateInfo shader_module_info {
                vk::ShaderModuleCreateFlags { }, shader_spv.size() * sizeof(shader_spv.data()[0]), shader_spv.data()
            };
            image_blitter.vertex_shader = device.createShaderModuleUnique(shader_module_info);
        }

        {
            std::string code =  "#version 450\n"
                                "#extension GL_ARB_separate_shader_objects : enable\n"
                                "\n"
                                "layout(binding = 0) uniform sampler2D sampler0;\n"
                                "layout(location = 0) out vec4 out_color;\n"
                                "\n"
                                "void main() {\n"
                                // NOTE: Alpha channel is set to 1.0 here because the Qt frontend requires this for overlay effects (blur, ...) to work
                                "  out_color = vec4(texture(sampler0, -gl_FragCoord.yx * vec2(" + fmt::format("{}", 1.0 / height) + ", " + fmt::format("{}", 1.0 / width) + ")).rgb, 1.0);\n"
                                "}\n";
            auto shader_spv = CompileShader(EShLangFragment, code.c_str(), "main");

            vk::ShaderModuleCreateInfo shader_module_info {
                vk::ShaderModuleCreateFlags { }, shader_spv.size() * sizeof(shader_spv.data()[0]), shader_spv.data()
            };
            image_blitter.fragment_shader = device.createShaderModuleUnique(shader_module_info);
        }

        image_blitter.pipeline = CreateGraphicsPipelineForFullscreenEffect(width, height,
                                                                        device,
                                                                       *image_blitter.renderpass,
                                                                       *image_blitter.pipeline_layout,
                                                                       *image_blitter.vertex_shader,
                                                                       *image_blitter.fragment_shader);
    }
}

EmuDisplay::~EmuDisplay() = default;

Frame& EmuDisplay::PrepareImage(DataStreamId stream_id, uint64_t timestamp) {
    Frame* frame = nullptr;
    while (retired_frames[Meta::to_underlying(stream_id)].pop(&frame, 1) != 1) {
        if (exit_requested) {
            fprintf(stderr, "Received exit notification from EmuDisplay in PrepareImage, unwinding...\n");
            throw ForceUnwind{};
        }

        // TODO: Use a condition variable instead?
//        fprintf(stderr, "Waiting for next retired frame... %lu\n", timestamp);
        std::this_thread::yield();
    }
    return *frame;
}

void EmuDisplay::PushImage(DataStreamId stream_id, Frame& frame, uint64_t timestamp) {
    frame.timestamp = timestamp;
//    fprintf(stderr, "Pushing frame: %lu\n", timestamp);
    auto pushed = incoming_frames[Meta::to_underlying(stream_id)].push(&frame);
    if (!pushed) {
        if (exit_requested) {
            fprintf(stderr, "Received exit notification from EmuDisplay in PushImage, unwinding...\n");
            throw ForceUnwind{};
        }
        throw std::runtime_error("QUEUE FULL");
    }
}


void EmuDisplay::SeekTo(uint64_t timestamp) {
//    fprintf(stderr, "Seeking to %lu\n", timestamp);
    for (auto stream_id : { DataStreamId::TopScreenLeftEye, DataStreamId::TopScreenRightEye, DataStreamId::BottomScreen }) {
        // Pop entries from incoming_frames to ready_frames
        auto& incoming = incoming_frames[Meta::to_underlying(stream_id)];
        auto& ready = ready_frames[Meta::to_underlying(stream_id)];
        while (incoming.read_available()) {
            ready.push_back(incoming.front());
            incoming.pop();
        }
//        fprintf(stderr, "Ready queue elements: %d\n", (int)ready.size());

        // Scenarios to handle:
        // * Emulator running too slow => seek time is always newer than timestamps
        //   * Images should display as soon as possible, if need be separately from each other
        // * Emulator running too fast, with speed limit => seek time is always behind timestamps
        //   * Each image should be displayed at least once
        // * Emulator running too fast, no speed limit => seek time is always behind timestamps
        //   * Skip images

        for (auto frame_it = ready.begin(); frame_it != ready.end();) {
            Frame* frame = *frame_it;
            // Drop outdated frames if newer ones are queued already
            if (frame->timestamp < timestamp && ready.size() > 1) {
                fprintf(stderr, "Discarding frame %lu\n", (**frame_it).timestamp);
                frame_it = ready.erase(frame_it);
                retired_frames[Meta::to_underlying(stream_id)].push(frame);
                continue;
            } else if (frame->timestamp < timestamp + 1000 && ready.size() == 1) {
                // TODO: Consider this stream inactive, since no new frame has been pushed in a while
            }

            ++frame_it;
        }
    }
}

Frame& EmuDisplay::GetCurrent(DataStreamId stream_id) {
    return *ready_frames[Meta::to_underlying(stream_id)].front();
}

} // namespace Display
