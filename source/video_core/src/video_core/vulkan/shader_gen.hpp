#pragma once

#include <string>

namespace Pica {

struct Context;

namespace VertexShader {
struct ShaderEngine;
}

namespace Vulkan {

std::string GenerateVertexShader(Context&, const VertexShader::ShaderEngine&);

std::string GenerateFragmentShader(Context&);

} // namespace Vulkan

} // namespace Pica
