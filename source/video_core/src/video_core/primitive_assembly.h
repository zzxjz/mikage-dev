#pragma once

#include <functional>

#include <platform/gpu/pica.hpp>

namespace Pica {

struct Context;

/*
 * Utility class to build triangles from a series of vertices,
 * according to a given triangle topology.
 */
template<typename VertexType>
struct PrimitiveAssembler {
    using TriangleHandler = std::function<void(Context&,
                                               VertexType& v0,
                                               VertexType& v1,
                                               VertexType& v2)>;

    PrimitiveAssembler();

    void Configure(Regs::TriangleTopology topology);

    /*
     * Queues a vertex, builds primitives from the vertex queue according to the given
     * triangle topology, and calls triangle_handler for each generated primitive.
     * NOTE: We could specify the triangle handler in the constructor, but this way we can
     * keep event and handler code next to each other.
     */
    void SubmitVertex(Context&, VertexType& vtx, TriangleHandler triangle_handler);

private:
    Regs::TriangleTopology topology;

    int buffer_index;
    VertexType buffer[2];
    bool strip_ready;
};


} // namespace
