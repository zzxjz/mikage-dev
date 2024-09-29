#include "glsl_helper.hpp"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <fmt/format.h>

const struct InitGlslang {
    InitGlslang() {
        glslang::InitializeProcess();
    }
    ~InitGlslang() {
        glslang::FinalizeProcess();
    }
} manage_glslang;

std::vector<uint32_t> CompileShader(EShLanguage kind, const char* code, const char* entrypoint) {
    glslang::TShader shader(kind);
    shader.setEntryPoint(entrypoint);
    shader.setStrings(&code, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, kind, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    if (!shader.parse(GetDefaultResources(), 100, ECoreProfile, false, false, EShMsgDefault)) {
        fmt::print(stderr, "Failed to compile {} shader:\n{}\n", glslang::StageName(kind), code);
        throw std::runtime_error(fmt::format(   "Failed to compile {} shader: {}\n",
                                                glslang::StageName(kind), shader.getInfoLog()));
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        fmt::print(stderr, "Failed to link {} shader:\n{}\n", glslang::StageName(kind), code);
        throw std::runtime_error(fmt::format(   "Failed to link {} shader: {}\n",
                                                glslang::StageName(kind), program.getInfoLog()));
    }

    glslang::TIntermediate& intermediate = *program.getIntermediate(kind);
    std::vector<uint32_t> result;
    glslang::SpvOptions spv_options;
    spv_options.disableOptimizer = false;
    glslang::GlslangToSpv(intermediate, result, &spv_options);
    return result;
}
