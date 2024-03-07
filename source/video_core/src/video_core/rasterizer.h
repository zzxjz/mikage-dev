#pragma once

namespace Pica {

struct Context;

namespace VertexShader {
    struct OutputVertex;
}

namespace Rasterizer {

void ProcessTriangle(Context& context,
                     const VertexShader::OutputVertex& v0,
                     const VertexShader::OutputVertex& v1,
                     const VertexShader::OutputVertex& v2);

} // namespace Rasterizer

} // namespace Pica
