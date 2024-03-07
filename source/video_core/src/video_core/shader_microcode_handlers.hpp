#pragma once

#include "shader_microcode.hpp"

#include <algorithm>

using nihstro::SwizzlePattern;

namespace Pica {
struct Context;
}

namespace Pica::VertexShader::MicroOps {

namespace detail {

static Math::Vec4<float24>& LookupRegister(RecompilerRuntimeContext& context, uint32_t reg_index) noexcept {
    // TODO: Actually merge these into a single block... Currently this reads out-of-bounds!
//    fmt::print(stderr, "LookupRegister {:#x}: {}, {}, {}, {}\n", reg_index, context.input_registers[reg_index].x.ToFloat32(), context.input_registers[reg_index].y.ToFloat32(), context.input_registers[reg_index].z.ToFloat32(), context.input_registers[reg_index].w.ToFloat32());
    return context.input_registers[reg_index];
}

static std::array<float24, 4> GetSwizzledSourceRegister1(RecompilerRuntimeContext& context, uint32_t reg_index,
                                                        bool negate, SwizzlePattern swizzle) {
    const auto& reg = LookupRegister(context, reg_index);

    std::array<float24, 4> ret = {
        reg[static_cast<int>(swizzle.GetSelectorSrc1(0))],
        reg[static_cast<int>(swizzle.GetSelectorSrc1(1))],
        reg[static_cast<int>(swizzle.GetSelectorSrc1(2))],
        reg[static_cast<int>(swizzle.GetSelectorSrc1(3))],
    };
    if (negate) {
        ret[0] = ret[0] * float24::FromFloat32(-1);
        ret[1] = ret[1] * float24::FromFloat32(-1);
        ret[2] = ret[2] * float24::FromFloat32(-1);
        ret[3] = ret[3] * float24::FromFloat32(-1);
    }
    return ret;
}

static std::array<float24, 4> GetSwizzledSourceRegister2(RecompilerRuntimeContext& context, uint32_t reg_index,
                                                        bool negate, SwizzlePattern swizzle) {
    const auto& reg = LookupRegister(context, reg_index);

    std::array<float24, 4> ret = {
        reg[static_cast<int>(swizzle.GetSelectorSrc2(0))],
        reg[static_cast<int>(swizzle.GetSelectorSrc2(1))],
        reg[static_cast<int>(swizzle.GetSelectorSrc2(2))],
        reg[static_cast<int>(swizzle.GetSelectorSrc2(3))],
    };
    if (negate) {
        ret[0] = ret[0] * float24::FromFloat32(-1);
        ret[1] = ret[1] * float24::FromFloat32(-1);
        ret[2] = ret[2] * float24::FromFloat32(-1);
        ret[3] = ret[3] * float24::FromFloat32(-1);
    }
    return ret;
}

static std::array<float24, 4> GetSwizzledSourceRegister3(RecompilerRuntimeContext& context, uint32_t reg_index,
                                                        bool negate, SwizzlePattern swizzle) {
    const auto& reg = LookupRegister(context, reg_index);

    std::array<float24, 4> ret = {
        reg[static_cast<int>(swizzle.GetSelectorSrc3(0))],
        reg[static_cast<int>(swizzle.GetSelectorSrc3(1))],
        reg[static_cast<int>(swizzle.GetSelectorSrc3(2))],
        reg[static_cast<int>(swizzle.GetSelectorSrc3(3))],
    };
    if (negate) {
        ret[0] = ret[0] * float24::FromFloat32(-1);
        ret[1] = ret[1] * float24::FromFloat32(-1);
        ret[2] = ret[2] * float24::FromFloat32(-1);
        ret[3] = ret[3] * float24::FromFloat32(-1);
    }
    return ret;
}

static float24 GetSwizzledSourceRegister1Comp(   RecompilerRuntimeContext& context, uint32_t reg_index,
                                                 uint32_t component, bool negate, SwizzlePattern swizzle) {
    const auto& reg = LookupRegister(context, reg_index);
    float24 ret = reg[static_cast<int>(swizzle.GetSelectorSrc1(component))];
    if (negate) {
        ret = ret * float24::FromFloat32(-1);
    }
    return ret;
}

static float24 GetSwizzledSourceRegister2Comp(   RecompilerRuntimeContext& context, uint32_t reg_index,
                                                 uint32_t component, bool negate, SwizzlePattern swizzle) {
    const auto& reg = LookupRegister(context, reg_index);
    float24 ret = reg[static_cast<int>(swizzle.GetSelectorSrc2(component))];
    if (negate) {
        ret = ret * float24::FromFloat32(-1);
    }
    return ret;
}

} // namespace detail

/**
 * Decorator for micro op handlers to automatically invoke the next handler
 * after execution.
 */
template<auto Handler>
static void WithNextOpChainedAfter(RecompilerRuntimeContext& context, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    Handler(context, arg1, arg2, arg3);

    auto next_micro_op = *context.next_micro_op++;
    next_micro_op.handler(context, next_micro_op.arg0, next_micro_op.arg1, next_micro_op.arg2);
}

template<MicroOpType>
static void BinaryOp(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle);

template<MicroOpType Type>
static void BinaryArithmetic(RecompilerRuntimeContext& context, uint32_t src_info, uint32_t dst_index, uint32_t swizzle_raw) {
    auto src1_reg_index = src_info & 0xff;
    auto src2_reg_index = (src_info >> 16) & 0xff;
    auto src1_addr_offset_index = static_cast<uint8_t>((src_info >> 8) & 0xff);
//    auto src2_addr_offset_index = static_cast<uint8_t>((src_info >> 24) & 0xff);

    // TODO: Only few instructions support addressing on src2, so we should optimize for that...!
    // TODO: Some games (notably Super Mario 3D Land shortly into a new safe
    //       file's intro sequence) move invalid values into the address
    //       registers. It doesn't change the end result since the read data
    //       is never used, but it means we need to sanitize these values.
    //       Alternatively, we could cast the index to uint8_t and just make
    //       sure the register block is 256 bytes in size
    auto swizzle = SwizzlePattern { swizzle_raw };
    auto src1 = detail::GetSwizzledSourceRegister1(context, std::clamp<uint32_t>(src1_reg_index + context.address_registers[src1_addr_offset_index], 0, 127), swizzle.negate_src1, swizzle);
    auto src2 = detail::GetSwizzledSourceRegister2(context, src2_reg_index/* + context.address_registers[src2_addr_offset_index]*/, swizzle.negate_src2, swizzle);

    Math::Vec4<float24>& dest = detail::LookupRegister(context, dst_index);

    BinaryOp<Type>(src1, src2, dest, swizzle);

    auto next_micro_op = *context.next_micro_op++;
    next_micro_op.handler(context, next_micro_op.arg0, next_micro_op.arg1, next_micro_op.arg2);
}

template<>
void BinaryOp<MicroOpType::Add>(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

//        fmt::print(stderr, "ADD, {}: {} <- {}, {}\n", i, (src1[i] + src2[i]).ToFloat32(), src1[i].ToFloat32(), src2[i].ToFloat32());
        dest[i] = src1[i] + src2[i];
    }
}

