#include <vector>

#include "clipper.h"
#include "context.h"
#include "rasterizer.h"
#include "shader.hpp"

#include <platform/gpu/pica.hpp>

#include <common/log.h>

namespace Pica {

namespace Clipper {

struct ClippingEdge {
public:
    ClippingEdge(Math::Vec4<float24> coeffs,
                 Math::Vec4<float24> bias = Math::Vec4<float24>(float24::FromFloat32(0),
                                                                float24::FromFloat32(0),
                                                                float24::FromFloat32(0),
                                                                float24::FromFloat32(0)))
        : coeffs(coeffs),
          bias(bias)
    {
    }

    bool IsInside(const OutputVertex& vertex) const {
        return Math::Dot(vertex.pos + bias, coeffs) <= float24::FromFloat32(0);
    }

    bool IsOutSide(const OutputVertex& vertex) const {
        return !IsInside(vertex);
    }

    OutputVertex GetIntersection(const OutputVertex& v0, const OutputVertex& v1) const {
        float24 dp = Math::Dot(v0.pos + bias, coeffs);
        float24 dp_prev = Math::Dot(v1.pos + bias, coeffs);
        float24 factor = dp_prev / (dp_prev - dp);

        return OutputVertex::Lerp(factor, v0, v1);
    }

private:
    float24 pos;
    Math::Vec4<float24> coeffs;
    Math::Vec4<float24> bias;
};

static void InitScreenCoordinates(Regs& registers, OutputVertex& vtx)
{
    struct {
        float24 halfsize_x;
        float24 offset_x;
        float24 halfsize_y;
        float24 offset_y;
        float24 zscale;
        float24 offset_z;
    } viewport;

    viewport.halfsize_x = float24::FromRawFloat(registers.viewport_size_x);
    viewport.halfsize_y = float24::FromRawFloat(registers.viewport_size_y);
    viewport.offset_x   = float24::FromFloat32(static_cast<float>(registers.viewport_corner.x));
    viewport.offset_y   = float24::FromFloat32(static_cast<float>(registers.viewport_corner.y));
    viewport.zscale     = float24::FromRawFloat(registers.viewport_depth_range);
    viewport.offset_z   = float24::FromRawFloat(registers.viewport_depth_far_plane);

    vtx.screenpos[0] = (vtx.pos.x / vtx.pos.w + float24::FromFloat32(1.0)) * viewport.halfsize_x + viewport.offset_x;
    vtx.screenpos[1] = (vtx.pos.y / vtx.pos.w + float24::FromFloat32(1.0)) * viewport.halfsize_y + viewport.offset_y;
    vtx.screenpos[2] = viewport.offset_z + vtx.pos.z / vtx.pos.w * viewport.zscale;
}

// Construct an std::vector with the given element type and a preallocated capacity
template<typename ValueType>
static std::vector<ValueType> MakePreallocatedVector(std::size_t size) {
    std::vector<ValueType> ret;
    ret.reserve(size);
    return std::move(ret);
}

void ProcessTriangle(Context& context, OutputVertex &v0, OutputVertex &v1, OutputVertex &v2) {
    // TODO (neobrain):
    // The list of output vertices has some fixed maximum size,
    // however I haven't taken the time to figure out what it is exactly.
    // For now, we hence just assume a maximal size of 1000 vertices.
    // These buffers are made static so they don't allocate memory on each call
    const size_t max_vertices = 1000;

    // Make sure to reserve space for all vertices.
    // Without this, buffer reallocation would invalidate references.
    static std::vector<OutputVertex> buffer_vertices = MakePreallocatedVector<OutputVertex>(max_vertices);
    static std::vector<OutputVertex*> output_list = MakePreallocatedVector<OutputVertex*>(max_vertices);

    buffer_vertices.clear();
    output_list.clear();

    output_list.push_back(&v0);
    output_list.push_back(&v1);
    output_list.push_back(&v2);

    // NOTE: We clip against a w=epsilon plane to guarantee that the output has a positive w value.
    // TODO: Not sure if this is a valid approach. Also should probably instead use the smallest
    //       epsilon possible within float24 accuracy.
    static const float24 EPSILON = float24::FromFloat32(0.00001);
    static const float24 f0 = float24::FromFloat32(0.0);
    static const float24 f1 = float24::FromFloat32(1.0);
    static const std::array<ClippingEdge, 7> clipping_edges = {{
        { Math::MakeVec( f1,  f0,  f0, -f1) },  // x = +w
        { Math::MakeVec(-f1,  f0,  f0, -f1) },  // x = -w
        { Math::MakeVec( f0,  f1,  f0, -f1) },  // y = +w
        { Math::MakeVec( f0, -f1,  f0, -f1) },  // y = -w
        { Math::MakeVec( f0,  f0,  f1,  f0) },  // z =  0
        { Math::MakeVec( f0,  f0, -f1, -f1) },  // z = -w
        { Math::MakeVec( f0,  f0,  f0, -f1), Math::Vec4<float24>(f0, f0, f0, EPSILON) }, // w = EPSILON
    }};

    // TODO: If one vertex lies outside one of the depth clipping planes, some platforms (e.g. Wii)
    //       drop the whole primitive instead of clipping the primitive properly. We should test if
    //       this happens on the 3DS, too.

    // Simple implementation of the Sutherland-Hodgman clipping algorithm.
    // TODO: Make this less inefficient (currently lots of useless buffering overhead happens here)
    for (auto edge : clipping_edges) {

        const std::vector<OutputVertex*> input_list = output_list;
        output_list.clear();

        const OutputVertex* reference_vertex = input_list.back();

        for (const auto& vertex : input_list) {
            // NOTE: This algorithm changes vertex order in some cases!
            if (edge.IsInside(*vertex)) {
                if (edge.IsOutSide(*reference_vertex)) {
                    buffer_vertices.push_back(edge.GetIntersection(*vertex, *reference_vertex));
                    output_list.push_back(&(buffer_vertices.back()));
                }

                output_list.push_back(vertex);
            } else if (edge.IsInside(*reference_vertex)) {
                buffer_vertices.push_back(edge.GetIntersection(*vertex, *reference_vertex));
                output_list.push_back(&(buffer_vertices.back()));
            }

            reference_vertex = vertex;
        }

        // Need to have at least a full triangle to continue...
        if (output_list.size() < 3)
            return;
    }

    InitScreenCoordinates(context.registers, *(output_list[0]));
    InitScreenCoordinates(context.registers, *(output_list[1]));

    for (size_t i = 0; i < output_list.size() - 2; i ++) {
        OutputVertex& vtx0 = *(output_list[0]);
        OutputVertex& vtx1 = *(output_list[i+1]);
        OutputVertex& vtx2 = *(output_list[i+2]);

        InitScreenCoordinates(context.registers, vtx2);

//        LOG_TRACE(Render_Software,
/*        printf(
                  "Triangle %lu/%lu (%lu buffer vertices) at position (%.3f, %.3f, %.3f, %.3f), "
                  "(%.3f, %.3f, %.3f, %.3f), (%.3f, %.3f, %.3f, %.3f) and "
                  "screen position (%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f)\n",
                  i,output_list.size(), buffer_vertices.size(),
                  vtx0.pos.x.ToFloat32(), vtx0.pos.y.ToFloat32(), vtx0.pos.z.ToFloat32(), vtx0.pos.w.ToFloat32(),
                  vtx1.pos.x.ToFloat32(), vtx1.pos.y.ToFloat32(), vtx1.pos.z.ToFloat32(), vtx1.pos.w.ToFloat32(),
                  vtx2.pos.x.ToFloat32(), vtx2.pos.y.ToFloat32(), vtx2.pos.z.ToFloat32(), vtx2.pos.w.ToFloat32(),
                  vtx0.screenpos.x.ToFloat32(), vtx0.screenpos.y.ToFloat32(), vtx0.screenpos.z.ToFloat32(),
                  vtx1.screenpos.x.ToFloat32(), vtx1.screenpos.y.ToFloat32(), vtx1.screenpos.z.ToFloat32(),
                  vtx2.screenpos.x.ToFloat32(), vtx2.screenpos.y.ToFloat32(), vtx2.screenpos.z.ToFloat32());*/

        Rasterizer::ProcessTriangle(context, vtx0, vtx1, vtx2);
    }
}


} // namespace

} // namespace
