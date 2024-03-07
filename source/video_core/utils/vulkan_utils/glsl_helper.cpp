#include "glsl_helper.hpp"

#include <cstring>

std::vector<uint32_t> CompileShader(shaderc_shader_kind kind, const char* code, const char* entrypoint) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;

    auto result = compiler.CompileGlslToSpv(code, strlen(code), kind, "pipeline_shader", entrypoint, options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        throw std::runtime_error("Failed to compile shader: " + result.GetErrorMessage() + "\nShader code: \n" + code);
    }
    return std::vector<uint32_t>(result.begin(), result.end());
}