template<>
void BinaryOp<MicroOpType::Mul>(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

//        fmt::print(stderr, "MUL, {}: {} <- {}, {}\n", i, (src1[i] * src2[i]).ToFloat32(), src1[i].ToFloat32(), src2[i].ToFloat32());
        dest[i] = src1[i] * src2[i];
    }
}

template<>
void BinaryOp<MicroOpType::Max>(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

//        fmt::print(stderr, "MAX, {}: {} <- {}, {}\n", i, ((src1[i] > src2[i]) ? src1[i] : src2[i]).ToFloat32(), src1[i].ToFloat32(), src2[i].ToFloat32());
        dest[i] = (src1[i] > src2[i]) ? src1[i] : src2[i];
    }
}

template<>
void BinaryOp<MicroOpType::Min>(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

//        fmt::print(stderr, "MIN, {}: {} <- {}, {}\n", i, ((src1[i] < src2[i]) ? src1[i] : src2[i]).ToFloat32(), src1[i].ToFloat32(), src2[i].ToFloat32());
        dest[i] = (src1[i] < src2[i]) ? src1[i] : src2[i];
    }
}

template<int NumComponents>
static void DotHelper(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    float24 dot = float24::FromFloat32(0.f);
    for (int i = 0; i < NumComponents; ++i)
        dot = dot + src1[i] * src2[i];

    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

        dest[i] = dot;
//        fmt::print(stderr, "DOT{}, {}: {} <- {}\n", NumComponents, i, dest[i].ToFloat32(), dot.ToFloat32());
    }
}

