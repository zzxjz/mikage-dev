#include "clipper.h"
#include "context.h"
#include "command_processor.h"
#include "primitive_assembly.h"
#include "renderer.hpp"
#include "shader.hpp"
#include "vertex_loader.hpp"

#include "debug_utils/debug_utils.h"
#include "../interrupt_listener.hpp"

#include <platform/gpu/pica.hpp>

#include <framework/exceptions.hpp>
#include <framework/math_vec.hpp>
#include "framework/profiler.hpp"
#include <framework/settings.hpp>

#include <common/log.h>

#include <range/v3/algorithm/find.hpp>

#include <boost/container/static_vector.hpp>

#include <iostream>
#include <spdlog/logger.h>

#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>

namespace Pica {

namespace Vulkan {
extern bool preassembled_triangles;
}
namespace CommandProcessor {

struct CommandListIterator {
    CommandListIterator(Memory::PhysicalMemory& mem, uint32_t addr, uint32_t size)
        : mem(mem),
          next_address(addr),
          end_address(addr + size) {
        if (size) {
            UpdateCommandBlock();
        }
    }

    void RestartAt(uint32_t addr, uint32_t size) {
        next_address = addr;
        end_address = addr + size;

        if (size) {
            UpdateCommandBlock();
        }
    }

    Memory::PhysicalMemory& mem;
    Memory::HostMemoryBackedPages data;

    uint32_t current_value;
    uint32_t current_command_id = 0xffffffff; // 0xffffffff indicates the end of the command list
    uint32_t start_address;
    uint32_t next_address;
    uint32_t end_address; // exclusive

    struct CommandBlock {
        uint32_t end_address; // unaligned address: There may be 4 bytes of padding between this and the start of the next block
        uint32_t write_mask;
        bool group_commands;
    } current_command_block;

    void UpdateCommandBlock() {
        start_address = next_address;
        ValidateContract(start_address < end_address); // TODO: Turn into assert
        // We don't need write access, but requesting it anyway ensures there are no active hooks of any kind in this range
        data = LookupContiguousMemoryBackedPage<Memory::HookKind::ReadWrite>(mem, start_address, end_address - start_address);
        current_value = Memory::Read<uint32_t>(data, 0);
        const CommandHeader header = { Memory::Read<uint32_t>(data, 4) };
        next_address += 2 * sizeof(uint32_t);

        auto parameter_mask = header.parameter_mask.Value();
        current_command_block.write_mask = (((parameter_mask & 0x1) ? (0xFFu <<  0) : 0u) |
                                            ((parameter_mask & 0x2) ? (0xFFu <<  8) : 0u) |
                                            ((parameter_mask & 0x4) ? (0xFFu << 16) : 0u) |
                                            ((parameter_mask & 0x8) ? (0xFFu << 24) : 0u));
        current_command_id = header.cmd_id;
        current_command_block.group_commands = header.group_commands;
        current_command_block.end_address = next_address + 4 * header.extra_data_length;
    }

    uint32_t Value() const {
        return current_value;
    }

    uint32_t Id() const {
        return current_command_id;
    }

    uint32_t Mask() const {
        return current_command_block.write_mask;
    }

    explicit operator bool() const {
        return current_command_id != 0xffffffff;
    }

    CommandListIterator& operator++() {
        if (next_address == current_command_block.end_address) {
            if (((next_address + 7) & 0xfffffff8) == end_address) {
                current_command_id = 0xffffffff;
            } else {
                // align read pointer to 8 bytes
                next_address = (next_address + 7) & 0xfffffff8;

                UpdateCommandBlock();
            }
        } else {
            if (current_command_block.group_commands) {
                ++current_command_id;
            }

            current_value = Memory::Read<uint32_t>(data, next_address - start_address);
            next_address += 4;
        }
        return *this;
    }

    CommandListIterator operator+(uint32_t values_to_skip) const {
        CommandListIterator ret = *this;
        ret += values_to_skip;
        return ret;
    }

