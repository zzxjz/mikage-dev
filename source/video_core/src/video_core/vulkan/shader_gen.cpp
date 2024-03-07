#include "shader_gen.hpp"
#include "../context.h"
#include "../shader.hpp"

#include "../debug_utils/debug_utils.h"

#include <framework/exceptions.hpp>

namespace Pica {

namespace VertexShader {
extern std::string global_vertex_shader_code_todo;
}

namespace Vulkan {

// TODO: Return string_view instead!
std::string GenerateVertexShader(Context&, const VertexShader::ShaderEngine& shader_engine) {
    if (shader_engine.ProcessesInputVertexesOnGPU()) {
        return VertexShader::global_vertex_shader_code_todo;
    } else {
        const char* code =  "#version 450\n"
                            "layout(location = 0) in vec4 in_pos;\n"
                            "layout(location = 1) in vec4 in_color;\n"
//                            "layout(location = 2) in vec2 in_tex0;\n"
//                            "layout(location = 3) in vec2 in_tex1;\n"
//                            "layout(location = 4) in vec2 in_tex2;\n"
                            "layout(location = 2) in vec4 in_tex0;\n"
                            "layout(location = 3) in vec4 in_tex1;\n"
                            "layout(location = 4) in vec4 in_tex2;\n"

                            "layout(location = 0) out vec4 out_color;\n"
                            "layout(location = 1) out vec4 out_tex0;\n"
                            "layout(location = 2) out vec4 out_tex1;\n"
                            "layout(location = 3) out vec4 out_tex2;\n"
//                            "layout(location = 1) out vec2 out_tex0;\n"
//                            "layout(location = 2) out vec2 out_tex1;\n"
//                            "layout(location = 3) out vec2 out_tex2;\n"

                            "void main() {\n"
                            // PICA200 depth range is -1..0, whereas Vulkan uses 0..1. Not sure which is the near and which is the far plane for PICA200 though, so for now we just invert the depth coordinate
                            // TODO: Should we invert the y coordinate here?
                            "  gl_Position = vec4(in_pos.xy, -in_pos.z, in_pos.w);\n"
                            "  out_color = in_color;\n"
                            "  out_tex0 = in_tex0;\n"
                            "  out_tex1 = in_tex1;\n"
                            "  out_tex2 = in_tex2;\n"
                            "}\n";
        return code;
    }
}

static std::string GenerateConversionVec4ToUvec4(const char* vec_str) {
    return std::string("uvec4(floor(") + vec_str + (" * 255.0 + 0.5.rrrr))");
}

std::string GenerateFragmentShader(Context& context) {
    auto& registers = context.registers;

    static std::string code;
    code =  "#version 450\n"
                        "#extension GL_ARB_separate_shader_objects : enable\n\n"

                        // GLSL's dot intrinsic returns a float, so use a custom one instead
                        // TODO: Check if shader compilers can produce better code with (uint)dot(a, b)!
                        "uint udot(uvec3 a, uvec3 b) { return a.r * b.r + a.g * b.g + a.b * b.b; }\n\n"

                        // TODO: These should actually be uvec4
                        // NOTE: The alpha channel for each of the light colors is 0
                        "struct Light {\n"
                        "  vec4 ambient;\n"
                        "  vec4 diffuse;\n"
                        "  vec4 diffuse_dir;\n"
                        "  uvec4 specular[2];\n"
                        "  uvec4 spot_dir;\n"
                        "};\n"
                        "layout(std140, binding = 0) uniform Uniforms {\n"
                        "  vec4 vs_uniforms[96];\n"
                        "  uvec4 vs_uniforms_int[4];\n"
                        "  uvec4 combiner_buffer_init;\n"
                        "  uvec4 tev_const[6];\n"
                        // TODO: Array of structures rather than structure of arrays?
                        "  vec4 light_global_ambient;\n"
                        "  Light lights[8];\n"
                        "} uniforms;\n\n"

                        "layout(location = 0) in vec4 in_color;\n";
                        // TODO: Move back to vec2!
//                        "layout(location = 1) in vec2 in_tex0;\n"
//                        "layout(location = 2) in vec2 in_tex1;\n"
//                        "layout(location = 3) in vec2 in_tex2;\n"
    if (registers.texture0_enable)
                        code += "layout(location = 1) in vec4 in_tex0;\n";
    if (registers.texture1_enable)
                        code += "layout(location = 2) in vec4 in_tex1;\n";
    if (registers.texture2_enable)
                        code += "layout(location = 3) in vec4 in_tex2;\n";
    if (registers.texture0_enable)
                        code += "layout(binding = 1) uniform sampler2D sampler0;\n";
    if (registers.texture1_enable)
                        code += "layout(binding = 2) uniform sampler2D sampler1;\n";
    if (registers.texture2_enable)
                        code += "layout(binding = 3) uniform sampler2D sampler2;\n";

    code += "layout(location = 4) in vec4 in_quat;\n";
    code += "layout(location = 5) in vec3 in_view;\n";

    // Lighting lookup tables
    // NOTE: No Vulkan drivers advertise maxImageDimension1D < 4096... TODO: But actually these are texel buffers now
    const char* light_lut_names[24] = {
        "light_lut_d0", "light_lut_d1", nullptr, "light_lut_fr", "light_lut_rb", "light_lut_rg", "light_lut_rr", nullptr,
        "light_lut_sp0", "light_lut_sp1", "light_lut_sp2", "light_lut_sp3", "light_lut_sp4", "light_lut_sp5", "light_lut_sp6", "light_lut_sp7",
        "light_lut_da0", "light_lut_da1", "light_lut_da2", "light_lut_da3", "light_lut_da4", "light_lut_da5", "light_lut_da6", "light_lut_da7",
    };
    for (unsigned i = 0; i < std::size(light_lut_names); ++i) {
        if (light_lut_names[i] == nullptr) {
            // Unused LUT slots
            continue;
        }
        code += fmt::format("layout(binding = {}) uniform itextureBuffer {};\n", 4 + i, light_lut_names[i]);
    }

    code += "layout(location = 0) out vec4 out_color;\n"
        "\n"
        "int FetchLUTValueFromSignedIndex(itextureBuffer lut, float dot) {\n"
        "  int index = clamp(int(floor(dot * 128.0)), -128, 127) & 0xff;\n"
        "  int subindex = int(8.0 * (dot * 256.0 - floor(dot * 256.0)));\n" // TODO: Multiply before subtracting?
        "  ivec2 value = texelFetch(lut, index).rg;\n"
        "  return value.r + subindex * value.g / 16 * 2;\n"
        "}\n"
        "int FetchLUTValueFromUnsignedIndex(itextureBuffer lut, float dot) {\n"
        "  int index = clamp(int(floor(dot * 256.0)), 0, 255);\n"
        "  int subindex = int(8.0 * (dot * 256.0 - floor(dot * 256.0)));\n" // TODO: Multiply before subtracting?
        "  ivec2 value = texelFetch(lut, index).rg;\n"
        "  return value.r + subindex * value.g / 8 * 2;\n"
        "}\n"
        "\n"
        "void main() {\n";

    if (registers.texture0_enable)
        code += "vec4 tex0_color = texture(sampler0, in_tex0.xy);\n\n";
    else
        code += "vec4 tex0_color = vec4(0.0);\n\n"; // NOTE: Some applications behave badly and configure textures as inputs but then never end up using them. An ahead-of-time combiner compiler would handle this better...

    if (registers.texture1_enable)
        code += "vec4 tex1_color = texture(sampler1, in_tex1.xy);\n\n";
    else
        code += "vec4 tex1_color = vec4(0.0);\n\n";

    if (registers.texture2_enable)
        code += "vec4 tex2_color = texture(sampler2, in_tex2.xy);\n\n";
    else
        code += "vec4 tex2_color = vec4(0.0);\n\n";

    if (!registers.lighting.disabled()()) {
        // TODO: Should the alpha channel be 1??
        code += fmt::format("vec4 lit_color_primary = vec4(uniforms.light_global_ambient.rgb, 1.0);\n"); // TODO: Should be a uvec4. TODO: Should this be enabled even if lighting is globally off?
        code += fmt::format("uvec4 lit_color_secondary = uvec4(0, 0, 0, 255);\n");

        code += fmt::format("vec3 view = normalize(in_view);\n");

        // (TODO: Handle opposite quaternion case)
        // TODO: To support bump mapping, a generic quaternion transform must be implemented here
        code += fmt::format("vec4 quat = normalize(in_quat);\n"); // Re-normalize post-interpolation quaternion
        code += fmt::format("vec3 normal = vec3(2 * quat.x * quat.z + 2 * quat.y * quat.w, 2 * quat.y * quat.z - 2 * quat.x * quat.w, 1 - 2 * quat.x * quat.x - 2 * quat.y * quat.y);\n");
        code += fmt::format("vec3 tangent = vec3(2 * (quat.x * quat.x + quat.w * quat.w) - 1, 2 * (quat.x * quat.y + quat.z * quat.w), 2 * (quat.x * quat.z - quat.y * quat.w));\n");

        // TODO: Could probably turn this into a runtime loop in the shader to reduce number of compiled shaders
        for (size_t light_id = 0; light_id <= registers.lighting.max_light_id.value()(); ++light_id) {
            code += fmt::format("{{\n");

            // TODO: Check light_enable mask bit!
            auto light_index = registers.lighting.GetLightIndex(light_id);
            if (light_index >= std::size(registers.lighting.lights)) {
                throw Mikage::Exceptions::Invalid("Invalid light index");
            }
            auto& light_config = registers.lighting.lights[light_index];

            auto compute_dot = [light_index](LightLutInput input) {
                switch (input) {
                    case LightLutInput::NH:
                        return fmt::format("dot(normal, half_vector)");
                        break;

                    case LightLutInput::NV:
                        return fmt::format("dot(normal, view)");

                    case LightLutInput::LN:
                        return fmt::format("dot(normal, light_vector)");

                    case LightLutInput::SP:
                        return fmt::format("dot(normal, uniforms.lights[{}].diffuse_dir.rgb / 2048.0)", light_index);

                    case LightLutInput::CP:
                        // Project the half vector onto the tangent surface,
                        // then take the dot product of the result with the tangent vector
                        return fmt::format("dot(tangent, half_vector - normal * dot(normal, half_vector))");

                    default:
                        throw Mikage::Exceptions::NotImplemented("Not implemented: LUT input {}", Meta::to_underlying(input));
                }
            };

            auto fetch_lut = [&light_lut_names](LightLutIndex index, const std::string& dot, LightLutScale scale, bool is_signed) {
                const char* scale_str = nullptr;
                if (scale == LightLutScale::x1) {
                    // No extra code needed
                    scale_str = "";
                } else if (scale == LightLutScale::x2) {
                    scale_str = " * 2";
                } else if (scale == LightLutScale::x4) {
                    scale_str = " * 4";
                } else if (scale == LightLutScale::x8) {
                    scale_str = " * 8";
                } else if (scale == LightLutScale::x0_25) {
                    scale_str = " / 4";
                } else if (scale == LightLutScale::x0_5) {
                    scale_str = " / 2";
                } else {
                    throw Mikage::Exceptions::NotImplemented("Not implemented: LUT scale {}", Meta::to_underlying(scale));
                }

                if (is_signed) {
                    return fmt::format("clamp(FetchLUTValueFromSignedIndex({}, {}){}, 0, 8192)", light_lut_names[Meta::to_underlying(index)], dot, scale_str);
                } else {
                    return fmt::format("clamp(FetchLUTValueFromUnsignedIndex({}, {}){}, 0, 8192)", light_lut_names[Meta::to_underlying(index)], dot, scale_str);
                }
            };

            // TODO: dist attenuation

            // TODO: Should the normalized view be added here? Test this!
            // NOTE: Nano Assault uses a really silly light vector (-10000, 0, 2500) on the title screen, so it seems safe to assume this is unconditionally normalized
            code += fmt::format("vec3 light_vector = normalize(uniforms.lights[{}].diffuse_dir.rgb{});\n",
                                light_index, light_config.config.is_directional() ? "" : " + in_view");

            code += fmt::format("vec3 half_vector = view + light_vector;\n");
            code += fmt::format("float half_vector_length_sq = dot(half_vector, half_vector);\n");
            code += fmt::format("half_vector = normalize(half_vector);\n");

            // TODO: Should the light vector be normalized?
            code += fmt::format("float diffuse_dot = max({}, 0.0);\n", compute_dot(LightLutInput::LN));
            code += fmt::format("lit_color_primary += uniforms.lights[{}].ambient;\n", light_index);
            code += fmt::format("lit_color_primary += diffuse_dot * uniforms.lights[{}].diffuse;\n", light_index);

            code += fmt::format("uvec3 specular = uvec3(0);\n");

            if (light_config.config.abs_dot_products()) {
                // TODO: Home menu with zeldamm
                // throw Mikage::Exceptions::NotImplemented("Not implemented: abs()");
            }

            const bool spot_light_enabled = !(registers.lighting.config.spot_disabled()() & (1 << light_index)) && (!registers.lighting.config.d0_disabled() || !registers.lighting.config.d1_disabled());
            if (spot_light_enabled) {
                std::string dot = compute_dot(registers.lighting.lut_config.selector_sp());
                code += fmt::format("uint spot = {};\n",
                                    fetch_lut(LightLutIndex { Meta::to_underlying(LightLutIndex::SP0) + light_index }, dot,
                                              registers.lighting.lut_config.scale_sp(), registers.lighting.lut_config.index_signed_sp()));
            }

            if ((!registers.lighting.config.d0_disabled() && light_config.config.specular0_use_geometric_factor()) ||
                    (!registers.lighting.config.d1_disabled() && light_config.config.specular1_use_geometric_factor())) {
                // throw Mikage::Exceptions::NotImplemented("geometric factor");
                code += fmt::format("uint geo_factor = half_vector_length_sq != 0.0 ? min(int(diffuse_dot / half_vector_length_sq * 256.0), 256) : 0;\n");
            }

            // Spotlight: dist_attenuation * lut[-L dot P] * optionally_clamp_if_L_dot_N_negative * attenuation factor from shadow texture * spot color
            //registers.lighting.lut_config.selector_d0()
            // TODO: assert consistency of used/enabled LUTs
            if (!registers.lighting.config.d0_disabled()) {
                std::string dot = compute_dot(registers.lighting.lut_config.selector_d0());
                code += fmt::format("specular += ({} / 16 * ({} / 16) * uniforms.lights[{}].specular[0].rgb + 0x8000) / 0x10000{};\n",
                            fetch_lut(LightLutIndex::D0, dot, registers.lighting.lut_config.scale_d0(), registers.lighting.lut_config.index_signed_d0()),
                            spot_light_enabled ? "spot" : "4096", light_index,
                            light_config.config.specular0_use_geometric_factor() ? " * geo_factor / 256" : "");
            }

            if (!registers.lighting.config.d1_disabled()) {
                if (!registers.lighting.config.rr_disabled() || !registers.lighting.config.rg_disabled() || !registers.lighting.config.rb_disabled()) {
                    // Captain Toad
                    // throw Mikage::Exceptions::NotImplemented("Not implemented: D1 refl");
                }

                // TODO: Implement two-sided diffuse

                std::string dot = compute_dot(registers.lighting.lut_config.selector_d1());
                code += fmt::format("specular += ({} / 16 * ({} / 16) * uniforms.lights[{}].specular[1].rgb + 0x8000) / 0x10000{};\n",
                            fetch_lut(LightLutIndex::D1, dot, registers.lighting.lut_config.scale_d1(), registers.lighting.lut_config.index_signed_d1()),
                            spot_light_enabled ? "spot" : "4096", light_index,
                            light_config.config.specular1_use_geometric_factor() ? " * geo_factor / 256" : ""); // TODO: geo factor should probably be multiplied before dividing by 0x10000
            }

            if (!(registers.lighting.config.dist_disabled()() & (1 << light_index)) && (!registers.lighting.config.d0_disabled() || !registers.lighting.config.d1_disabled())) {
                // Captain Toad
                // throw Mikage::Exceptions::NotImplemented("Not implemented: Distance attenuation");
            }

            if (registers.lighting.config.clamp_highlights()) {
                // TODO: Does hardware clamp negative dot products? (We don't, since diffuse_dot is taken with abs() or clamp() above)
                // TODO: Does hardware really just use the LN product?
                code += fmt::format("if (diffuse_dot < 1/256.f) {{\n");
                code += fmt::format("  specular = uvec3(0);\n");
                code += fmt::format("}}\n");
            }

            // TODO: According to Citra, fresnel is part of the last lighting stage, which is relevant for computing the half vector
            // TODO: Need to test how the arithmetic here works out
            if (light_id == registers.lighting.max_light_id.value() && !registers.lighting.config.fr_disabled()) {
                if (registers.lighting.config.fresnel_to_primary_alpha() || registers.lighting.config.fresnel_to_secondary_alpha()) {
                    // throw Mikage::Exceptions::NotImplemented("Not implemented: Fresnel");
                    std::string dot = compute_dot(registers.lighting.lut_config.selector_fr());
                    code += fmt::format("uint fresnel = ({} / 16 * (4096 / 16) * 255 + 0x8000) / 0x10000;\n",
                                fetch_lut(LightLutIndex::FR, dot, registers.lighting.lut_config.scale_fr(), registers.lighting.lut_config.index_signed_fr()));

                    if (registers.lighting.config.fresnel_to_primary_alpha()) {
                        code += fmt::format("lit_color_primary.a = fresnel / 255.0;\n");
                    }
                    if (registers.lighting.config.fresnel_to_secondary_alpha()) {
                        code += fmt::format("lit_color_secondary.a = fresnel;\n");
                    }
                }
            }

            code += fmt::format("  lit_color_secondary.rgb += specular;\n");

            code += fmt::format("}}\n");
        }

        code += "lit_color_primary = clamp(lit_color_primary, 0, 1.0);\n";
        code += "lit_color_secondary = clamp(lit_color_secondary, 0, 255);\n";
    } else {
        // TODO: Which default value to use?
        // TODO: Is it even valid to access this value when fragment lighting is disabled? What's the behavior in that case?
        code += "vec4 lit_color_primary = vec4(0, 0, 0, 1.0);\n";
        code += "uvec4 lit_color_secondary = uvec4(0, 0, 0, 255);\n";
    }

    // TODO: Is it correct to initialize the combiner buffer to this value, or should it be something else and updated to the init value at the end of stage0?
    // Either way, making sure combiner_buffer is definitely initialized by stage 1 even if the update flag isn't set is necessary
    code += "uvec4 combiner_buffer = uniforms.combiner_buffer_init;\n";
    code += "uvec4 combiner_output = combiner_buffer;\n";
    const auto tev_stages = registers.GetTevStages();
//     DebugUtils::DumpTevStageConfig(tev_stages);
    for (size_t tev_stage_index = 0; tev_stage_index < std::size(tev_stages); ++tev_stage_index) {
        auto& tev_stage = tev_stages[tev_stage_index];

        using Source = Regs::TevStageConfig::Source;
        using ColorModifier = Regs::TevStageConfig::ColorModifier;
        using AlphaModifier = Regs::TevStageConfig::AlphaModifier;
        using Operation = Regs::TevStageConfig::Operation;

        const bool is_nop_stage_rgb = (tev_stage.color_modifier1 == ColorModifier::SourceColor && tev_stage.color_op == Operation::Replace);
        const bool is_nop_stage_a = (tev_stage.alpha_modifier1 == AlphaModifier::SourceAlpha && tev_stage.alpha_op == Operation::Replace);

        std::string tev_stage_str = "tev" + std::to_string(tev_stage_index) + "_";

        auto GetSourceColor = [&](Source source) -> std::string {
            switch (source) {
            case Source::PrimaryColor:
                return GenerateConversionVec4ToUvec4("in_color");

            // TODO: Might need to be signed!
            case Source::PrimaryFragmentColor:
//                if (registers.lighting.disabled()()) {
//                    throw std::runtime_error("Tried to access primary lighting color but fragment lighting isn't enabled");
//                }
                return GenerateConversionVec4ToUvec4("lit_color_primary");

            case Source::SecondaryFragmentColor:
                return "lit_color_secondary";

            case Source::Texture0:
                return GenerateConversionVec4ToUvec4("tex0_color");

            case Source::Texture1:
                return GenerateConversionVec4ToUvec4("tex1_color");

            case Source::Texture2:
                return GenerateConversionVec4ToUvec4("tex2_color");

            case Source::Texture3: // TODO: Some ctrulib homebrew relies on this without actually using it???
                return "uvec4(0,0,0,0)";

            case Source::CombinerBuffer:
                if (tev_stage_index == 0) {
                    // TODO: Verify the buffer is never used uninitialized.
                    //       Some applications (notably Super Mario 3D Land)
                    //       will hit this code path since they use
                    //       PreviousBuffer for the third combiner input but
                    //       the specify a binary operation so it won't get
                    //       used. We need a more sophisticated check to detect
                    //       truly invalid configurations
                    // TODO: Conversely, for stages after stage 0, we should
                    //       verify that any of the previous stages indeed has
                    //       initialized this buffer
                }
                return "combiner_buffer";

            case Source::Constant:
                return fmt::format("uniforms.tev_const[{}]", tev_stage_index);

            case Source::Previous:
                return "combiner_output";

            default:
                throw std::runtime_error(fmt::format("Unknown color combiner source {:#x}", static_cast<uint32_t>(source)));
            }
        };

        auto GetSourceAlpha = [&](Source source) -> std::string {
            switch (source) {
            case Source::PrimaryColor:
                return GenerateConversionVec4ToUvec4("in_color");

            // TODO: Might need to be signed!
            case Source::PrimaryFragmentColor:
//                if (registers.lighting.disabled()()) {
//                    throw std::runtime_error("Tried to access primary lighting color but fragment lighting isn't enabled");
//                }
                return GenerateConversionVec4ToUvec4("lit_color_primary");

            case Source::SecondaryFragmentColor:
                return "lit_color_secondary";

            case Source::Texture0:
                return GenerateConversionVec4ToUvec4("tex0_color");

            case Source::Texture1:
                return GenerateConversionVec4ToUvec4("tex1_color");

            case Source::Texture2:
                return GenerateConversionVec4ToUvec4("tex2_color");

            case static_cast<Source>(6): // TODO: Implement properly
                return "uvec4(0, 0, 0, 0)";

            case Source::CombinerBuffer:
                return "combiner_buffer";

            case Source::Constant:
                return fmt::format("uniforms.tev_const[{}]", tev_stage_index);

            case Source::Previous:
                return "combiner_output";

            default:
                throw std::runtime_error(fmt::format("Unknown alpha combiner source {:#x}", static_cast<uint32_t>(source)));
            }
        };

        auto GetModifiedColor = [](ColorModifier modifier, const char* source) -> std::string {
            switch (modifier)
            {
            case ColorModifier::SourceColor:
                return source + std::string(".rgb");

            case ColorModifier::OneMinusSourceColor:
                return std::string("(uvec3(255) - ") + source + std::string(".rgb)");

            case ColorModifier::SourceAlpha:
                return source + std::string(".aaa");

            case ColorModifier::OneMinusSourceAlpha:
                return std::string("(uvec3(255) - ") + source + std::string(".aaa)");

            case ColorModifier::SourceRed:
                return source + std::string(".rrr");

            case ColorModifier::OneMinusSourceRed:
                return std::string("(uvec3(255) - ") + source + std::string(".rrr)");

            case ColorModifier::SourceGreen:
                return source + std::string(".ggg");

            case ColorModifier::OneMinusSourceGreen:
                return std::string("(uvec3(255) - ") + source + std::string(".ggg)");

            case ColorModifier::SourceBlue:
                return source + std::string(".bbb");

            case ColorModifier::OneMinusSourceBlue:
                return std::string("(uvec3(255) - ") + source + std::string(".bbb)");

            default:
                throw std::runtime_error(fmt::format("Unknown color modifier {:#x}", static_cast<uint32_t>(modifier)));
            }
        };

        auto GetModifiedAlpha = [](AlphaModifier modifier, const char* source) -> std::string {
            switch (modifier)
            {
            case AlphaModifier::SourceAlpha:
                return source + std::string(".a");

            case AlphaModifier::OneMinusSourceAlpha:
                return std::string("(255 - ") + source + ".a)";

            case AlphaModifier::SourceRed:
                return source + std::string(".r");

            case AlphaModifier::OneMinusSourceRed:
                return std::string("(255 - ") + source + ".r)";

            case AlphaModifier::SourceGreen:
                return source + std::string(".g");

            case AlphaModifier::OneMinusSourceGreen:
                return std::string("(255 - ") + source + ".g)";

            case AlphaModifier::SourceBlue:
                return source + std::string(".b");

            case AlphaModifier::OneMinusSourceBlue:
                return std::string("(255 - ") + source + ".b)";

            default:
                throw std::runtime_error(fmt::format("Unknown alpha modifier {:#x}", static_cast<uint32_t>(modifier)));
            }
        };

        code += "uvec3 " + tev_stage_str + "rgb_in1 = " + GetModifiedColor(tev_stage.color_modifier1, GetSourceColor(tev_stage.color_source1).c_str()) + ";\n";
        code += "uvec3 " + tev_stage_str + "rgb_in2 = " + GetModifiedColor(tev_stage.color_modifier2, GetSourceColor(tev_stage.color_source2).c_str()) + ";\n";
        code += "uvec3 " + tev_stage_str + "rgb_in3 = " + GetModifiedColor(tev_stage.color_modifier3, GetSourceColor(tev_stage.color_source3).c_str()) + ";\n";

        auto CombineRGB = [&](Operation op) -> std::string {
            switch (op) {
            case Operation::Replace:
                return tev_stage_str + "rgb_in1";

            case Operation::Modulate:
                return "((" + tev_stage_str + "rgb_in1 * " + tev_stage_str + "rgb_in2) / 255)";

            case Operation::Add:
                return "min(uvec3(255), " + tev_stage_str + "rgb_in1 + " + tev_stage_str + "rgb_in2)";

            case Operation::AddSigned:
                // NOTE: Since we're using unsigned arithmetic, we clamp
                //       against 127 *before* subtracting 127 (as opposed
                //       to clamping against 0 after) to prevent underflow
                // TODO: Is it 127 or 128?
                return "(min(uvec3(255), max(uvec3(127), " + tev_stage_str + "rgb_in1 + " + tev_stage_str + "rgb_in2) - uvec3(127)))";

            case Operation::Subtract:
                return "(max(" + tev_stage_str + "rgb_in1, " + tev_stage_str + "rgb_in2) - " + tev_stage_str + "rgb_in2)";

            case Operation::Dot3RGB:
            case Operation::Dot3RGBA:
            {
                std::string op1 = tev_stage_str + "rgb_in1";
                std::string op2 = tev_stage_str + "rgb_in2";
                // The GLES 1.1 specification defines the result of this as
                // 4 * (a - vec3(0.5)) . (b - vec3(0.5)) (where "." is the dot
                // product). For us, it's more suitable to write that as:
                //
                //     4 * a.b + 3 - (a + b) . (2,2,2)
                //
                // This has two advantages:
                // - it doesn't use 0.5 (which can't be expressed as a non-normalized integer (0..255), and
                // - it can be bounds-checked more easily to prevent underflows with unsigned arithmetic
                //
                // The final formula we end up with is
                //     min(1.0, max((a + b) . (2,2,2), 4 * a.b + 3) - (a + b) . (2,2,2)
                //
                // Notable edge cases:
                // - a = b = 0 => result is 1
                // - a = (2, 0, 0), b = (2, 1, 1) => result is 9 - 1 - 1 = 7
                // TODO: Does this produce bit-identical results to the 3DS?
                return "min(255, max(4 * udot(" + op1 + ", " + op2 + ") / 255 + 3 * 255, udot(" + op1 + " + " + op2 + ", uvec3(2))) - udot(" + op1 + " + " + op2 + ", uvec3(2))).rrr";
            }

            case Operation::MultiplyThenAdd:
                return "min(uvec3(255), (" + tev_stage_str + "rgb_in1 * " + tev_stage_str + "rgb_in2 + 255 * " + tev_stage_str + "rgb_in3" + ") / 255)";

            case Operation::AddThenMultiply:
                return "min(uvec3(255), ((" + tev_stage_str + "rgb_in1 + " + tev_stage_str + "rgb_in2) * " + tev_stage_str + "rgb_in3" + ") / 255)";

            case Operation::Lerp:
                return "((" + tev_stage_str + "rgb_in1 * " + tev_stage_str + "rgb_in3 + " + tev_stage_str + "rgb_in2 * (uvec3(255) - " + tev_stage_str + "rgb_in3)) / 255)";

            default:
                throw std::runtime_error(fmt::format("Unknown color combiner operation {:#x}", static_cast<uint32_t>(op)));
            }
        };

        code += "uint " + tev_stage_str + "a_in1 = " + GetModifiedAlpha(tev_stage.alpha_modifier1, GetSourceAlpha(tev_stage.alpha_source1).c_str()) + ";\n";
        code += "uint " + tev_stage_str + "a_in2 = " + GetModifiedAlpha(tev_stage.alpha_modifier2, GetSourceAlpha(tev_stage.alpha_source2).c_str()) + ";\n";
        code += "uint " + tev_stage_str + "a_in3 = " + GetModifiedAlpha(tev_stage.alpha_modifier3, GetSourceAlpha(tev_stage.alpha_source3).c_str()) + ";\n";

        auto CombineA = [&](Operation op) -> std::string {
            switch (op) {
            case Operation::Replace:
                return tev_stage_str + "a_in1";

            case Operation::Modulate:
                return "((" + tev_stage_str + "a_in1 * " + tev_stage_str + "a_in2) / 255)";

            case Operation::Add:
                return "min(255, " + tev_stage_str + "a_in1 + " + tev_stage_str + "a_in2)";

            case Operation::AddSigned:
                return "(min(255, max(127, " + tev_stage_str + "a_in1 + " + tev_stage_str + "a_in2) - 127))";

            case Operation::Lerp:
                return "((" + tev_stage_str + "a_in1 * " + tev_stage_str + "a_in3 + " + tev_stage_str + "a_in2 * (255 - " + tev_stage_str + "a_in3)) / 255)";

            case Operation::Subtract:
                return "(max(" + tev_stage_str + "a_in1, " + tev_stage_str + "a_in2) - " + tev_stage_str + "a_in2)";

            case Operation::Dot3RGBA:
            {
                // NOTE: This combiner computes the RGB dot product and copies it into all components, including the alpha channel.
                //       The input alpha component is unused.
                std::string op1 = tev_stage_str + "rgb_in1";
                std::string op2 = tev_stage_str + "rgb_in2";
                // See color combiner code above for explanation
                return "min(255, max(4 * udot(" + op1 + ", " + op2 + ") / 255 + 3 * 255, udot(" + op1 + " + " + op2 + ", uvec3(2))) - udot(" + op1 + " + " + op2 + ", uvec3(2)))";
            }

            case Operation::MultiplyThenAdd:
                return "min(255, (" + tev_stage_str + "a_in1 * " + tev_stage_str + "a_in2 + 255 * " + tev_stage_str + "a_in3" + ") / 255)";

            case Operation::AddThenMultiply:
                // TODO: Port this change to sw renderer
                return "min(255, ((" + tev_stage_str + "a_in1 + " + tev_stage_str + "a_in2) * " + tev_stage_str + "a_in3" + ") / 255)";

            default:
                throw std::runtime_error(fmt::format("Unknown alpha combiner operation {:#x}", static_cast<uint32_t>(op)));
            }
        };

        code += "uvec3 " + tev_stage_str + "out_rgb = " + CombineRGB(tev_stage.color_op) + ";\n";

        code += "uint " + tev_stage_str + "out_a = " + CombineA(tev_stage.alpha_op) + ";\n";

        // TODO: Move to pica.h
        if (tev_stage_index > 0) {
            // Update combiner buffer with result from previous stage.
            // NOTE: This lags behind by one stage, i.e. stage 2 uses the result
            //       of stage 0, stage 3 the result of stage 1, etc.
            //       Hence we update the combiner buffer *after* the stage inputs
            //       have been read, and it is updated *before* writing the
            //       combiner output
            if (registers.combiner_buffer.TevStageUpdatesRGB(tev_stage_index - 1)) {
                code += "combiner_buffer.rgb = combiner_output.rgb;\n\n";
            }

            if (registers.combiner_buffer.TevStageUpdatesA(tev_stage_index - 1)) {
                code += "combiner_buffer.a = combiner_output.a;\n\n";
            }
        }

        // TODO: Multiplier should be used *during* the operations! Otherwise, e.g. AddSigned has reduced precision
        code += "combiner_output = ";
        bool enable_multiplier = (tev_stage.GetMultiplierRGB() != 1 || tev_stage.GetMultiplierA() != 1);
        if (enable_multiplier) {
            code += fmt::format("min(uvec4(255), uvec4(uvec3({}), {}) * ", tev_stage.GetMultiplierRGB(), tev_stage.GetMultiplierA());
        }
        code += "uvec4(" + tev_stage_str + "out_rgb, " + tev_stage_str + "out_a)";
        if (enable_multiplier) {
            code += ")";
        }
        code += ";\n\n";
    }

    auto& alpha_test = context.registers.output_merger.alpha_test;
    if (alpha_test.enable && alpha_test.function != AlphaTest::Function::Always) {
        const char* op = std::invoke([func=alpha_test.function.Value()]() {
            // Inverse mapping for finding failing pixels rather than passing ones
            switch (func) {
            case AlphaTest::Function::NotEqual:
                return "==";

            case AlphaTest::Function::GreaterThan:
                return "<=";

            case AlphaTest::Function::GreaterThanOrEqual:
                return "<";

            default:
                throw std::runtime_error(fmt::format("Unknown alpha test function {:#x}", static_cast<uint32_t>(func)));
            }
        });
        code += fmt::format("if (combiner_output.a {} {})\n", op, alpha_test.reference.Value());
        code += "  discard;\n";
    }

    code += "  out_color = vec4(combiner_output) / 255.0;\n";

    code += "}\n";
    return code;
}



} // namespace Vulkan

} // namespace Pica