template<>
void BinaryOp<MicroOpType::Dot3>(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    return DotHelper<3>(src1, src2, dest, swizzle);
}

template<>
void BinaryOp<MicroOpType::Dot4>(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    return DotHelper<4>(src1, src2, dest, swizzle);
}

static void Mad(RecompilerRuntimeContext& context, uint32_t src_info, uint32_t dst_index, uint32_t swizzle_raw) {
    auto src1_reg_index = src_info & 0xff;
    auto src2_reg_index = (src_info >> 16) & 0xff;
    auto src3_reg_index = (dst_index >> 16) & 0xff;
    auto src2_addr_offset_index = static_cast<uint8_t>((src_info >> 8) & 0xff);
    auto src3_addr_offset_index = static_cast<uint8_t>((src_info >> 24) & 0xff);

    // TODO: Some games (notably Super Mario 3D Land shortly into a new safe
    //       file's intro sequence) move invalid values into the address
    //       registers. It doesn't change the end result since the read data
    //       is never used, but it means we need to sanitize these values.
    //       Alternatively, we could cast the index to uint8_t and just make
    //       sure the register block is 256 bytes in size
    auto swizzle = SwizzlePattern { swizzle_raw };
    auto src1 = detail::GetSwizzledSourceRegister1(context, src1_reg_index, swizzle.negate_src1, swizzle);
    auto src2 = detail::GetSwizzledSourceRegister2(context, std::clamp<uint32_t>(src2_reg_index + context.address_registers[src2_addr_offset_index], 0, 127), swizzle.negate_src2, swizzle);
    auto src3 = detail::GetSwizzledSourceRegister3(context, std::clamp<uint32_t>(src3_reg_index + context.address_registers[src3_addr_offset_index], 0, 127), swizzle.negate_src3, swizzle);

    Math::Vec4<float24>& dest = detail::LookupRegister(context, dst_index & 0xff);

    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

        // TODO: Hint norestrict to the compiler?
//        fmt::print(stderr, "MAD, {}: {} <- {}, {}, {}\n", i, (src1[i] * src2[i] + src3[i]).ToFloat32(), src1[i].ToFloat32(), src2[i].ToFloat32(), src3[i].ToFloat32());
        dest[i] = src1[i] * src2[i] + src3[i];
    }
}

template<MicroOpType>
static void UnaryOp(const std::array<float24, 4>& src1, Math::Vec4<float24>& dest, SwizzlePattern swizzle);

template<MicroOpType Type>
static void UnaryArithmetic(RecompilerRuntimeContext& context, uint32_t src_info, uint32_t dst_index, uint32_t swizzle_raw) {
    auto src1_reg_index = src_info & 0xff;
    auto src1_addr_offset_index = static_cast<uint8_t>((src_info >> 8) & 0xff);

    Math::Vec4<float24>& dest = detail::LookupRegister(context, dst_index);

    auto clamped_reg_index = std::clamp<uint32_t>(src1_reg_index + context.address_registers[src1_addr_offset_index], 0, 127);

    SwizzlePattern swizzle { swizzle_raw };
    auto src1 = detail::GetSwizzledSourceRegister1(context, clamped_reg_index, swizzle.negate_src1, swizzle);

    UnaryOp<Type>(src1, dest, swizzle);

    auto next_micro_op = *context.next_micro_op++;
    next_micro_op.handler(context, next_micro_op.arg0, next_micro_op.arg1, next_micro_op.arg2);
}

template<>
void UnaryOp<MicroOpType::Mov>(const std::array<float24, 4>& src1, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

        // TODO: Hint norestrict to the compiler?
        dest[i] = src1[i];
    }
}

template<>
void UnaryOp<MicroOpType::Floor>(const std::array<float24, 4>& src1, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

        dest[i] = float24::FromFloat32(std::floor(src1[i].ToFloat32()));
    }
}

