#include "primitive_assembly.h"
#include "shader.hpp"

#include <platform/gpu/pica.hpp>

#include <common/log.h>

namespace Pica {

template<typename VertexType>
PrimitiveAssembler<VertexType>::PrimitiveAssembler() {
    Configure(Regs::TriangleTopology::List);
}

template<typename VertexType>
void PrimitiveAssembler<VertexType>::Configure(Regs::TriangleTopology topology) {
    this->topology = topology;
    // TODO: Should we indeed reset all state here? Actual hardware probably has an explicit way of doing so!
    buffer_index = 0;
    strip_ready = false;
}

template<typename VertexType>
void PrimitiveAssembler<VertexType>::SubmitVertex(Context& context, VertexType& vtx, TriangleHandler triangle_handler)
{
    switch (topology) {
        case Regs::TriangleTopology::List:
        case Regs::TriangleTopology::ListIndexed:
            if (buffer_index < 2) {
                buffer[buffer_index++] = vtx;
            } else {
                buffer_index = 0;

                triangle_handler(context, buffer[0], buffer[1], vtx);
            }
            break;

        case Regs::TriangleTopology::Strip:
        case Regs::TriangleTopology::Fan:
            if (strip_ready)
                triangle_handler(context, buffer[0], buffer[1], vtx);

            buffer[buffer_index] = vtx;

            if (topology == Regs::TriangleTopology::Strip) {
                strip_ready |= (buffer_index == 1);
                buffer_index = !buffer_index;
            } else if (topology == Regs::TriangleTopology::Fan) {
                buffer_index = 1;
                strip_ready = true;
            }
            break;

        default:
            LOG_ERROR(HW_GPU, "Unknown triangle topology %x:", (int)topology);
            break;
    }
}

// explicitly instantiate use cases
template
struct PrimitiveAssembler<VertexShader::OutputVertex>;
template
struct PrimitiveAssembler<VertexShader::InputVertex>;

} // namespace
