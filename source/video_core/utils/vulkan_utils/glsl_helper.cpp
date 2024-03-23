#include "glsl_helper.hpp"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <cstring>

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

    shader.parse(GetDefaultResources(), 100, false, EShMsgDefault);

    // TODO: Print shader.getInfoLog()

    glslang::TProgram program;
    program.addShader(&shader);
    program.link(EShMsgDefault);
    // TODO: Print program.getInfoLog()

    // TODO: Set up flags: optimization, validation, ...
    glslang::TIntermediate& intermediate = *program.getIntermediate(kind);
    std::vector<uint32_t> result;
    glslang::GlslangToSpv(intermediate, result);
    return result;
}