static void MovToAddressReg(RecompilerRuntimeContext& context, uint32_t src_info, uint32_t dst_index, uint32_t swizzle_raw) {
    auto src1_reg_index = src_info & 0xff;
    auto src1_addr_offset_index = static_cast<uint8_t>((src_info >> 8) & 0xff);

    auto clamped_reg_index = std::clamp<uint32_t>(src1_reg_index + context.address_registers[src1_addr_offset_index], 0, 127);

    SwizzlePattern swizzle { swizzle_raw };

    for (int i = 0; i < 2; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

        auto src1 = detail::GetSwizzledSourceRegister1Comp(context, clamped_reg_index, i, swizzle.negate_src1, swizzle);
        // TODO: Figure out how the rounding is done on hardware
        context.address_registers[i + 1] = static_cast<s32>(src1.ToFloat32());
    }
}

static void Rcp(RecompilerRuntimeContext& context, uint32_t src_info, uint32_t dst_index, uint32_t swizzle_raw) {
    auto src1_reg_index = src_info & 0xff;
    auto src1_addr_offset_index = static_cast<uint8_t>((src_info >> 8) & 0xff);

    Math::Vec4<float24>& dest = detail::LookupRegister(context, dst_index);

    auto clamped_reg_index = std::clamp<uint32_t>(src1_reg_index + context.address_registers[src1_addr_offset_index], 0, 127);

    SwizzlePattern swizzle { swizzle_raw };
    auto src1 = detail::GetSwizzledSourceRegister1Comp(context, clamped_reg_index, 0, swizzle.negate_src1, swizzle);

    // The result is computed for the first component and duplicated to all output elements
    // TODO: Be stable against division by zero!
    float24 result = float24::FromFloat32(1.0f / src1.ToFloat32());

    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

        // TODO: Hint norestrict to the compiler?
        dest[i] = result;
    }
}

static void Rsq(RecompilerRuntimeContext& context, uint32_t src_info, uint32_t dst_index, uint32_t swizzle_raw) {
    auto src1_reg_index = src_info & 0xff;
    auto src1_addr_offset_index = static_cast<uint8_t>((src_info >> 8) & 0xff);

    Math::Vec4<float24>& dest = detail::LookupRegister(context, dst_index);

    auto clamped_reg_index = std::clamp<uint32_t>(src1_reg_index + context.address_registers[src1_addr_offset_index], 0, 127);

    SwizzlePattern swizzle { swizzle_raw };
    auto src1 = detail::GetSwizzledSourceRegister1Comp(context, clamped_reg_index, 0, swizzle.negate_src1, swizzle);

    // The result is computed for the first component and duplicated to all output elements
    // TODO: Be stable against division by zero!
    float24 result = float24::FromFloat32(1.0f / sqrt(src1.ToFloat32()));

    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

        // TODO: Hint norestrict to the compiler?
        dest[i] = result;
    }
}

template<template<typename> class Comp>
static void Cmp(RecompilerRuntimeContext& context, uint32_t src_info, uint32_t component, uint32_t swizzle_raw) {
    auto src1_reg_index = src_info & 0xff;
    auto src2_reg_index = (src_info >> 16) & 0xff;
    auto src1_addr_offset_index = static_cast<uint8_t>((src_info >> 8) & 0xff);
//    auto src2_addr_offset_index = static_cast<uint8_t>((src_info >> 24) & 0xff);

    SwizzlePattern swizzle { swizzle_raw };
    auto src1 = detail::GetSwizzledSourceRegister1Comp(context, src1_reg_index + context.address_registers[src1_addr_offset_index], component, swizzle.negate_src1, swizzle);
    auto src2 = detail::GetSwizzledSourceRegister2Comp(context, src2_reg_index/* + context.address_registers[src2_addr_offset_index]*/, component, swizzle.negate_src2, swizzle);

    // NOTE: "swizzle" is actually the component index to compare. Note that contrary to the PICA200 CMP instruction, this micro op only compares a single component
    // TODO: Use a dedicated function for this instead of re-using MicroOpBinaryArithmetic
    context.conditional_code[component] = Comp<float24>{}(src1, src2);
}

static void Goto(   RecompilerRuntimeContext& context, MicroOpOffset target,
                    [[maybe_unused]] ShaderCodeOffset debug_pica_offset, uint32_t) {
    context.next_micro_op = &context.micro_ops_start[target.value];
    auto next_micro_op = *context.next_micro_op++;
    next_micro_op.handler(context, next_micro_op.arg0, next_micro_op.arg1, next_micro_op.arg2);
}

