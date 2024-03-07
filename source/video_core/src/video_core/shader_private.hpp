#pragma once

#include <nihstro/shader_bytecode.h>

namespace Pica::VertexShader {

/**
 * Abstract value-type to represent offsets in a shader.
 *
 * The template parameter is used to distinguish different address spaces, such
 * as the space of offsets in the original shader as uploaded through a PICA200
 * command list, or the space of offsets in an internal block representation of
 * the shader. Distinguishing these address spaces provides a type-safety net
 * that ensures code behavior matches developer intent.
 */
template<typename>
struct ShaderCodeOffsetBase {
    uint32_t value;

    ShaderCodeOffsetBase IncrementedBy(uint32_t instructions) const {
        return { value + instructions };
    }

    ShaderCodeOffsetBase GetPrevious() const {
        return { value - 1 };
    }

    ShaderCodeOffsetBase GetNext() const {
        return { value + 1 };
    }

    bool operator==(ShaderCodeOffsetBase other) const {
        return value == other.value;
    }

    bool operator!=(ShaderCodeOffsetBase other) const {
        return value != other.value;
    }

    bool operator<(ShaderCodeOffsetBase other) const {
        return value < other.value;
    }

    int32_t operator-(ShaderCodeOffsetBase other) const {
        return value - other.value;
    }

    template<typename Rng>
    nihstro::Instruction ReadFrom(const Rng& code) const {
        return { code[value] };
    }

    constexpr ShaderCodeOffsetBase& operator++() noexcept {
        ++value;
        return *this;
    }

    constexpr ShaderCodeOffsetBase operator++(int) noexcept {
        return { value++ + 1 };
    }

    using difference_type = int32_t;
};

using ShaderCodeOffset = ShaderCodeOffsetBase<struct PicaTag>;

} // namespace Pica::VertexShader
