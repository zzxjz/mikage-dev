#pragma once

namespace Pica {

struct Context;

namespace VertexShader {
    struct OutputVertex;
}

namespace Clipper {

using VertexShader::OutputVertex;

void ProcessTriangle(Context& context, OutputVertex& v0, OutputVertex& v1, OutputVertex& v2);

} // namespace

} // namespace