static void Branch(RecompilerRuntimeContext& context, uint32_t predicate_reg, MicroOpOffset target_if_true, MicroOpOffset target_if_false) {
    context.next_micro_op = &context.micro_ops_start[context.conditional_code[predicate_reg] ? target_if_true.value : target_if_false.value];
    auto next_micro_op = *context.next_micro_op++;
    next_micro_op.handler(context, next_micro_op.arg0, next_micro_op.arg1, next_micro_op.arg2);
}

template<bool IsLoopBegin>
static void Call(RecompilerRuntimeContext& context, MicroOpOffset target, uint32_t num_instructions, MicroOpOffset return_offset) {
    context.next_micro_op = &context.micro_ops_start[target.value];

    // TODO: Push return_offset to call stack
    if (context.call_stack.size() >= context.call_stack.capacity()) {
        throw std::runtime_error("Shader call stack exhausted");
    }
    context.call_stack.push_back({  /*target.IncrementedBy(num_instructions)*/ MicroOpOffset { 0 }, return_offset,
                                    IsLoopBegin ? context.next_repeat_counter : uint8_t { 0 },
                                    IsLoopBegin ? context.next_loop_increment : uint8_t { 0 },
                                    target });
//    context.last_call_stack_address = target.IncrementedBy(num_instructions);

    auto next_micro_op = *context.next_micro_op++;
    next_micro_op.handler(context, next_micro_op.arg0, next_micro_op.arg1, next_micro_op.arg2);
}

// Last micro op in a function or loop body
static void PopCallStack(   RecompilerRuntimeContext& context,
                            [[maybe_unused]] uint32_t debug_micro_op_offset,
                            [[maybe_unused]] uint32_t debug_pica_offset, uint32_t) {
if (context.call_stack.size() <= 1) {
throw std::runtime_error("yo wtf");
}
    auto& top = context.call_stack.back();
    context.LoopRegister() += top.loop_increment;

    MicroOpOffset return_address = top.return_address;

    // TODO: Add a specialized LoopRepeat micro op so that we don't need to
    //       check the repeat counter in the general case
    if (top.repeat_counter-- == 0) {
        context.call_stack.pop_back();
//        context.last_call_stack_address = context.call_stack.back().final_address;
    } else {
        return_address = top.loop_begin_address;
    }

    context.next_micro_op = &context.micro_ops_start[return_address.value];
    auto micro_op = *context.next_micro_op++;

    // Invoke next instruction
    micro_op.handler(context, micro_op.arg0, micro_op.arg1, micro_op.arg2);
}

template<nihstro::Instruction::FlowControlType::Op Op>
static void EvalCondition(RecompilerRuntimeContext& context, uint32_t refx, uint32_t refy, uint32_t) {
    const bool results[2] = {
        refx == context.conditional_code[0],
        refy == context.conditional_code[1] };

    using OpType = nihstro::Instruction::FlowControlType::Op;

    switch (Op) {
    case OpType::Or:
        context.conditional_code[2] = (results[0] || results[1]);
        break;

    case OpType::And:
        context.conditional_code[2] = (results[0] && results[1]);
        break;

    case OpType::JustX:
        context.conditional_code[2] = results[0];
        break;

    case OpType::JustY:
        context.conditional_code[2] = results[1];
        break;
    }
}

static void PrepareLoop(RecompilerRuntimeContext& context, uint32_t x, uint32_t y, uint32_t z) {
    context.LoopRegister() = y;
    context.next_repeat_counter = x;
    context.next_loop_increment = z;
}

static void PredicateNext(RecompilerRuntimeContext& context, uint32_t predicate_reg, uint32_t, uint32_t) {
    if (!context.conditional_code[predicate_reg]) {
        // Skip instruction
        ++context.next_micro_op;
    }
}

static void End(RecompilerRuntimeContext&, uint32_t, uint32_t, uint32_t) {
    // Do nothing (in particular, do not invoke the next handler)
}

static void Trace(RecompilerRuntimeContext&, MicroOpOffset offset, ShaderCodeOffset pica, uint32_t) {
    fmt::print(stderr, "Trace: {:#x} (pica: {:#x})\n", offset.value, 4 * pica.value);
}

} // namespace Pica::VertexShader::MicroOps
