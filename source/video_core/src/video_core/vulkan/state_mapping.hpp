#pragma once

#include <platform/gpu/pica.hpp>

#include <framework/exceptions.hpp>
#include <framework/meta_tools.hpp>

#include <vulkan/vulkan.hpp>

namespace Pica {

namespace Vulkan {

inline vk::Format FromPicaState(Regs::VertexAttributes::Format format, uint32_t num_elements) {
    if (num_elements < 1 || num_elements > 4) {
        throw Mikage::Exceptions::NotImplemented("Unknown vertex attribute format {:#x} with element count {}", Meta::to_underlying(format), num_elements);
    }

    // Shaders are generated with vec4 inputs; the number of components doesn't need to match, but the "numeric format" does.
    // Hence the integer based formats use eR8Sscaled instead of eR8Sint.

    switch (format) {
    case Regs::VertexAttributes::Format::BYTE:
    {
        vk::Format formats[] = { vk::Format::eR8Sscaled, vk::Format::eR8G8Sscaled, vk::Format::eR8G8B8Sscaled, vk::Format::eR8G8B8A8Sscaled };
        return formats[num_elements - 1];
    }

    case Regs::VertexAttributes::Format::UBYTE:
    {
        vk::Format formats[] = { vk::Format::eR8Uscaled, vk::Format::eR8G8Uscaled, vk::Format::eR8G8B8Uscaled, vk::Format::eR8G8B8A8Uscaled };
        return formats[num_elements - 1];
    }

    case Regs::VertexAttributes::Format::SHORT:
    {
        vk::Format formats[] = { vk::Format::eR16Sscaled, vk::Format::eR16G16Sscaled, vk::Format::eR16G16B16Sscaled, vk::Format::eR16G16B16A16Sscaled };
        return formats[num_elements - 1];
    }

    case Regs::VertexAttributes::Format::FLOAT:
    {
        vk::Format formats[] = { vk::Format::eR32Sfloat, vk::Format::eR32G32Sfloat, vk::Format::eR32G32B32Sfloat, vk::Format::eR32G32B32A32Sfloat };
        return formats[num_elements - 1];
    }

    default:
        throw Mikage::Exceptions::NotImplemented("Unknown vertex attribute format {:#x}", Meta::to_underlying(format));
    }
}

inline vk::PrimitiveTopology FromPicaState(Regs::TriangleTopology topology) {
    switch (topology) {
        case Regs::TriangleTopology::List:
        case Regs::TriangleTopology::ListIndexed: // NOTE: Actually GS-related
            return vk::PrimitiveTopology::eTriangleList;

        case Regs::TriangleTopology::Strip:
            return vk::PrimitiveTopology::eTriangleStrip;

        case Regs::TriangleTopology::Fan:
            return vk::PrimitiveTopology::eTriangleFan;

        default:
            throw Mikage::Exceptions::NotImplemented("Unknown triangle topology {:#x}", Meta::to_underlying(topology));
    }
}

inline std::pair<vk::CullModeFlags, vk::FrontFace> FromPicaState(CullMode mode) {
    switch (mode) {
    case CullMode::KeepAll:
        // TODO: Citra uses eCounterClockwise here, why?
        return std::pair { vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise };

    case CullMode::KeepClockWise:
        return std::pair { vk::CullModeFlagBits::eBack, vk::FrontFace::eCounterClockwise };

    case CullMode::KeepCounterClockWise:
        return std::pair { vk::CullModeFlagBits::eBack, vk::FrontFace::eClockwise };

    default:
        throw std::runtime_error(fmt::format("Unknown cull mode {:#x}", static_cast<uint32_t>(mode)));
    }
}

inline vk::Viewport FromPicaViewport(uint32_t origin_x, uint32_t origin_y, float24 size_x, float24 size_y, float24 depth_range, float24 depth_far_plane) {
    auto size_x32 = size_x.ToFloat32();
    auto size_y32 = size_y.ToFloat32();

    if (depth_range.ToFloat32() > 0) {
//        throw std::runtime_error(fmt::format("Unexpected viewport depth range {}", depth_range.ToFloat32()));
    }

    auto viewport = vk::Viewport {
        static_cast<float>(origin_x),
        static_cast<float>(origin_y),
        2 * size_x32,
        2 * size_y32,
        // NOTE: The depth range needs to be inverted (and reordered) since PICA200 clips from -1 <= Z_d <= 0 whereas Vulkan clips from 0 <= Z_d <= 1
        -depth_far_plane.ToFloat32(),
        -(depth_far_plane.ToFloat32() + depth_range.ToFloat32())
    };

    // TODO: Shouldn't be necessary. But sometimes we get values like 1.000031 here..
    if (viewport.maxDepth > 1.0) viewport.maxDepth = 1.0;

    return viewport;
}

inline vk::BlendFactor FromPicaState(AlphaBlendFactor factor) {
    switch (factor) {
    case AlphaBlendFactor::Zero:
        return vk::BlendFactor::eZero;

    case AlphaBlendFactor::One:
        return vk::BlendFactor::eOne;

    case AlphaBlendFactor::SourceColor:
        return vk::BlendFactor::eSrcColor;

    case AlphaBlendFactor::DestinationColor:
        return vk::BlendFactor::eDstColor;

    case AlphaBlendFactor::SourceAlpha:
        return vk::BlendFactor::eSrcAlpha;

    case AlphaBlendFactor::OneMinusSourceAlpha:
        return vk::BlendFactor::eOneMinusSrcAlpha;

    case AlphaBlendFactor::DestinationAlpha:
        return vk::BlendFactor::eDstAlpha;

    case AlphaBlendFactor::OneMinusDestinationAlpha:
        return vk::BlendFactor::eOneMinusDstAlpha;

    case AlphaBlendFactor::ConstantColor:
        return vk::BlendFactor::eConstantColor;

    case AlphaBlendFactor::OneMinusConstantColor:
        return vk::BlendFactor::eOneMinusConstantColor;

    case AlphaBlendFactor::ConstantAlpha:
        return vk::BlendFactor::eConstantAlpha;

    case AlphaBlendFactor::OneMinusConstantAlpha:
        return vk::BlendFactor::eOneMinusConstantAlpha;

    case AlphaBlendFactor::SourceAlphaSaturate:
        return vk::BlendFactor::eSrcAlphaSaturate;

    default:
        throw std::runtime_error(fmt::format("Unknown alpha blend factor {:#x}", static_cast<uint32_t>(factor)));
    }
}

inline vk::BlendOp FromPicaState(AlphaBlendEquation eq) {
    switch (eq) {
    case AlphaBlendEquation::Add:
        return vk::BlendOp::eAdd;

    case AlphaBlendEquation::ReverseSubtract:
        return vk::BlendOp::eReverseSubtract;

    default:
        throw std::runtime_error(fmt::format("Unknown alpha blend equation {:#x}", static_cast<uint32_t>(eq)));
    }
}

inline vk::LogicOp FromPicaState(LogicOp op) {
    switch (op) {
    case LogicOp::Copy:
        return vk::LogicOp::eCopy;

    case LogicOp::Set:
        return vk::LogicOp::eSet;

    case LogicOp::Noop:
        return vk::LogicOp::eNoOp;

    default:
        throw std::runtime_error(fmt::format("Unknown logic op {:#x}", static_cast<uint32_t>(op)));
    }
}

inline vk::CompareOp FromPicaState(StencilTest::CompareFunc compare_func) {
    using Func = StencilTest::CompareFunc;

    switch (compare_func) {
    case Func::Never:
        return vk::CompareOp::eNever;

    case Func::Always:
        return vk::CompareOp::eAlways;

    case Func::Equal:
        return vk::CompareOp::eEqual;

    case Func::NotEqual:
        return vk::CompareOp::eNotEqual;

    case Func::LessThan:
        return vk::CompareOp::eLess;

    case Func::GreaterThan:
        return vk::CompareOp::eGreater;

    default:
        throw std::runtime_error(fmt::format("Unknown stencil compare func {:#x}", static_cast<uint32_t>(compare_func)));
    }
}

inline vk::StencilOp FromPicaState(StencilTest::Op op) {
    using Op = StencilTest::Op;

    switch (op) {
    case Op::Keep:
        return vk::StencilOp::eKeep;

    case Op::Zero:
        return vk::StencilOp::eZero;

    case Op::Replace:
        return vk::StencilOp::eReplace;

    case Op::IncrementAndClamp:
        return vk::StencilOp::eIncrementAndClamp;

    case Op::DecrementAndClamp:
        return vk::StencilOp::eDecrementAndClamp;

    case Op::Invert:
        return vk::StencilOp::eInvert;

    default:
        throw std::runtime_error(fmt::format("Unknown stencil op {:#x}", static_cast<uint32_t>(op)));
    }
}

inline vk::StencilOpState FromPicaState(StencilTest stencil_func) {
    return {
        FromPicaState(stencil_func.op_fail_stencil()),
        FromPicaState(stencil_func.op_pass_both()),
        FromPicaState(stencil_func.op_pass_stencil_fail_depth()),
        FromPicaState(stencil_func.compare_function()),
        stencil_func.mask_in(),
        stencil_func.mask_out(),
        stencil_func.reference()
    };
}

inline vk::CompareOp FromPicaState(DepthFunc depth_func) {
    switch (depth_func) {
    // TODO: What do games use this for? Consider providing an optimized code path!
    //       Used in Cubic Ninja's opening screens
    case DepthFunc::Never:
        return vk::CompareOp::eNever;

    case DepthFunc::Always:
        return vk::CompareOp::eAlways;

    case DepthFunc::LessThan:
        return vk::CompareOp::eLess;

    case DepthFunc::LessThanOrEqual:
        return vk::CompareOp::eLessOrEqual;

    case DepthFunc::GreaterThan:
        return vk::CompareOp::eGreater;

    case DepthFunc::GreaterThanOrEqual:
        return vk::CompareOp::eGreaterOrEqual;

    default:
        throw std::runtime_error(fmt::format("Unknown depth test function {:#x}", static_cast<uint32_t>(depth_func)));
    }
}

inline vk::Filter FromPicaState(TexFilter filter) {
    switch (filter) {
    case TexFilter::Nearest:
        return vk::Filter::eNearest;

    case TexFilter::Linear:
        return vk::Filter::eLinear;

    default:
        throw std::runtime_error(fmt::format("Unknown tex filter {:#x}", static_cast<uint32_t>(filter)));
    }
}

inline vk::SamplerAddressMode FromPicaState(TexWrapMode mode) {
    switch (mode) {
    case TexWrapMode::ClampToEdge:
        return vk::SamplerAddressMode::eClampToEdge;

    // TODO: Dummy implementation for animal crossing
    case TexWrapMode::ClampToBorder:
        return vk::SamplerAddressMode::eClampToEdge;

    case TexWrapMode::Repeat:
        return vk::SamplerAddressMode::eRepeat;

    case TexWrapMode::MirroredRepeat:
        return vk::SamplerAddressMode::eMirroredRepeat;

    default:
        throw std::runtime_error(fmt::format("Unknown tex wrap mode {:#x}", static_cast<uint32_t>(mode)));
    }
}

} // namespace Vulkan

} // namespace Pica
