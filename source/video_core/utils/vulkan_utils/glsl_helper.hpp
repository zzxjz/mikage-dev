#pragma once

#include <glslang/Public/ShaderLang.h>

#include <cstdint>
#include <vector>

/**
 * Compiles the given GLSL shader source to SPIR-V
 */
std::vector<uint32_t> CompileShader(EShLanguage, const char* code, const char* entrypoint);
