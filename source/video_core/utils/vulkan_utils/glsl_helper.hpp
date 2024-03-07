#pragma once

#include <shaderc/shaderc.hpp>

/**
 * Compiles the given GLSL shader source to SPIR-V
 */
std::vector<uint32_t> CompileShader(shaderc_shader_kind, const char* code, const char* entrypoint);