    CommandListIterator& operator+=(uint32_t values_to_skip) {
        while (values_to_skip-- && *this) {
            ++*this;
        }
        return *this;
    }
};

// Detect pipeline configurations known to not actually render anything
static bool IsNopDraw(const Regs& registers) {
    // Reading from the depth buffer if depth access is disabled will always return 0 on hardware (see https://github.com/JayFoxRox/citra/pull/10)
    if (!registers.framebuffer.depth_stencil_read_enabled() && registers.output_merger.depth_test_enable && registers.output_merger.depth_test_func == DepthFunc::LessThan) {
        // Observed e.g. in Captain Toad: Treasure Tracker
        return true;
    }

    return false;
}

static inline void WritePicaReg(Context& context, u32 id, u32 value, u32 mask, CommandListIterator& next_command) {
    auto& registers = context.registers;
    if (id >= registers.NumIds())
        return;

    // TODO: Figure out how register masking acts on e.g. vs_uniform_setup.set_value
    u32 old_value = registers[id];
    registers[id] = (old_value & ~mask) | (value & mask);

    if (g_debug_context)
        g_debug_context->OnEvent(DebugContext::Event::CommandLoaded, reinterpret_cast<void*>(&id));

    DebugUtils::OnPicaRegWrite(id, registers[id]);

    switch(id) {
        case 0x1: case 0x2: case 0x3: case 0x4: case 0x5: case 0x6: case 0x7:
        case 0x8: case 0x9: case 0xa: case 0xb: case 0xc: case 0xd: case 0xe: case 0xf:
        case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
        case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        case 0x31: case 0x32: case 0x33:            case 0x35: case 0x36: case 0x37:
        case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
            throw Mikage::Exceptions::Invalid("Invalid command list entry?");

        // Trigger IRQ
        case PICA_REG_INDEX(trigger_irq):
            if (value != 0x12345678) {
                throw Mikage::Exceptions::NotImplemented("Unexpected interrupt token");
            }
            context.os->NotifyInterrupt(0x2d);
            if (next_command && (next_command + 1)) {
                // NOTE: This is generally placed at the very end of the
                //       command list, but since hardware appears to require
                //       16-byte alignment of command list sizes, one final
                //       command may still be processed after (e.g. in Luigi's
                //       Mansion 2). However if even more commands follow, this
                //       likely indicates a bug.
                throw Mikage::Exceptions::Invalid("End of command list was indicated before it was actually reached");
            }
            break;

        case 0x20:
            // This actually sets the reference value for trigger_irq.
            // No other value has been observed for this, so it's not implemented.
            if (value != 0x12345678) {
                throw Mikage::Exceptions::NotImplemented("Unexpected reference interrupt token");
            }
            break;

        case 0x111:
            // Flush framebuffer
            break;

        case 0x110:
            // Invalidate framebuffer
            break;

        case 0x11c:
        case 0x11d:
            if (old_value == registers[id])
                break;

            // Changed framebuffer location
            printf("TRACE: CHANGED FB LOC\n");
            break;

        case PICA_REG_INDEX(lighting.set_lut_data[0]):
        case PICA_REG_INDEX(lighting.set_lut_data[1]):
        case PICA_REG_INDEX(lighting.set_lut_data[2]):
        case PICA_REG_INDEX(lighting.set_lut_data[3]):
        case PICA_REG_INDEX(lighting.set_lut_data[4]):
        case PICA_REG_INDEX(lighting.set_lut_data[5]):
        case PICA_REG_INDEX(lighting.set_lut_data[6]):
        case PICA_REG_INDEX(lighting.set_lut_data[7]):
        {
            auto is_lut_data_command = [](uint32_t id) {
                return (id >= PICA_REG_INDEX(lighting.set_lut_data[0]) &&
                        id <= PICA_REG_INDEX(lighting.set_lut_data[7]));
            };

            auto lut_selector = Meta::to_underlying(registers.lighting.lut_write_index.table_selector()());
            if (lut_selector >= context.light_lut_data.size()) {
                throw Mikage::Exceptions::Invalid("Tried writing to invalid LUT");
            }
            auto& lut = context.light_lut_data[lut_selector];

            while (true) {
                auto entry_index = registers.lighting.lut_write_index.entry_index()();
                if (entry_index >= lut.size()) {
                    throw Mikage::Exceptions::Invalid("Tried writing outside of light LUT bounds");
                }

                lut[entry_index] = value;
                entry_index = (entry_index + 1) % lut.size(); // TODO: Test that this indeed auto-wraps
                registers.lighting.lut_write_index = registers.lighting.lut_write_index.entry_index().Set(entry_index);

                if (!is_lut_data_command(next_command.Id()) || next_command.Mask() != 0xffffffff) {
                    break;
                }
                value = next_command.Value();
                ++next_command;
            }
            break;
        }

        case PICA_REG_INDEX(triangle_topology):
            // TODO: This will also reset the internal assembler state. Is this desired? Should we keep around any internal state instead?
            context.primitive_assembler.Configure(registers.triangle_topology.Value());
            context.primitive_assembler_new.Configure(registers.triangle_topology.Value());
            break;

        // It seems like these trigger vertex rendering
        case PICA_REG_INDEX(trigger_draw):
        case PICA_REG_INDEX(trigger_draw_indexed):
        {
            if (IsNopDraw(registers)) {
                break;
            }

            if (g_debug_context)
                g_debug_context->OnEvent(DebugContext::Event::IncomingPrimitiveBatch, nullptr);

            const auto& attribute_config = registers.vertex_attributes;

            VertexLoader vertex_loader(context, attribute_config);

            // Load vertices
            const bool is_indexed = (id == PICA_REG_INDEX(trigger_draw_indexed));

            const auto& index_info = registers.index_array;
            const uint32_t index_address = attribute_config.GetPhysicalBaseAddress() + index_info.offset;
            const bool index_u16 = index_info.format != 0;

            const bool render_on_cpu = (context.settings->get<Settings::RendererTag>() == Settings::Renderer::Software);

            ZoneNamedN(TriangleBatch, "Triangle Batch", true);

            struct {
                boost::container::static_vector<uint32_t, 16> vertex_ids;
                boost::container::static_vector<VertexShader::OutputVertex, 16> data;

                size_t offset = 0;
            } vertex_cache;
            vertex_cache.vertex_ids.resize(16, 0xffffffff);
            vertex_cache.data.resize(16);

            auto& vertex_loader_activity = reinterpret_cast<Profiler::TaggedActivity<Profiler::Activities::VertexLoader>&>(context.activity->GetSubActivity("SubmitBatch").GetSubActivity("VertexLoader"));
            vertex_loader_activity->Resume();

            TracyCZoneN(VertexShaderCompile, "Vertex Shader Compile", true);

            VertexShader::InputVertex input {};
            auto& engine = context.shader_engines.GetOrCompile(context, context.settings->get<Settings::ShaderEngineTag>(), registers.vs_main_offset);
            engine.Reset(registers.vs_input_register_map, input, 1 + registers.max_shader_input_attribute_index());
            engine.UpdateUniforms(context.shader_uniforms.f);
            const bool output_vertexes_cacheable = !engine.ProcessesInputVertexesOnGPU();

            TracyCZoneEnd(VertexShaderCompile);

            if (!render_on_cpu) {
                // TODO: Use proper triangle count?
                context.renderer->PrepareTriangleBatch(context, engine, registers.num_vertices, is_indexed);
            }

            TracyCZoneN(VertexLoading, "Vertex Loading", true);

            const bool hardware_vertex_loading = true;
            const bool hardware_primitive_assembly = hardware_vertex_loading || false;

            // TODO: Support indexed rendering in the renderer instead
            // TODO: Don't query a memory backed page if indexing isn't actually enabled
            auto index_memory = Memory::LookupContiguousMemoryBackedPage(*context.mem, index_address, (index_u16 ? 2 : 1) * registers.num_vertices);
            // TODO: Some games trigger draws with "num_vertices == 1". What's the purpose of this? (Happens e.g. during startup of Steel Diver: Sub Wars)
            // NOTE: Index 0xffff has no special behavior (no primitive restart or similar; https://github.com/citra-emu/citra/pull/3714)
            for (unsigned int index = 0; !hardware_vertex_loading && index < registers.num_vertices; ++index)
            {
                // TODO: Should vertex_offset indeed only be applied We had only applied for non-indexed rendering?
                unsigned int vertex =
                    is_indexed ? (index_u16 ? Memory::Read<uint16_t>(index_memory, 2 * index) : Memory::Read<uint8_t>(index_memory, index))
                               : (index + registers.vertex_offset);

                VertexShader::OutputVertex* cached_vertex = nullptr;
                VertexShader::OutputVertex noncached_vertex;
                if (output_vertexes_cacheable) {
                    if (is_indexed && index > 2) {
                        // Lookup vertex data in cache (skipping the first three vertices since these are unlikely to be in the cache)
                        auto it = std::find(vertex_cache.vertex_ids.begin(), vertex_cache.vertex_ids.end(), vertex);
                        if (it != vertex_cache.vertex_ids.end()) {
                            cached_vertex = &vertex_cache.data[std::distance(vertex_cache.vertex_ids.begin(), it)];
    //                         printf("TRACE: Vertex cache HIT\n");
                        } else {
    //                         printf("TRACE: Vertex cache miss\n");
                        }
                    }

                    if (!cached_vertex) {
                        // Initialize data for the current vertex
                        vertex_loader.Load(input, context, attribute_config, vertex);
                        if (g_debug_context)
                            g_debug_context->OnEvent(DebugContext::Event::VertexLoaded, (void*)&input);

                        // Send to vertex shader
                        noncached_vertex = engine.Run(context, registers.vs_main_offset);
                        cached_vertex = &noncached_vertex;
                        if (is_indexed) {
                            vertex_cache.vertex_ids[vertex_cache.offset] = vertex;
                            vertex_cache.data[vertex_cache.offset++] = *cached_vertex;
                            vertex_cache.offset = vertex_cache.offset % vertex_cache.data.size();
                        }
                    }
                }

                // Send to triangle clipper
                if (render_on_cpu) {
                    // TODO: Test if this still works
                    context.primitive_assembler.SubmitVertex(context, *cached_vertex, Clipper::ProcessTriangle);
                } else {
                    if (output_vertexes_cacheable) {
                        context.primitive_assembler.SubmitVertex(context, *cached_vertex, [&](auto& context, auto& v1, auto& v2, auto& v3) { return context.renderer->AddPreassembledTriangle(context, v1, v2, v3); });
                    } else if (!hardware_primitive_assembly) {
                        vertex_loader.Load(input, context, attribute_config, vertex);
                        auto num_attributes = context.registers.vertex_attributes.GetNumTotalAttributes();
Vulkan::preassembled_triangles = true;
                        context.primitive_assembler_new.SubmitVertex(   context, input,
                                                                        [&](auto& context, auto& v1, auto& v2, auto& v3) {
                                                                            context.renderer->AddVertex(context, num_attributes, v1);
                                                                            context.renderer->AddVertex(context, num_attributes, v2);
                                                                            context.renderer->AddVertex(context, num_attributes, v3);
                                                                        });
                    } else {
                        // Hardware vertex loading implies hardware primitive assembly
                        vertex_loader.Load(input, context, attribute_config, vertex);
                        auto num_attributes = context.registers.vertex_attributes.GetNumTotalAttributes();
                        context.renderer->AddVertex(context, num_attributes, input);
                    }
                }
            }
            TracyCZoneEnd(VertexLoading);
            vertex_loader_activity->Interrupt();

            if (!render_on_cpu) {
                context.renderer->FinalizeTriangleBatch(context, engine, is_indexed && hardware_vertex_loading);
            }

            if (g_debug_context)
                g_debug_context->OnEvent(DebugContext::Event::FinishedPrimitiveBatch, nullptr);

            break;
        }

        case PICA_REG_INDEX(fixed_vertex_attribute_sink.index):
            // There should be no pending data for fixed and/or immediate-mode attribute data
//            if (context.fixed_vertex_attributes.word_index != 0) {
//                throw std::runtime_error("Changed fixed vertex attribute before submitting all of the previous attribute's data");
//            }

            break;

        // Fixed vertex attribute data
        case PICA_REG_INDEX(fixed_vertex_attribute_sink.data[0]):
        case PICA_REG_INDEX(fixed_vertex_attribute_sink.data[1]):
        case PICA_REG_INDEX(fixed_vertex_attribute_sink.data[2]):
        {
            if (mask != 0xffffffff) {
                throw std::runtime_error("Tried to submit vertex attributes with mask");
            }

            auto is_attribute_data_command = [](uint32_t id) {
                return (id >= PICA_REG_INDEX(fixed_vertex_attribute_sink.data[0]) &&
                        id <= PICA_REG_INDEX(fixed_vertex_attribute_sink.data[2]));
            };

//            context.shader_uniforms.f[95].x = float24::FromFloat32(1.0);
//            context.shader_uniforms.f[95].y = float24::FromFloat32(1.0);
//            context.shader_uniforms.f[95].z = float24::FromFloat32(1.0);
//            context.shader_uniforms.f[95].w = float24::FromFloat32(1.0);

            auto& sink = registers.fixed_vertex_attribute_sink;
            if (sink.IsImmediateSubmission()) {
                const bool render_on_cpu = (context.settings->get<Settings::RendererTag>() == Settings::Renderer::Software);

                auto words_per_vertex = 3 * (registers.immediate_rendering_max_input_attribute_index + 1);

                // Read ahead to find the total data size
                uint32_t num_vertices = 1; // There's always at least one vertex
                // Skip to the second vertex, then keep incrementing num_vertices until we reach a different GPU command
                for (auto it = next_command + (words_per_vertex - 1); it && is_attribute_data_command(it.Id()); it += words_per_vertex) {
                    ++num_vertices;
                }

                if (IsNopDraw(registers)) {
                    break;
                }

                VertexShader::InputVertex input {};
                auto& engine = context.shader_engines.GetOrCompile(context, context.settings->get<Settings::ShaderEngineTag>(), registers.vs_main_offset);
                engine.Reset(registers.vs_input_register_map, input, 1 + registers.max_shader_input_attribute_index());
                engine.UpdateUniforms(context.shader_uniforms.f);
                const bool output_vertexes_cacheable = !engine.ProcessesInputVertexesOnGPU();

                if (!render_on_cpu) {
                    // NOTE: For triangle lists, the optimal triangle bound is
                    //       "num_vertices / 3", but this is too low for strips
                    //       and fans. Instead we use the vertex count as a safe
                    //       upper bound for the triangle count.
                    context.renderer->PrepareTriangleBatch(context, engine, num_vertices, false);
                }

                bool first = true;
                while (is_attribute_data_command(next_command.Id())) {
                    uint32_t data_buffer[48] = { value };
                    for (uint32_t word_index = first; word_index != words_per_vertex; ++word_index) {
                        if (!is_attribute_data_command(next_command.Id())) {
                            throw std::runtime_error("Submitted other commands before vertex was complete");
                        }
                        data_buffer[word_index] = next_command.Value();
fprintf(stderr, "WritePicaReg_cont: %#04x <- %#010x & %#010x\n", next_command.Id(), next_command.Value(), next_command.Mask());
                        ++next_command;
                    }
                    first = false;

                    // Submit vertex
                    {
                        // Initialize data for the current vertex
                        input = {};

                        // TODO: Instead of loading into data_buffer, we can just decode each attribute individually!
                        auto num_attributes = context.registers.immediate_rendering_max_input_attribute_index + 1;
                        for (size_t attribute_index = 0; attribute_index < num_attributes; ++attribute_index) {
                            // Load vertex data: Each attribute is packed as four 24-bit floats
                            uint32_t* data = &data_buffer[3 * attribute_index];
                            input.attr[attribute_index][3] = float24::FromRawFloat(data[0] >> 8);
                            input.attr[attribute_index][2] = float24::FromRawFloat(((data[0] & 0xFF)<<16) | ((data[1] >> 16) & 0xFFFF));
                            input.attr[attribute_index][1] = float24::FromRawFloat(((data[1] & 0xFFFF)<<8) | ((data[2] >> 24) & 0xFF));
                            input.attr[attribute_index][0] = float24::FromRawFloat(data[2] & 0xFFFFFF);
                        }

                        if (g_debug_context)
                            g_debug_context->OnEvent(DebugContext::Event::VertexLoaded, (void*)&input);

                        if (output_vertexes_cacheable) {
                            // Send to vertex shader
                            VertexShader::OutputVertex output = engine.Run(context, registers.vs_main_offset);
                            if (render_on_cpu) {
                                context.primitive_assembler.SubmitVertex(context, output, Clipper::ProcessTriangle);
                            } else {
                                context.primitive_assembler.SubmitVertex(context, output, [&](auto& context, auto& v1, auto& v2, auto& v3) { return context.renderer->AddPreassembledTriangle(context, v1, v2, v3); });
                            }
                        } else {
                            // TODO: Add hardware_primitive_assembly check
                            // Hardware vertex loading obviously doesn't apply here, but the code must be able to handle it...
//                            throw Mikage::Exceptions::NotImplemented("TODO: Adapt for hardware vertex loading");
//                            vertex_loader.Load(input, context, attribute_config, vertex);
//                            auto num_attributes = context.registers.vertex_attributes.GetNumTotalAttributes();
                            context.renderer->AddVertex(context, num_attributes, input);
                        }
                    }
                }

                if (!render_on_cpu) {
                    context.renderer->FinalizeTriangleBatch(context, engine, false);
                }
            } else {
                if (sink.index >= 12) {
                    throw std::runtime_error("Invalid index for fixed vertex attribute");
                }

//                auto& attributes = context.fixed_vertex_attributes;

//                if (attributes.word_index > 3) {
//                    throw std::runtime_error("Trying to write more than 3 words of fixed vertex attribute data");
//                }

                // Load default attribute:
                // Applications are expected to write 3 words of data to this register, corresponding to 4 packed 24-bit floats.

                u32 data_buffer[4] = { value };
                for (size_t i = 1; i < 3; ++i) {
                    if (!is_attribute_data_command(next_command.Id())) {
                        throw std::runtime_error("Expected full vertex attribute");
                    }

                    data_buffer[i] = next_command.Value();
                    ++next_command;
                }

                context.vertex_attribute_defaults[sink.index][3] = float24::FromRawFloat(data_buffer[0] >> 8);
                context.vertex_attribute_defaults[sink.index][2] = float24::FromRawFloat(((data_buffer[0] & 0xFF)<<16) | ((data_buffer[1] >> 16) & 0xFFFF));
                context.vertex_attribute_defaults[sink.index][1] = float24::FromRawFloat(((data_buffer[1] & 0xFFFF)<<8) | ((data_buffer[2] >> 24) & 0xFF));
                context.vertex_attribute_defaults[sink.index][0] = float24::FromRawFloat(data_buffer[2] & 0xFFFFFF);

                ++sink.index;
            }
            break;
        }

        case PICA_REG_INDEX(vs_bool_uniforms):
            for (unsigned i = 0; i < 16; ++i)
                context.shader_uniforms.b[i] = (registers.vs_bool_uniforms.Value() & (1 << i)) != 0;

            context.shader_engines.SetDirty();
            break;

        case PICA_REG_INDEX_WORKAROUND(vs_int_uniforms[0], 0x2b1):
        case PICA_REG_INDEX_WORKAROUND(vs_int_uniforms[1], 0x2b2):
        case PICA_REG_INDEX_WORKAROUND(vs_int_uniforms[2], 0x2b3):
        case PICA_REG_INDEX_WORKAROUND(vs_int_uniforms[3], 0x2b4):
        {
            int index = (id - PICA_REG_INDEX_WORKAROUND(vs_int_uniforms[0], 0x2b1));
            auto values = registers.vs_int_uniforms[index];
            context.shader_uniforms.i[index] = Math::Vec4<u8>(values.x, values.y, values.z, values.w);
            context.shader_engines.SetDirty();
            LOG_TRACE(HW_GPU, "Set integer uniform %d to %02x %02x %02x %02x",
                      index, values.x.Value(), values.y.Value(), values.z.Value(), values.w.Value());
            break;
        }

        case PICA_REG_INDEX_WORKAROUND(vs_uniform_setup.set_value[0], 0x2c1):
        case PICA_REG_INDEX_WORKAROUND(vs_uniform_setup.set_value[1], 0x2c2):
        case PICA_REG_INDEX_WORKAROUND(vs_uniform_setup.set_value[2], 0x2c3):
        case PICA_REG_INDEX_WORKAROUND(vs_uniform_setup.set_value[3], 0x2c4):
        case PICA_REG_INDEX_WORKAROUND(vs_uniform_setup.set_value[4], 0x2c5):
        case PICA_REG_INDEX_WORKAROUND(vs_uniform_setup.set_value[5], 0x2c6):
        case PICA_REG_INDEX_WORKAROUND(vs_uniform_setup.set_value[6], 0x2c7):
        case PICA_REG_INDEX_WORKAROUND(vs_uniform_setup.set_value[7], 0x2c8):
        {
            auto& uniform_setup = registers.vs_uniform_setup;

            if (mask != 0xffffffff) {
                throw std::runtime_error("Tried to overwrite uniforms with mask");
            }

            u32 uniform_write_buffer[4] = { value };
            for (size_t i = 1; i < (uniform_setup.IsFloat32() ? 4 : 3); ++i) {
                if (next_command.Id() < PICA_REG_INDEX_WORKAROUND(vs_uniform_setup.set_value[0], 0x2c1) &&
                        next_command.Id() > PICA_REG_INDEX_WORKAROUND(vs_uniform_setup.set_value[7], 0x2c8)) {
                    throw std::runtime_error("Unexpected partial uniform update");
                }

                uniform_write_buffer[i] = next_command.Value();
                ++next_command;
            }

            auto& uniform = context.shader_uniforms.f[uniform_setup.index];

            if (uniform_setup.index > 95) {
                LOG_ERROR(HW_GPU, "Invalid VS uniform index %d", (int)uniform_setup.index);
                break;
            }

            // NOTE: The destination component order indeed is "backwards"
            if (uniform_setup.IsFloat32()) {
                for (auto i : {0,1,2,3}) {
                    float val;
                    memcpy(&val, &uniform_write_buffer[i], sizeof(float));
                    uniform[3 - i] = float24::FromFloat32(val);
                }
            } else {
                // TODO: Untested
                uniform.w = float24::FromRawFloat(uniform_write_buffer[0] >> 8);
                uniform.z = float24::FromRawFloat(((uniform_write_buffer[0] & 0xFF)<<16) | ((uniform_write_buffer[1] >> 16) & 0xFFFF));
                uniform.y = float24::FromRawFloat(((uniform_write_buffer[1] & 0xFFFF)<<8) | ((uniform_write_buffer[2] >> 24) & 0xFF));
                uniform.x = float24::FromRawFloat(uniform_write_buffer[2] & 0xFFFFFF);
            }

//            context.logger->info("Set uniform {} to ({} {} {} {})", (int)uniform_setup.index,
//                      uniform.x.ToFloat32(), uniform.y.ToFloat32(), uniform.z.ToFloat32(),
//                      uniform.w.ToFloat32());

            // TODO: Verify that this actually modifies the register!
            uniform_setup.index = uniform_setup.index + 1;

            break;
        }

        // Load shader program code
        case PICA_REG_INDEX_WORKAROUND(vs_program.set_word[0], 0x2cc):
        case PICA_REG_INDEX_WORKAROUND(vs_program.set_word[1], 0x2cd):
        case PICA_REG_INDEX_WORKAROUND(vs_program.set_word[2], 0x2ce):
        case PICA_REG_INDEX_WORKAROUND(vs_program.set_word[3], 0x2cf):
        case PICA_REG_INDEX_WORKAROUND(vs_program.set_word[4], 0x2d0):
        case PICA_REG_INDEX_WORKAROUND(vs_program.set_word[5], 0x2d1):
        case PICA_REG_INDEX_WORKAROUND(vs_program.set_word[6], 0x2d2):
        case PICA_REG_INDEX_WORKAROUND(vs_program.set_word[7], 0x2d3):
        {
            // TODO: Bounds verification!
            if (registers.vs_program.offset >= context.shader_memory.size()) {
                break;
            }
            context.shader_memory[registers.vs_program.offset] = value;
            registers.vs_program.offset++;
            context.shader_engines.SetDirty();
            break;
        }

        // Load swizzle pattern data
        case PICA_REG_INDEX_WORKAROUND(vs_swizzle_patterns.set_word[0], 0x2d6):
        case PICA_REG_INDEX_WORKAROUND(vs_swizzle_patterns.set_word[1], 0x2d7):
        case PICA_REG_INDEX_WORKAROUND(vs_swizzle_patterns.set_word[2], 0x2d8):
        case PICA_REG_INDEX_WORKAROUND(vs_swizzle_patterns.set_word[3], 0x2d9):
        case PICA_REG_INDEX_WORKAROUND(vs_swizzle_patterns.set_word[4], 0x2da):
        case PICA_REG_INDEX_WORKAROUND(vs_swizzle_patterns.set_word[5], 0x2db):
        case PICA_REG_INDEX_WORKAROUND(vs_swizzle_patterns.set_word[6], 0x2dc):
        case PICA_REG_INDEX_WORKAROUND(vs_swizzle_patterns.set_word[7], 0x2dd):
        {
            // TODO: Bounds verification!
            if (registers.vs_swizzle_patterns.offset >= context.swizzle_data.size()) {
                break;
            }
            context.swizzle_data[registers.vs_swizzle_patterns.offset] = value;
            registers.vs_swizzle_patterns.offset++;
            context.shader_engines.SetDirty();
            break;
        }

        // TODO: Unify this code with the corresponding MMIO registers
        // NOTE: Games may recursively kick off hundreds of secondary lists
        //       here, so we can't use a recursive implementation strategy
        //       as that would cause a stack overflow.
        //       Notably, this affects Super Mario 3D Land's title screen
        case PICA_REG_INDEX_WORKAROUND(command_processor.trigger[0], 0x23c):
        case PICA_REG_INDEX_WORKAROUND(command_processor.trigger[1], 0x23d):
        {
            uint32_t index = id - PICA_REG_INDEX_WORKAROUND(command_processor.trigger[0], 0x23c);
            auto address = registers.command_processor.address[index] * 8;
            auto size = registers.command_processor.size[index] * 8;
            next_command.RestartAt(address, size);
            break;
        }

        default:
            break;
    }

    if (g_debug_context)
        g_debug_context->OnEvent(DebugContext::Event::CommandProcessed, reinterpret_cast<void*>(&id));
}

void ProcessCommandList(Context& context, PAddr list_addr, u32 size) {
    CommandListIterator it(*context.mem, list_addr, size);

    fprintf(stderr, "\nWritePicaReg: Incoming command list\n");
    while (it) {
        auto id = it.Id();
        auto value = it.Value();
        auto mask = it.Mask();
        WritePicaReg(context, id, value, mask, ++it);
    }
}

} // namespace

} // namespace
