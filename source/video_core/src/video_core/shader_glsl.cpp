#include <bitset>
#include <stack>
#include <set>

#include <range/v3/algorithm/all_of.hpp>
#include <range/v3/algorithm/copy.hpp>
#include <range/v3/algorithm/fill.hpp>
#include <range/v3/algorithm/find_if.hpp>
#include <range/v3/algorithm/upper_bound.hpp>
#include <range/v3/view/drop.hpp>
#include <range/v3/view/indirect.hpp>
#include <range/v3/view/indices.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/map.hpp>
#include <range/v3/view/take.hpp>
#include <range/v3/index.hpp>

#include <common/log.h>

#include <nihstro/shader_bytecode.h>


#include "context.h"
#include "pica.h"
#include "vertex_shader.h"
#include "debug_utils/debug_utils.h"

#include <framework/meta_tools.hpp>
#include <framework/exceptions.hpp>

#include <Tracy.hpp>
#include <TracyC.h>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include <iostream>
#include <variant>

using nihstro::OpCode;
using nihstro::Instruction;
using nihstro::RegisterType;
using nihstro::SourceRegister;
using nihstro::SwizzlePattern;

namespace Pica {

namespace VertexShader {

struct ShaderEngineImpl {
    struct MicroOp {
        // TODO: Instead, use some sort of index into a handler table!
        ShaderMicrocodeHandler handler;
        uint32_t arg0 = 0;
        uint32_t arg1 = 0;
        uint32_t arg2 = 0;
    };

    ShaderEngineImpl(Context& context, uint32_t entry_point);

    std::array<Math::Vec4<float24>*, 16> input_register_table;

    Math::Vec4<float24> dummy_register;

    ShaderCodeOffset entry_point;

    uint32_t num_input_attributes;

    MicroOp* next_micro_op = nullptr;

    std::array<Math::Vec4<float24>, 16> input_registers;

    Math::Vec4<float24> temporary_registers[16];

    std::array<Math::Vec4<float24>, 96> float_uniforms;

    Math::Vec4<float24> output_registers[16];

    // Two conditional codes are defined by the PICA200 architecture; the third one is for internal use
    bool conditional_code[3];

    // Four address registers:
    // * First one is always 0
    // * Two PICA200 address registers
    // * One PICA200 loop counter
    // The dummy register in the beginning allows for quick (branch-predictable) lookup of these registers
    // TODO: How many bits do these actually have?
    s32 address_registers[4] = { 0, 0, 0, 0 };

    s32& LoopRegister() {
        return address_registers[3];
    }

    enum {
        INVALID_ADDRESS = 0xFFFFFFFF
    };

    struct CallStackElement {
        MicroOpOffset final_address;  // Address upon which we jump to return_address
        MicroOpOffset return_address; // Where to jump when leaving scope
        u8 repeat_counter;  // How often to repeat until this call stack element is removed
        u8 loop_increment;  // Which value to add to the loop counter after an iteration
                            // TODO: Should this be a signed value? Does it even matter?
        MicroOpOffset loop_begin_address; // The start address of the current loop (0 if we're not in a loop)
    };

    // Parameters for next call stack element
    u8 next_repeat_counter = 0;
    u8 next_loop_increment = 0;

    // TODO: Is there a maximal size for this?
    // NOTE: This stack is never empty. (It will always at least contain a
    //       dummy element that always compares false against the current
    //       offset)
    boost::container::static_vector<CallStackElement, 16> call_stack;
    MicroOpOffset last_call_stack_address;

    struct {
        ShaderCodeOffset max_offset; // maximum program counter ever reached
        u32 max_opdesc_id; // maximum swizzle pattern index ever used
    } debug;

// TODO: Errr... don't use static here. But we use it currently to avoid redundant allocations!
    /*static */std::map<ShaderCodeOffset, MicroOpOffset> micro_op_block_offsets;
    /*static */std::vector<MicroOp> micro_ops;

    // TODO: Do this cleaner. Should be in an entirely different backend!
    std::string glsl;
};

// TODO: Context& parameter can be dropped once we drop LegacyInterpretShaderOpcode
template<auto Handler>
static void WrapDefaultMicroOpHandler(ShaderEngineImpl& engine, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    Handler(engine, arg1, arg2, arg3);

    auto next_micro_op = *engine.next_micro_op++;
    next_micro_op.handler(engine, next_micro_op.arg0, next_micro_op.arg1, next_micro_op.arg2);
}

/**
 * Handler that processes a sequence of non-control-flow micro ops at once, avoiding the need to re-read the next micro op pointer all the time.
 *
 * This allows for better processing efficiency, since the next micro op pointer doesn't need to be re-read every time. It allows keeps down call stack depth.
 */
static void BurstMicroOpHandler(ShaderEngineImpl& engine, uint32_t num_micro_ops, uint32_t, uint32_t) {
    for (auto micro_op_ptr : ranges::view::iota(engine.next_micro_op, engine.next_micro_op + num_micro_ops)) {
        micro_op_ptr->handler(engine, micro_op_ptr->arg0, micro_op_ptr->arg1, micro_op_ptr->arg2);
    }
    engine.next_micro_op += num_micro_ops;
}

static void EndMicroOpHandler(ShaderEngineImpl&, uint32_t, uint32_t, uint32_t) {
    // Do nothing (in particular, do not invoke the next handler)
}

template<bool IsLoopBegin>
static void MicroOpCall(ShaderEngineImpl& engine, MicroOpOffset target, uint32_t num_instructions, MicroOpOffset return_offset) {
    // TODO: It would be better to store relative offsets, so we can compute the next micro op solely from the current value of next_micro_op
    engine.next_micro_op = &engine.micro_ops[target.value];

    // TODO: Push return_offset to call stack
    if (engine.call_stack.size() >= engine.call_stack.capacity()) {
        throw std::runtime_error("Shader call stack exhausted");
    }
    engine.call_stack.push_back({   target.IncrementedBy(num_instructions), return_offset,
                                    IsLoopBegin ? engine.next_repeat_counter : uint8_t { 0 },
                                    IsLoopBegin ? engine.next_loop_increment : uint8_t { 0 },
                                    target });
    engine.last_call_stack_address = target.IncrementedBy(num_instructions);

    auto next_micro_op = *engine.next_micro_op++;
    next_micro_op.handler(engine, next_micro_op.arg0, next_micro_op.arg1, next_micro_op.arg2);
}

static void MicroOpGoto(ShaderEngineImpl& engine, MicroOpOffset target, uint32_t, uint32_t) {
    // TODO: Consider using relative addressing here: next_micro_op += relative_offset
    engine.next_micro_op = &engine.micro_ops[target.value];
    auto next_micro_op = *engine.next_micro_op++;
    next_micro_op.handler(engine, next_micro_op.arg0, next_micro_op.arg1, next_micro_op.arg2);
}

static void MicroOpBranch(ShaderEngineImpl& engine, uint32_t predicate_reg, MicroOpOffset target_if_true, MicroOpOffset target_if_false) {
    engine.next_micro_op = &engine.micro_ops[engine.conditional_code[predicate_reg] ? target_if_true.value : target_if_false.value];
    auto next_micro_op = *engine.next_micro_op++;
    next_micro_op.handler(engine, next_micro_op.arg0, next_micro_op.arg1, next_micro_op.arg2);
}

static void MicroOpPredicateNext(ShaderEngineImpl& engine, uint32_t predicate_reg, uint32_t, uint32_t) {
    if (!engine.conditional_code[predicate_reg]) {
        // Skip instruction
        ++engine.next_micro_op;
    }
}

template<auto F>
static void MicroOpTypeErased(ShaderEngineImpl& engine, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    F(engine, { arg1 }, { arg2 }, { arg3 });
}

// TODO: Rename to PopCallStack
static void MicroOpCheckCallStack(ShaderEngineImpl& engine, MicroOpOffset micro_op_offset, uint32_t, uint32_t) {
//    while (micro_op_offset == engine.last_call_stack_address) {
    if (true) {
        auto& top = engine.call_stack.back();
        engine.LoopRegister() += top.loop_increment;

        if (top.repeat_counter-- == 0) {
            micro_op_offset = top.return_address;
            engine.call_stack.pop_back();
        } else {
            micro_op_offset = top.loop_begin_address;
        }

        engine.next_micro_op = &engine.micro_ops[micro_op_offset.value];
        engine.last_call_stack_address = engine.call_stack.back().final_address;
    }

    auto micro_op = *engine.next_micro_op;
    ++engine.next_micro_op;

    // Invoke next instruction
    micro_op.handler(engine, micro_op.arg0, micro_op.arg1, micro_op.arg2);
}

template<Instruction::FlowControlType::Op Op>
static void MicroOpEvalCondition(ShaderEngineImpl& engine, uint32_t refx, uint32_t refy, uint32_t) {
    const bool results[2] = {
        refx == engine.conditional_code[0],
        refy == engine.conditional_code[1] };

    using OpType = Instruction::FlowControlType::Op;

    switch (Op) {
    case OpType::Or:
        engine.conditional_code[2] = (results[0] || results[1]);
        break;

    case OpType::And:
        engine.conditional_code[2] = (results[0] && results[1]);
        break;

    case OpType::JustX:
        engine.conditional_code[2] = results[0];
        break;

    case OpType::JustY:
        engine.conditional_code[2] = results[1];
        break;
    }
}

// Returns the index into state.conditional_code from which to read the condition result
static uint32_t EmitMicroOpEvalCondition(std::vector<ShaderEngineImpl::MicroOp>& micro_ops, Instruction::FlowControlType flow_control) {
    using Op = Instruction::FlowControlType::Op;
    if (flow_control.op == Op::JustX && flow_control.refx) {
        return 0;
    } else if (flow_control.op == Op::JustY && flow_control.refy) {
        return 1;
    } else {
        auto evaluator = std::invoke([&]() {
            switch (flow_control.op) {
            case Op::Or:
                return WrapDefaultMicroOpHandler<MicroOpEvalCondition<Op::Or>>;

            case Op::And:
                return WrapDefaultMicroOpHandler<MicroOpEvalCondition<Op::And>>;

            case Op::JustX:
                return WrapDefaultMicroOpHandler<MicroOpEvalCondition<Op::JustX>>;

            case Op::JustY:
                return WrapDefaultMicroOpHandler<MicroOpEvalCondition<Op::JustY>>;
            }
        });

        micro_ops.push_back({ evaluator, flow_control.refx.Value(), flow_control.refy.Value() });
        return 2;
    }
}

static void MicroOpPrepareLoop(ShaderEngineImpl& engine, uint32_t x, uint32_t y, uint32_t z) {
    engine.LoopRegister() = y;

    // TODO: uhh these are never used?
    engine.next_repeat_counter = x;
    engine.next_loop_increment = z;
}

static Math::Vec4<float24>& LookupRegister(ShaderEngineImpl& engine, uint32_t reg_index) noexcept {
    // NOTE: Output registers are relocated to offset 0x80 by the micro op compiler
    //       This is necessary since PICA200 places them at offset 0x0, making them collide with input register indexes
    return engine.input_registers[reg_index];
}

static std::array<float24, 4> GetSwizzledSourceRegister1(ShaderEngineImpl& engine, uint32_t reg_index,
                                                        bool negate, SwizzlePattern swizzle) {
    const auto& reg = LookupRegister(engine, reg_index);

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

static std::array<float24, 4> GetSwizzledSourceRegister2(ShaderEngineImpl& engine, uint32_t reg_index,
                                                        bool negate, SwizzlePattern swizzle) {
    const auto& reg = LookupRegister(engine, reg_index);

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

static float24 GetSwizzledSourceRegister1Comp(   ShaderEngineImpl& engine, uint32_t reg_index,
                                                 uint32_t component, bool negate, SwizzlePattern swizzle) {
    const auto& reg = LookupRegister(engine, reg_index);
    float24 ret = reg[static_cast<int>(swizzle.GetSelectorSrc1(component))];
    if (negate) {
        ret = ret * float24::FromFloat32(-1);
    }
    return ret;
}

static float24 GetSwizzledSourceRegister2Comp(   ShaderEngineImpl& engine, uint32_t reg_index,
                                                 uint32_t component, bool negate, SwizzlePattern swizzle) {
    const auto& reg = LookupRegister(engine, reg_index);
    float24 ret = reg[static_cast<int>(swizzle.GetSelectorSrc2(component))];
    if (negate) {
        ret = ret * float24::FromFloat32(-1);
    }
    return ret;
}

// TODO: Generate separate functions for all of the swizzle pattern combinations
template<bool BurstProcessed, auto BinOp>
static void MicroOpBinaryArithmetic(ShaderEngineImpl& engine, uint32_t src_info, uint32_t dst_index, SwizzlePattern swizzle) {
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
    auto src1 = GetSwizzledSourceRegister1(engine, std::clamp<uint32_t>(src1_reg_index + engine.address_registers[src1_addr_offset_index], 0, 127), swizzle.negate_src1, swizzle);
    auto src2 = GetSwizzledSourceRegister2(engine, src2_reg_index/* + engine.address_registers[src2_addr_offset_index]*/, swizzle.negate_src2, swizzle);

    Math::Vec4<float24>& dest = LookupRegister(engine, dst_index);

//    state.debug.max_opdesc_id = std::max<u32>(state.debug.max_opdesc_id, 1+instr.common.operand_desc_id);

    BinOp(src1, src2, dest, swizzle);

    if constexpr (!BurstProcessed) {
        auto next_micro_op = *engine.next_micro_op++;
        next_micro_op.handler(engine, next_micro_op.arg0, next_micro_op.arg1, next_micro_op.arg2);
    }
}

static void MicroOpADD(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

        dest[i] = src1[i] + src2[i];
    }
}

static void MicroOpMUL(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

        dest[i] = src1[i] * src2[i];
    }
}

static void MicroOpMAX(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

        dest[i] = (src1[i] > src2[i]) ? src1[i] : src2[i];
    }
}

static void MicroOpMIN(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

        dest[i] = (src1[i] < src2[i]) ? src1[i] : src2[i];
    }
}

template<int NumComponents>
static void MicroOpDP(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    float24 dot = float24::FromFloat32(0.f);
    for (int i = 0; i < NumComponents; ++i)
        dot = dot + src1[i] * src2[i];

    for (int i = 0; i < 4; ++i) {
        if (!swizzle.DestComponentEnabled(i))
            continue;

        dest[i] = dot;
    }
}

/**
 * Specialized version of MicroOpDP, which only assigns to one component in the target register.
 *
 * The component is encoded in the dest_mask field of the SwizzlePattern by the micro op compiler.
 */
template<int NumComponents>
static void MicroOpDPScalar(const std::array<float24, 4>& src1, const std::array<float24, 4>& src2, Math::Vec4<float24>& dest, SwizzlePattern swizzle) {
    float24 dot = float24::FromFloat32(0.f);
    for (int i = 0; i < NumComponents; ++i)
        dot = dot + src1[i] * src2[i];

    dest[swizzle.dest_mask] = dot;
}

/**
 * If IsScalar is true, use a specialized version that only assigns to one component in the target register.
 *
 * In that case, the component index is encoded in the dest_mask field of the SwizzlePattern by the micro op compiler. Also, the pre-swizzled component of src1 is encoded in the src1_selector_0 field
 */
template<bool IsScalar>
static void MicroOpMOV(ShaderEngineImpl& engine, uint32_t src_info, uint32_t dst_index, SwizzlePattern swizzle) {
    auto src1_reg_index = src_info & 0xff;
    auto src1_addr_offset_index = static_cast<uint8_t>((src_info >> 8) & 0xff);

    Math::Vec4<float24>& dest = LookupRegister(engine, dst_index);

    auto clamped_reg_index = std::clamp<uint32_t>(src1_reg_index + engine.address_registers[src1_addr_offset_index], 0, 127);

    if constexpr (IsScalar) {
        uint32_t component = swizzle.dest_mask;
        auto src1 = LookupRegister(engine, clamped_reg_index)[static_cast<int>(swizzle.src1_selector_0.Value())];
        if (swizzle.negate_src1) {
            src1 = src1 * float24::FromFloat32(-1);
        }
        dest[component] = src1;
    } else {
        auto src1 = GetSwizzledSourceRegister1(engine, clamped_reg_index, swizzle.negate_src1, swizzle);
        for (int i = 0; i < 4; ++i) {
            if (!swizzle.DestComponentEnabled(i))
                continue;

            // TODO: Hint norestrict to the compiler?
            dest[i] = src1[i];
        }
    }
}

template<template<typename> class Comp>
static void MicroOpCMP(ShaderEngineImpl& engine, uint32_t src_info, uint32_t component, SwizzlePattern swizzle) {
    auto src1_reg_index = src_info & 0xff;
    auto src2_reg_index = (src_info >> 16) & 0xff;
    auto src1_addr_offset_index = static_cast<uint8_t>((src_info >> 8) & 0xff);
//    auto src2_addr_offset_index = static_cast<uint8_t>((src_info >> 24) & 0xff);

    auto src1 = GetSwizzledSourceRegister1Comp(engine, src1_reg_index + engine.address_registers[src1_addr_offset_index], component, swizzle.negate_src1, swizzle);
    auto src2 = GetSwizzledSourceRegister2Comp(engine, src2_reg_index/* + engine.address_registers[src2_addr_offset_index]*/, component, swizzle.negate_src2, swizzle);

    // NOTE: "swizzle" is actually the component index to compare. Note that contrary to the PICA200 CMP instruction, this micro op only compares a single component
    // TODO: Use a dedicated function for this instead of re-using MicroOpBinaryArithmetic
    engine.conditional_code[component] = Comp<float24>{}(src1, src2);
}

static void LegacyInterpretShaderOpcode(ShaderEngineImpl& engine, uint32_t instr_raw, uint32_t swizzle_raw, uint32_t) {
    auto instr = nihstro::Instruction { instr_raw };

    // Placeholder for invalid inputs
    Math::Vec4<float24> dummy_vec4_float24;

    const SwizzlePattern swizzle { swizzle_raw };

    auto LookupSourceRegister = [&](const SourceRegister& source_reg) -> Math::Vec4<float24>& {
        return engine.input_registers[source_reg];
    };

    switch (instr.opcode.Value().GetInfo().type) {
    case OpCode::Type::Arithmetic:
    {
        bool is_inverted = 0 != (instr.opcode.Value().GetInfo().subtype & OpCode::Info::SrcInversed);
        if (is_inverted) {
            // TODO: We don't really support this properly: For instance, the address register
            //       offset needs to be applied to SRC2 instead, etc.
            //       For now, we just abort in this situation.
            LOG_CRITICAL(HW_GPU, "Bad condition...");
            exit(0);
        }

        const int address_offset = engine.address_registers[instr.common.address_register_index];

        const auto& src1_ = LookupSourceRegister(instr.common.GetSrc1(is_inverted) + (is_inverted ? 0 : address_offset));
        const auto& src2_ = LookupSourceRegister(instr.common.GetSrc2(is_inverted) + (is_inverted ? address_offset : 0));

        const bool negate_src1 = ((bool)swizzle.negate_src1 != false);
        const bool negate_src2 = ((bool)swizzle.negate_src2 != false);

        float24 src1[4] = {
            src1_[(int)swizzle.GetSelectorSrc1(0)],
            src1_[(int)swizzle.GetSelectorSrc1(1)],
            src1_[(int)swizzle.GetSelectorSrc1(2)],
            src1_[(int)swizzle.GetSelectorSrc1(3)],
        };
        if (negate_src1) {
            src1[0] = src1[0] * float24::FromFloat32(-1);
            src1[1] = src1[1] * float24::FromFloat32(-1);
            src1[2] = src1[2] * float24::FromFloat32(-1);
            src1[3] = src1[3] * float24::FromFloat32(-1);
        }
        float24 src2[4] = {
            src2_[(int)swizzle.GetSelectorSrc2(0)],
            src2_[(int)swizzle.GetSelectorSrc2(1)],
            src2_[(int)swizzle.GetSelectorSrc2(2)],
            src2_[(int)swizzle.GetSelectorSrc2(3)],
        };
        if (negate_src2) {
            src2[0] = src2[0] * float24::FromFloat32(-1);
            src2[1] = src2[1] * float24::FromFloat32(-1);
            src2[2] = src2[2] * float24::FromFloat32(-1);
            src2[3] = src2[3] * float24::FromFloat32(-1);
        }

        Math::Vec4<float24>& dest = (instr.common.dest.Value() < 0x10) ? engine.output_registers[instr.common.dest.Value().GetIndex()]
                                    : (instr.common.dest.Value() < 0x20) ? engine.temporary_registers[instr.common.dest.Value().GetIndex()]
                                        : dummy_vec4_float24;

        engine.debug.max_opdesc_id = std::max<u32>(engine.debug.max_opdesc_id, 1+instr.common.operand_desc_id);

        switch (instr.opcode.Value().EffectiveOpCode()) {
        case OpCode::Id::ADD:
        {
            for (int i = 0; i < 4; ++i) {
                if (!swizzle.DestComponentEnabled(i))
                    continue;

                dest[i] = src1[i] + src2[i];
            }

            break;
        }

        case OpCode::Id::MUL:
        {
            for (int i = 0; i < 4; ++i) {
                if (!swizzle.DestComponentEnabled(i))
                    continue;

                dest[i] = src1[i] * src2[i];
            }

            break;
        }

        case OpCode::Id::MAX:
            for (int i = 0; i < 4; ++i) {
                if (!swizzle.DestComponentEnabled(i))
                    continue;

                // NOTE: Secondary changes from Citra PR #1065. Not needed for OoT3D UI
//                    dest[i] = std::max(src1[i], src2[i]);
                dest[i] = (src1[i] > src2[i]) ? src1[i] : src2[i];
            }
            break;

        case OpCode::Id::MIN:
            for (int i = 0; i < 4; ++i) {
                if (!swizzle.DestComponentEnabled(i))
                    continue;

                // NOTE: Secondary changes from Citra PR #1065. Not needed for OoT3D UI
//                    dest[i] = std::min(src1[i], src2[i]);
                dest[i] = (src1[i] < src2[i]) ? src1[i] : src2[i];
            }
            break;

        case OpCode::Id::DP3:
        case OpCode::Id::DP4:
        {
            float24 dot = float24::FromFloat32(0.f);
            int num_components = (instr.opcode.Value() == OpCode::Id::DP3) ? 3 : 4;
            for (int i = 0; i < num_components; ++i)
                dot = dot + src1[i] * src2[i];

            for (int i = 0; i < 4; ++i) {
                if (!swizzle.DestComponentEnabled(i))
                    continue;

                dest[i] = dot;
            }
            break;
        }

        // Reciprocal
        case OpCode::Id::RCP:
        {
            // The reciprocal is computed for the first component and duplicated to all output elements
            // TODO: Be stable against division by zero!
            float24 result = float24::FromFloat32(1.0f / src1[0].ToFloat32());
            for (int i = 0; i < 4; ++i) {
                if (!swizzle.DestComponentEnabled(i))
                    continue;

                dest[i] = result;
            }

            break;
        }

        // Reciprocal Square Root
        case OpCode::Id::RSQ:
        {
            // The result is computed for the first component and duplicated to all output elements
            // TODO: Be stable against division by zero!
            float24 result = float24::FromFloat32(1.0f / sqrt(src1[0].ToFloat32()));
            for (int i = 0; i < 4; ++i) {
                if (!swizzle.DestComponentEnabled(i))
                    continue;

                dest[i] = result;
            }

            break;
        }

        case OpCode::Id::MOVA:
        {
            for (int i = 0; i < 2; ++i) {
                if (!swizzle.DestComponentEnabled(i))
                    continue;

                // TODO: Figure out how the rounding is done on hardware
                engine.address_registers[i + 1] = static_cast<s32>(src1[i].ToFloat32());
            }

            break;
        }

        case OpCode::Id::MOV:
        {
            for (int i = 0; i < 4; ++i) {
                if (!swizzle.DestComponentEnabled(i))
                    continue;

                dest[i] = src1[i];
            }
            break;
        }

        case OpCode::Id::CMP:
            for (int i = 0; i < 2; ++i) {
                // TODO: Can you restrict to one compare via dest masking?

                auto compare_op = instr.common.compare_op;
                auto op = (i == 0) ? compare_op.x.Value() : compare_op.y.Value();

                switch (op) {
                    case compare_op.Equal:
                        engine.conditional_code[i] = (src1[i] == src2[i]);
                        break;

                    case compare_op.NotEqual:
                        engine.conditional_code[i] = (src1[i] != src2[i]);
                        break;

                    case compare_op.LessThan:
                        engine.conditional_code[i] = (src1[i] <  src2[i]);
                        break;

                    case compare_op.LessEqual:
                        engine.conditional_code[i] = (src1[i] <= src2[i]);
                        break;

                    case compare_op.GreaterThan:
                        engine.conditional_code[i] = (src1[i] >  src2[i]);
                        break;

                    case compare_op.GreaterEqual:
                        engine.conditional_code[i] = (src1[i] >= src2[i]);
                        break;

                    default:
                        LOG_ERROR(HW_GPU, "Unknown compare mode %x", static_cast<int>(op));
                        break;
                }
            }
            break;

        default:
            LOG_ERROR(HW_GPU, "Unhandled arithmetic instruction: 0x%02x (%s): 0x%08x",
                      (int)instr.opcode.Value().EffectiveOpCode(), instr.opcode.Value().GetInfo().name, instr.hex);
            _dbg_assert_(HW_GPU, 0);
            break;
        }

        break;
    }

    case OpCode::Type::MultiplyAdd:
    {
        if (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MAD) {
            const bool is_inverted = false;

            const int address_offset = engine.address_registers[instr.mad.address_register_index];

            const auto& src1_ = LookupSourceRegister(instr.mad.GetSrc1(is_inverted));
            const auto& src2_ = LookupSourceRegister(instr.mad.GetSrc2(is_inverted) + (is_inverted ? 0 : address_offset));
            const auto& src3_ = LookupSourceRegister(instr.mad.GetSrc3(is_inverted) + (is_inverted ? address_offset : 0));

            const bool negate_src1 = ((bool)swizzle.negate_src1 != false);
            const bool negate_src2 = ((bool)swizzle.negate_src2 != false);
            const bool negate_src3 = ((bool)swizzle.negate_src3 != false);

            float24 src1[4] = {
                src1_[(int)swizzle.GetSelectorSrc1(0)],
                src1_[(int)swizzle.GetSelectorSrc1(1)],
                src1_[(int)swizzle.GetSelectorSrc1(2)],
                src1_[(int)swizzle.GetSelectorSrc1(3)],
            };
            if (negate_src1) {
                src1[0] = src1[0] * float24::FromFloat32(-1);
                src1[1] = src1[1] * float24::FromFloat32(-1);
                src1[2] = src1[2] * float24::FromFloat32(-1);
                src1[3] = src1[3] * float24::FromFloat32(-1);
            }
            float24 src2[4] = {
                src2_[(int)swizzle.GetSelectorSrc2(0)],
                src2_[(int)swizzle.GetSelectorSrc2(1)],
                src2_[(int)swizzle.GetSelectorSrc2(2)],
                src2_[(int)swizzle.GetSelectorSrc2(3)],
            };
            if (negate_src2) {
                src2[0] = src2[0] * float24::FromFloat32(-1);
                src2[1] = src2[1] * float24::FromFloat32(-1);
                src2[2] = src2[2] * float24::FromFloat32(-1);
                src2[3] = src2[3] * float24::FromFloat32(-1);
            }
            float24 src3[4] = {
                src3_[(int)swizzle.GetSelectorSrc3(0)],
                src3_[(int)swizzle.GetSelectorSrc3(1)],
                src3_[(int)swizzle.GetSelectorSrc3(2)],
                src3_[(int)swizzle.GetSelectorSrc3(3)],
            };
            if (negate_src3) {
                src3[0] = src3[0] * float24::FromFloat32(-1);
                src3[1] = src3[1] * float24::FromFloat32(-1);
                src3[2] = src3[2] * float24::FromFloat32(-1);
                src3[3] = src3[3] * float24::FromFloat32(-1);
            }

            Math::Vec4<float24>& dest = (instr.mad.dest.Value() < 0x10) ? engine.output_registers[instr.mad.dest.Value().GetIndex()]
                                        : (instr.mad.dest.Value() < 0x20) ? engine.temporary_registers[instr.mad.dest.Value().GetIndex()]
                                            : dummy_vec4_float24;

            for (int i = 0; i < 4; ++i) {
                if (!swizzle.DestComponentEnabled(i))
                    continue;

                dest[i] = src1[i] * src2[i] + src3[i];
            }
        } else {
            throw std::runtime_error("Unhandled multiply-add instruction");
        }
        break;
    }

    default:
    {
        // Handle each instruction on its own
        switch (instr.opcode.Value()) {
        case OpCode::Id::NOP:
            break;

        default:
            LOG_ERROR(HW_GPU, "Unhandled instruction: 0x%02x (%s): 0x%08x",
                      (int)instr.opcode.Value().EffectiveOpCode(), instr.opcode.Value().GetInfo().name, instr.hex);
            break;
        }

        break;
    }
    }
}


// Map starting instruction offset to block length
struct AnalyzedProgram {
    enum class SuccessionVia {
        BranchToIf,   // Branch to if-body
        BranchToElse, // Branch to else-body (or past the conditional if else-body is empty)
        Fallthrough,  // Continue at the next PICA200 instruction
        Return,       // Return from function to caller
        JumpTo,       // Jump to another offset
        ExitLoop,     // Exit from a PICA200 loop
        LoopEnd,      // Last block in a PICA200 loop
        CondJumpTo,   // Conditional Jump
    };

    struct PostExecJump {
        static PostExecJump PopCallStack() {
            PostExecJump ret;
            ret.target = CallStackTag { };
            return ret;
        }

        static PostExecJump ReturnTo(ShaderCodeOffset target) {
            PostExecJump ret;
            ret.target = target;
            return ret;
        }

        bool FromCallStack() const {
            return std::holds_alternative<CallStackTag>(target);
        }

        std::optional<ShaderCodeOffset> GetStaticTarget() const {
            auto* static_target = std::get_if<ShaderCodeOffset>(&target);
            if (!static_target) {
                return std::nullopt;
            }
            return *static_target;
        }

        explicit operator bool() const {
            return !std::holds_alternative<std::monostate>(target);
        }

    private:
        struct CallStackTag { };
        std::variant<std::monostate, CallStackTag, ShaderCodeOffset> target;
    };

    struct BlockInfo {
        uint32_t length = 0xffffffff; // 0xffffffff to indicate no known end bound

        std::set<ShaderCodeOffset> predecessors;

        // Block successors (excluding function calls as these are tracked in call_targets)
        std::map<ShaderCodeOffset, SuccessionVia> successors;

        // Optional target to implicitly jump to at the end of the block
        PostExecJump post_exec_jump;

        // First instruction that is hit by all code paths following this block
        ShaderCodeOffset post_dominator = { 0xffffffff };
    };

    std::map<ShaderCodeOffset, BlockInfo> blocks;

    struct CallInfo {
        ShaderCodeOffset target;
        ShaderCodeOffset return_site;
    };

    // Map from call instruction offset to call target and return site
    std::map<ShaderCodeOffset, CallInfo> call_targets;

    struct Function {
        ShaderCodeOffset entry;
        ShaderCodeOffset exit;
        std::set<ShaderCodeOffset> blocks;
    };

    std::vector<Function> functions;

    ShaderCodeOffset entry_point;
};

static void ParseShaderCode(Context& context, AnalyzedProgram& analyzed_program, std::set<ShaderCodeOffset> dirty_blocks) {
    using SuccessionVia = AnalyzedProgram::SuccessionVia;

    std::map<ShaderCodeOffset, AnalyzedProgram::BlockInfo>& blocks = analyzed_program.blocks;

    auto& shader_uniforms = context.shader_uniforms;

    while (!dirty_blocks.empty()) {
        const ShaderCodeOffset block_start = *dirty_blocks.begin();
        dirty_blocks.erase(dirty_blocks.begin());
        ShaderCodeOffset offset = block_start;
        auto& block_info = blocks.at(offset);
        auto& current_block_length = block_info.length;
        current_block_length = 0;

        auto add_call = [&](ShaderCodeOffset function_entry, ShaderCodeOffset function_exit) {
            auto block_offsets = blocks | ranges::views::keys;
            auto it = ranges::upper_bound(block_offsets, function_entry, [](auto a, auto b) { return !(a < b || a == b); });
            if (it != ranges::end(block_offsets) && !(*it < function_entry)) {
                throw std::runtime_error("Yo wtf");
            }
            if (it != ranges::end(block_offsets) && function_entry < it->IncrementedBy(blocks.at(*it).length)) {
                throw std::runtime_error("Need to implement block splitting");
            }

            auto [target_block_it, inserted] = blocks.insert({ function_entry, AnalyzedProgram::BlockInfo { } });
            if (inserted) {
                dirty_blocks.insert(function_entry);
            }

            analyzed_program.call_targets.insert({ function_entry, AnalyzedProgram::CallInfo { offset, function_exit } });
        };

        auto add_successor = [&](ShaderCodeOffset successor_offset, AnalyzedProgram::SuccessionVia reason) {
            auto [target_block_it, inserted] = blocks.insert({ successor_offset, AnalyzedProgram::BlockInfo { } });
            if (inserted) {
                dirty_blocks.insert(successor_offset);
            }

            block_info.successors.insert({ successor_offset, reason });
            target_block_it->second.predecessors.insert(block_start);
        };

        // Used for registering single-instruction blocks that need to check
        // the call stack (e.g. the last instruction in an if-body, or the
        // last instruction in a function)
        auto add_return_site = [&blocks, &dirty_blocks](ShaderCodeOffset return_site, ShaderCodeOffset return_target, bool is_conditional) {
            // TODO: Do a proper lookup for blocks containing return_site instead! If necessary, split the block at return_site
            auto [block_it, inserted] = blocks.insert({ return_site, AnalyzedProgram::BlockInfo { } });
            if (!inserted) {
                // TODO: For function return sites, this is fine.
//                throw std::runtime_error(fmt::format("Error: Function/Conditional at {:#x} has already been analyzed", 4 * return_site.value));
            } else {
                block_it->second.length = 1;
                dirty_blocks.insert(return_site);
            }

            // For conditionals, encode a static return target.
            // For function calls, encode a call stack pop instead but note
            // down the return target.
            block_it->second.post_exec_jump = is_conditional ? AnalyzedProgram::PostExecJump::ReturnTo(return_target) : AnalyzedProgram::PostExecJump::PopCallStack();
            if (is_conditional) {
                block_it->second.successors.insert({ return_target, is_conditional ? SuccessionVia::JumpTo : SuccessionVia::Return });
            }

            auto [target_block_it, inserted2] = blocks.insert({ return_target, AnalyzedProgram::BlockInfo { } });
            if (inserted2) {
                dirty_blocks.insert(return_target);
            }
            if (is_conditional) {
                target_block_it->second.predecessors.insert(return_site);
            }
        };

        bool end_block = false;
        while (!end_block) {
            const Instruction instr = offset.ReadFrom(context.shader_memory);

            if (offset != block_start && blocks.count(offset)) {
                // Fall through into an existing block.
                // This happens e.g. for the end of branch bodies, which is
                // registered as a single-instruction block to register the
                // post-branch address
                end_block = true;
                add_successor(offset, SuccessionVia::Fallthrough);
                break;
            }

            switch (instr.opcode.Value().GetInfo().type) {
            case OpCode::Type::Arithmetic:
            case OpCode::Type::MultiplyAdd:
                ++current_block_length;
                break;

            default:
            {
                // Handle each instruction on its own
                switch (instr.opcode.Value()) {
                // TODO: Decide whether to handle this in here or in the legacy processor...
                case OpCode::Id::NOP:
                    ++current_block_length;
                    break;

                case OpCode::Id::END:
                    ++current_block_length;
                    end_block = true;
                    analyzed_program.functions.push_back({ analyzed_program.entry_point, offset, {} });
                    break;

                case OpCode::Id::JMPC:
                    ++current_block_length;
                    end_block = true;
                    add_successor(ShaderCodeOffset { instr.flow_control.dest_offset }, SuccessionVia::CondJumpTo);
                    add_successor(offset.GetNext(), SuccessionVia::Fallthrough);
                    break;

                case OpCode::Id::JMPU:
                    ++current_block_length;
                    // TODO: Apart from b15, these conditions can be evaluated statically!
                    // NOTE: The lowest bit of the num_instructions field indicates an inverted condition for this instruction.
                    // TODO: Add a better-named field alias for this case
                    if (shader_uniforms.b[instr.flow_control.bool_uniform_id] == !(instr.flow_control.num_instructions & 1)) {
                        add_successor(ShaderCodeOffset { instr.flow_control.dest_offset }, SuccessionVia::JumpTo);
                    } else {
                        add_successor(offset.GetNext(), SuccessionVia::Fallthrough);
                    }
                    end_block = true;
                    break;

                case OpCode::Id::CALL:
                case OpCode::Id::CALLU:
                case OpCode::Id::CALLC:
                {
                    ++current_block_length;
                    auto target = ShaderCodeOffset { instr.flow_control.dest_offset };
                    // NOTE: For CALLU, the condition can be evaluated statically for uniforms other than b15 (geometry shader)!
                    // TODO: Handle b15!!
                    if (instr.opcode.Value() != OpCode::Id::CALLU || shader_uniforms.b[instr.flow_control.bool_uniform_id]) {
                        auto return_site = target.IncrementedBy(instr.flow_control.num_instructions);
                        add_return_site(return_site.GetPrevious(), offset.GetNext(), false);
                        add_call(target, return_site.GetPrevious());
                    }

                    add_successor(offset.GetNext(), SuccessionVia::Fallthrough);

                    end_block = true;
                    break;
                }

                // TODO: We must must find a way to contract these to a call-less expression
                case OpCode::Id::IFU:
                case OpCode::Id::IFC:
                {
                    ++current_block_length;
                    end_block = true;

                    auto else_offset = ShaderCodeOffset { instr.flow_control.dest_offset };
                    auto branch_end = else_offset.IncrementedBy(instr.flow_control.num_instructions);

                    // TODO: For IFU, the condition can be evaluated statically for uniforms other than b15 (geometry shader)!
                    // "If" case
                    if (instr.opcode.Value() != OpCode::Id::IFU || shader_uniforms.b[instr.flow_control.bool_uniform_id]) {
                        add_return_site(else_offset.GetPrevious(), branch_end, true);
                        add_successor(offset.GetNext(), SuccessionVia::BranchToIf);
                    }

                    // "else" case (if non-empty)
                    if (instr.opcode.Value() != OpCode::Id::IFU || !shader_uniforms.b[instr.flow_control.bool_uniform_id]) {
                        if (!(else_offset == branch_end)) {
                            add_successor(else_offset, SuccessionVia::BranchToElse);
                        } else {
                            // Post-branch
                            add_successor(branch_end, SuccessionVia::JumpTo);
                        }
                    }

                    break;
                }

                case OpCode::Id::BREAKC:
                    throw std::runtime_error("TODO breakc");
                    // TODO
                    ++current_block_length;
                    break;

                case OpCode::Id::LOOP:
                {
                    // Next basic block starts *after* the LOOP instruction
                    ++current_block_length;
                    end_block = true;

                    auto loop_end = ShaderCodeOffset { instr.flow_control.dest_offset }.GetNext();
                    add_return_site(loop_end.GetPrevious(), offset.GetNext(), false);

                    add_successor(offset.GetNext(), SuccessionVia::JumpTo);
                    add_successor(loop_end, SuccessionVia::ExitLoop);

                    blocks.at(loop_end.GetPrevious()).successors.insert({ loop_end, SuccessionVia::LoopEnd });
                    blocks.at(loop_end).predecessors.insert(loop_end.GetPrevious());

                    break;
                }

                default:
                    throw std::runtime_error("Unhandled shader instruction: " + std::to_string(Meta::to_underlying(instr.opcode.Value().EffectiveOpCode())));
                }

                break;
            }
            }

            // This only gets set if this is a single-instruction block or we merged with one
            if (block_info.post_exec_jump) {
                end_block = true;
            }

            offset = offset.GetNext();
        }

        block_info.length = offset - block_start;
    }
}

static uint32_t ProjectForMicroOp(uint32_t value) {
    return value;
}

static uint32_t ProjectForMicroOp(MicroOpOffset offset) {
    return offset.value;
}

static uint32_t ProjectForMicroOp(ShaderCodeOffset offset) {
    return offset.value;
}

static uint32_t ProjectForMicroOp(SwizzlePattern swizzle) {
    return swizzle.hex;
}

/**
 * Some micro code take micro code offsets, but only PICA200 shader offsets are
 * known upon generating them, so we store a ShaderCodeOffset as a
 * MicroOpOffset and later patch it up manually
 */
template<typename T>
using CoerceShaderOffsets = std::conditional_t<std::is_same_v<T, ShaderCodeOffset>, MicroOpOffset, T>;

template<auto F, typename T1 = uint32_t, typename T2 = uint32_t, typename T3 = uint32_t>
static void AddMicroOp(std::vector<ShaderEngineImpl::MicroOp>& micro_ops, T1 t1 = {}, T2 t2 = {}, T3 t3 = {}) {
    static_assert(std::is_invocable_v<  decltype(F), ShaderEngineImpl&,
                                        CoerceShaderOffsets<T1>, CoerceShaderOffsets<T2>,
                                        CoerceShaderOffsets<T3>>);
    micro_ops.push_back(ShaderEngineImpl::MicroOp { MicroOpTypeErased<F>, ProjectForMicroOp(t1), ProjectForMicroOp(t2), ProjectForMicroOp(t3) });
}

template<auto F>
auto GetBinaryMicroOp(bool is_burst_processed) {
    return (is_burst_processed
            ? &MicroOpTypeErased<MicroOpBinaryArithmetic<true, F>>
            : &MicroOpTypeErased<MicroOpBinaryArithmetic<false, F>>);
}

template<auto F>
auto GetCMPMicroOp(bool is_burst_processed) {
    return (is_burst_processed
            ? &MicroOpTypeErased<F>
            : &WrapDefaultMicroOpHandler<MicroOpTypeErased<F>>);
}

// TODO: Return this to the caller instead so we can output a nice control flow graph
static AnalyzedProgram analyzed_program;

// TODO: Get rid of this
std::string global_vertex_shader_code_todo;

static void ProcessShaderCode(Context& context, std::map<ShaderCodeOffset, MicroOpOffset>& micro_op_block_offsets, std::vector<ShaderEngineImpl::MicroOp>& micro_ops, ShaderCodeOffset entry_point) {
    auto& shader_uniforms = context.shader_uniforms;

    analyzed_program.call_targets.clear(); // avoid allocations. TODO: Remove
    analyzed_program.functions.clear(); // avoid allocations. TODO: Remove
    analyzed_program.blocks.clear(); // avoid allocations. TODO: Remove
    analyzed_program.blocks[entry_point] = { };
    std::set<ShaderCodeOffset> dirty_blocks { entry_point };
    analyzed_program.entry_point = entry_point;
    ParseShaderCode(context, analyzed_program, dirty_blocks);

    // TODO: Optimization passes:
    // * Inline single-instruction "Goto" blocks with one possible successor into call instructions that refer to that block's offset
    // * Merge single predecessor blocks
    // * Turn "CheckCallStack" into "PopCallStack" if there's only a single path from CALL to the current instruction
    // * Turn CheckCallStack at end of if-block into static jump to post-branch

    // Optimize CFG: Merge adjacent blocks with linear connection together
    {
        // Contract the blocks "A" and "B" in subgraphs of the following form:
        //           A
        //           │
        //       ┌───B───┐
        //       │   │   │
        //       C   D   E
        // into
        //       ┌───A'──┐
        //       │   │   │
        //       C   D   E
        // where A and B have been merged into a single block A'.
        //
        // The requirement for this optimization is that A and B are linearly
        // connected, i.e.:
        // * B is A's only successor
        // * A is B's only predecessor
        // * A has no non-trivial PostExecJump
        // * A and B have been generated from adjacent blocks of PICA200 code
        // * A may not end in a call instruction

        //
        // TODO: Drop the 4th requirement by enabling CFG blocks to store
        //       multiple PICA200 blocks instead of just a single one
        for (auto block_iter = analyzed_program.blocks.begin(), next_block_iter = std::next(block_iter); block_iter != analyzed_program.blocks.end(); block_iter = next_block_iter++) {
            auto& block = *block_iter; // Node B
            if (block.second.predecessors.size() == 1) {
                const auto pred_block_offset = *block.second.predecessors.begin();
                auto& pred_block = analyzed_program.blocks.at(pred_block_offset); // Node A (will merge B into this to get A')
                if (pred_block.successors.size() != 1 || pred_block_offset.IncrementedBy(pred_block.length) != block.first) {
                    continue;
                }

                // For CALL instructions, we need a mapping between PICA200
                // offsets and micro op offsets. For simplicity, we ensure this
                // by avoiding merging blocks for now.
                auto last_pred_instr = nihstro::Instruction { pred_block_offset.IncrementedBy(pred_block.length - 1).ReadFrom(context.shader_memory) };
                auto last_pred_instr_op = last_pred_instr.opcode.Value().EffectiveOpCode();
                if (last_pred_instr_op == OpCode::Id::CALL || last_pred_instr_op == OpCode::Id::CALLU || last_pred_instr_op == OpCode::Id::CALLC) {
                    continue;
                }
                // Same issue for LOOP
                if (last_pred_instr_op == OpCode::Id::LOOP) {
                    continue;
                }
                // Same issue for JMP
                if (last_pred_instr_op == OpCode::Id::JMPU || last_pred_instr_op == OpCode::Id::JMPC) {
                    continue;
                }


                if (pred_block.successors.begin()->second != AnalyzedProgram::SuccessionVia::Fallthrough) {
                    continue;
                }

                if (pred_block.post_exec_jump) {
                    // Cannot merge into a predecessor that has a non-trivial block connection
                    if (pred_block.post_exec_jump.GetStaticTarget() == block.first) {
                        pred_block.post_exec_jump = AnalyzedProgram::PostExecJump { };
                    } else {
                        continue;
                    }
                }

                // Update PostExecJump action for the merged node A'
                if (block.second.post_exec_jump) {
                    pred_block.post_exec_jump = block.second.post_exec_jump;
                }

                // Repoint predecessor references in successors (C/D/E) from B to the merged node A'
                for (auto& successor_offset : block.second.successors) { // Nodes C, D, and E
                    auto& successor = analyzed_program.blocks.at(successor_offset.first);
                    successor.predecessors.erase(block.first);
                    auto [it, inserted] = successor.predecessors.insert(pred_block_offset);
                    Mikage::ValidateContract(inserted);
                }

                // Repoint successor references in the predecessor (A) from B to B's successors
                pred_block.successors.erase(block.first);
                pred_block.successors.merge(block.second.successors);
                pred_block.length += block.second.length;

                next_block_iter = analyzed_program.blocks.erase(block_iter);
            }
        }
    }

    auto ComputePostDominator = [](ShaderCodeOffset block_offset, const auto& recurse) -> ShaderCodeOffset {
        auto& block = analyzed_program.blocks.at(block_offset);
        auto successor_offset = block.successors.begin()->first;

        if (block.successors.size() == 1) {
            // Not a conditional, hence nothing to do
            return block.successors.begin()->first;
        }

        // If there is more than one successor, it must be a conditional
        // (and hence there must be exactly 2 successors)
        Mikage::ValidateContract(block.successors.size() == 2);

        // Depth-first search to find prospective post-dominator
        while (true) {
            auto& successor_info = analyzed_program.blocks.at(successor_offset);

            if (successor_info.predecessors.size() == 2) {
                // This is the convergence point of the two branches
                return successor_offset;
            }

            if (successor_info.successors.size() == 2) {
                // Nested conditional. Compute post-dominators recursively and then skip past this branch
                successor_offset = recurse(successor_offset, recurse);
            } else {
                Mikage::ValidateContract(successor_info.successors.size() == 1);
                successor_offset = successor_info.successors.begin()->first;
            }
        }
    };

    {
        for (auto& block : analyzed_program.blocks) {
            // Skip blocks that have been optimized away but not removed from the CFG
            // TODO: Should not be necessary anymore, now that we do remove these blocks
            if (block.second.successors.size() == 0) {
                continue;
            }

            // Skip return sites
            auto is_call_stack_pop = [](AnalyzedProgram::SuccessionVia reason) { return reason == AnalyzedProgram::SuccessionVia::Return; };
            if (ranges::all_of(block.second.successors | ranges::views::values, is_call_stack_pop)) {
                continue;
            }
            if (block.second.successors.size() == 2) {
                block.second.post_dominator = ComputePostDominator(block.first, ComputePostDominator);
                fmt::print(stderr, "Found post-dominator {:#x} for {:#x}\n", 4 * block.second.post_dominator.value, 4 * block.first.value);
            }
        }
    }

    // Detect functions
    {
        auto& functions = analyzed_program.functions;

        // First, find all blocks to functions and append their callees to the function list
        for (auto& [function_entry, call_info] : analyzed_program.call_targets) {
            auto function_it = ranges::find_if(functions, [entry=function_entry](auto& f) { return f.entry == entry; });
            if (function_it == functions.end()) {
                functions.push_back({ function_entry, call_info.return_site, {} });
            } else if (function_it->exit != call_info.return_site) {
                throw std::runtime_error("A function at this offset already exists with different parameters");
            }
        }

        // Second, do a depth-first search to verify all paths from the function entry converge at its exit
        auto check_convergence = [&](ShaderCodeOffset start, ShaderCodeOffset end, std::set<ShaderCodeOffset>& function_blocks, const auto& recurse) -> bool {
            // Traverse linear chain of blocks
            auto is_same_block = [/*&analyzed_program*/](ShaderCodeOffset block_start, ShaderCodeOffset b) {
                auto block_end = block_start.IncrementedBy(analyzed_program.blocks.at(block_start).length);
                return !(b < block_start) && (b < block_end);
            };
            for (; !is_same_block(start, end);) {
                auto& block = analyzed_program.blocks.at(start);
                auto first_successor_it = block.successors.begin();

                function_blocks.insert(start);

                if (block.successors.size() == 1) {
                    start = first_successor_it->first;
                } else {
                    // Move on to non-linear traversal
                    break;
                }
            }

            function_blocks.insert(start);
            auto& block = analyzed_program.blocks.at(start);

            if (is_same_block(start, end)) {
                return true;
            } else if (block.successors.empty()) {
                // Reached end of the program without convergence
                return false;
            } else {
                // Continue with depth-first traversal to make sure all paths are convergent towards "end"
                return ranges::all_of(  block.successors | ranges::views::keys,
                                        [&](ShaderCodeOffset offset) { return recurse(offset, end, function_blocks, recurse); });
            }
        };

        for (AnalyzedProgram::Function& func : functions) {
            if (!check_convergence(func.entry, func.exit, func.blocks, check_convergence)) {
                throw std::runtime_error("Function execution paths do not converge");
            }
        }
    }

    // Ensure blocks are non-overlapping
    for (auto block_iter : ranges::view::iota(analyzed_program.blocks.begin(), analyzed_program.blocks.end())) {
        if (block_iter == std::prev(analyzed_program.blocks.end())) {
            break;
        }

        auto& block = *block_iter;
        auto& next_start = std::next(block_iter)->first;
        if (next_start < block.first.IncrementedBy(block.second.length)) {
            throw std::runtime_error(fmt::format("Blocks are overlapping: {:#x}-{:#x}, {:#x}", block_iter->first.value * 4, block.first.IncrementedBy(block.second.length).value * 4, next_start.value * 4));
        }
    }


    const auto& blocks = analyzed_program.blocks;

    for (auto& block : blocks) {
        if (micro_op_block_offsets.count(block.first)) {
            throw std::runtime_error(fmt::format("Already processed block at pica byte offset {:#x}", 4 * block.first.value));
        }

        // TODO: Can we get rid of this line? I think it's needed for empty blocks, which we generate for exit points on instruction offsets that are never executed (e.g. end of a function)
        micro_op_block_offsets[block.first] = MicroOpOffset { static_cast<uint32_t>(micro_ops.size()) };

// TODO: Check SuccessionVia instead to link blocks together
bool dont_link_to_next = false;

        // For empty blocks... TODO: We really should only need to check once per block rather than once per instruction!
        micro_op_block_offsets[block.first] = MicroOpOffset { static_cast<uint32_t>(micro_ops.size()) };

        if (block.second.length == 0xffffffff) {
            throw std::runtime_error(fmt::format("Unknown block length for block starting at {:#x}", 4 * block.first.value));
        }

        const auto block_end = block.first.IncrementedBy(block.second.length);
        for (auto instr_offset = block.first; !(instr_offset == block_end); instr_offset = instr_offset.GetNext()) {
            const Instruction instr = instr_offset.ReadFrom(context.shader_memory);
            auto is_control_flow = [](Instruction instr) {
                auto opcode = instr.opcode.Value().GetInfo().type;
                return (opcode != OpCode::Type::Arithmetic && opcode != OpCode::Type::MultiplyAdd);
            };

            if (!is_control_flow(instr)) {
                auto last_instr = instr_offset;
                for (auto instr : ranges::view::iota(instr_offset, block_end)) {
                    if (is_control_flow(instr.ReadFrom(context.shader_memory))) {
                        break;
                    }
                    last_instr = instr;
                }

                // TODO: If there's only one instruction to process, call the interpreter directly!

//                bool enable_burst = ((last_instr.GetNext() - instr_offset) > 2);
                bool enable_burst = true;
                ShaderEngineImpl::MicroOp* burst_op = nullptr;
                if (enable_burst) {
                    micro_ops.push_back({ WrapDefaultMicroOpHandler<BurstMicroOpHandler>, static_cast<uint32_t>(last_instr.GetNext() - instr_offset), instr_offset.value /* debugging information only */ });
                    burst_op = &micro_ops.back();
                }
                for (auto burst_offset : ranges::view::iota(instr_offset, last_instr.GetNext())) {
                    auto burst_instr = burst_offset.ReadFrom(context.shader_memory);


                    bool is_inverted = 0 != (burst_instr.opcode.Value().GetInfo().subtype & OpCode::Info::SrcInversed);
                    // TODO: Actually, is_inverted might not be supported properly currently...

                    uint32_t src1_reg_index = burst_instr.common.GetSrc1(is_inverted);
                    uint32_t src2_reg_index = burst_instr.common.GetSrc2(is_inverted);
                    uint32_t src1_addr_offset_index = (is_inverted ? 0 : burst_instr.common.address_register_index.Value());
                    uint32_t src2_addr_offset_index = (is_inverted ? burst_instr.common.address_register_index.Value() : 0);
                    uint32_t src_info = src1_reg_index | (src2_reg_index << 16u) | (src1_addr_offset_index << 8u) | (src2_addr_offset_index << 24u);
                    // Relocate output registers to 0x80 to avoid index collision with input registers
                    auto dst_reg = (burst_instr.common.dest.Value() < 0x10) ? (burst_instr.common.dest.Value() + 0x80) : static_cast<uint32_t>(burst_instr.common.dest.Value());

                    auto common_swizzle = SwizzlePattern { context.swizzle_data[burst_instr.common.operand_desc_id] };
                    auto num_dest_components = [](SwizzlePattern swizzle) {
                        uint32_t ret = 0;
                        for (auto i : ranges::view::iota(0, 4)) {
                            ret += swizzle.DestComponentEnabled(i);
                        }
                        return ret;
                    };
                    auto get_scalar_component = [](SwizzlePattern swizzle) -> std::optional<uint32_t> {
                        if (swizzle.dest_mask == 1) {
                            return 3;
                        } else if (swizzle.dest_mask == 2) {
                            return 2;
                        } else if (swizzle.dest_mask == 4) {
                            return 1;
                        } else if (swizzle.dest_mask == 8) {
                            return 0;
                        } else {
                            return std::nullopt;
                        }
                    };

                    if (burst_instr.opcode.Value().EffectiveOpCode() == OpCode::Id::ADD) {
                        micro_ops.push_back({ GetBinaryMicroOp<MicroOpADD>(enable_burst), src_info, dst_reg, common_swizzle.hex });
                    } else if (burst_instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MUL) {
                        micro_ops.push_back({ GetBinaryMicroOp<MicroOpMUL>(enable_burst), src_info, dst_reg, common_swizzle.hex });
                    } else if (burst_instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MAX) {
                        micro_ops.push_back({ GetBinaryMicroOp<MicroOpMAX>(enable_burst), src_info, dst_reg, common_swizzle.hex });
                    } else if (burst_instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MIN) {
                        micro_ops.push_back({ GetBinaryMicroOp<MicroOpMIN>(enable_burst), src_info, dst_reg, common_swizzle.hex });
                    } else if (burst_instr.opcode.Value().EffectiveOpCode() == OpCode::Id::DP3) {
                        if (num_dest_components(common_swizzle) == 1) {
                            common_swizzle.dest_mask = *get_scalar_component(common_swizzle);
                            micro_ops.push_back({ GetBinaryMicroOp<MicroOpDPScalar<3>>(enable_burst), src_info, dst_reg, common_swizzle.hex });
                        } else {
                            micro_ops.push_back({ GetBinaryMicroOp<MicroOpDP<3>>(enable_burst), src_info, dst_reg, common_swizzle.hex });
                        }
                    } else if (burst_instr.opcode.Value().EffectiveOpCode() == OpCode::Id::DP4) {
                        if (num_dest_components(common_swizzle) == 1) {
                            common_swizzle.dest_mask = *get_scalar_component(common_swizzle);
                            micro_ops.push_back({ GetBinaryMicroOp<MicroOpDPScalar<4>>(enable_burst), src_info, dst_reg, common_swizzle.hex });
                        } else {
                            micro_ops.push_back({ GetBinaryMicroOp<MicroOpDP<4>>(enable_burst), src_info, dst_reg, common_swizzle.hex });
                        }
                    } else if (burst_instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MOV) {
                        if (num_dest_components(common_swizzle) == 1) {
                            auto component = *get_scalar_component(common_swizzle);
                            common_swizzle.dest_mask = component;
                            common_swizzle.src1_selector_0 = common_swizzle.GetSelectorSrc1(component);
                            micro_ops.push_back({ GetCMPMicroOp<MicroOpMOV<true>>(enable_burst), src_info, dst_reg, common_swizzle.hex });
                        } else {
                            micro_ops.push_back({ GetCMPMicroOp<MicroOpMOV<false>>(enable_burst), src_info, dst_reg, common_swizzle.hex });
                        }
                    } else if (burst_instr.opcode.Value().EffectiveOpCode() == OpCode::Id::CMP) {
                        for (uint32_t i = 0; i < 2; ++i) {
                            auto compare_op = burst_instr.common.compare_op;
                            auto op = (i == 0) ? compare_op.x.Value() : compare_op.y.Value();

                            auto handler = std::invoke([=]() -> ShaderMicrocodeHandler {
                                switch (op) {
                                    case compare_op.Equal:
                                        return GetCMPMicroOp<MicroOpCMP<std::equal_to>>(enable_burst);

                                    case compare_op.NotEqual:
                                        return GetCMPMicroOp<MicroOpCMP<std::not_equal_to>>(enable_burst);

                                    case compare_op.LessThan:
                                        return GetCMPMicroOp<MicroOpCMP<std::less>>(enable_burst);

                                    case compare_op.LessEqual:
                                        return GetCMPMicroOp<MicroOpCMP<std::less_equal>>(enable_burst);

                                    case compare_op.GreaterThan:
                                        return GetCMPMicroOp<MicroOpCMP<std::greater>>(enable_burst);

                                    case compare_op.GreaterEqual:
                                        return GetCMPMicroOp<MicroOpCMP<std::greater_equal>>(enable_burst);

                                    default:
                                        throw std::runtime_error(fmt::format("Unknown compare mode {:#x}", static_cast<int>(op)));
                                }
                            });

                            micro_ops.push_back({ handler, src_info, i, context.swizzle_data[burst_instr.common.operand_desc_id] });
                        }

                        // CMP occupies two micro ops, so patch up the burst operation accordingly
                        if (burst_op) {
                            ++burst_op->arg0;
                        }
                    } else {
                        const bool is_mad = (burst_instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MAD);
                        uint32_t swizzle = context.swizzle_data[is_mad ? burst_instr.mad.operand_desc_id : burst_instr.common.operand_desc_id];

                        if (enable_burst) {
                            micro_ops.push_back({ LegacyInterpretShaderOpcode, burst_offset.ReadFrom(context.shader_memory).hex, swizzle, burst_offset.value /* debugging information only */ });
                        } else {
                            micro_ops.push_back({ WrapDefaultMicroOpHandler<LegacyInterpretShaderOpcode>, burst_offset.ReadFrom(context.shader_memory).hex, swizzle, burst_offset.value /* debugging information only */ });
                        }
                    }
                }

                // Skip forward
                instr_offset = last_instr;
            } else {
                // Handle each instruction on its own
                switch (instr.opcode.Value()) {
                case OpCode::Id::NOP:
                    break;

                case OpCode::Id::END:
                    micro_ops.push_back({ EndMicroOpHandler });
                    dont_link_to_next = true;
                    break;

                case OpCode::Id::CALLU:
                    if (instr.flow_control.bool_uniform_id == 15) {
//                        throw std::runtime_error("b15 not implemented");
                    }
                    [[fallthrough]];
                case OpCode::Id::CALL:
                case OpCode::Id::CALLC:

                    if (instr.opcode.Value() == OpCode::Id::CALLC) {
                        auto predicate_result = EmitMicroOpEvalCondition(micro_ops, instr.flow_control);
                        AddMicroOp<WrapDefaultMicroOpHandler<MicroOpPredicateNext>>(micro_ops, predicate_result, instr_offset.value /* for debugging only */);
                    }
                    if (instr.opcode.Value() != OpCode::Id::CALLU || shader_uniforms.b[instr.flow_control.bool_uniform_id]) {
                        auto target = ShaderCodeOffset { instr.flow_control.dest_offset };
                        AddMicroOp<MicroOpCall<false>>( micro_ops,
                                                        target,
                                                        instr.flow_control.num_instructions,
                                                        instr_offset.GetNext());
                    }

                    dont_link_to_next = (instr.opcode.Value() == OpCode::Id::CALL);
                    break;

                case OpCode::Id::IFC:
                {
                    auto predicate_result = EmitMicroOpEvalCondition(micro_ops, instr.flow_control);
                    AddMicroOp<WrapDefaultMicroOpHandler<MicroOpPredicateNext>>(micro_ops, predicate_result, instr_offset.value /* for debugging only */);

                    auto if_body = instr_offset.GetNext();
                    auto else_body = ShaderCodeOffset { instr.flow_control.dest_offset };

                    // TODO: Instead of manually emitting gotos here, encode the jump targets as PostExecJumps in the individual blocks

                    // Jump to if-body if predicate is true (which will skip past the jump to the else-body)
                    AddMicroOp<MicroOpGoto>(micro_ops, if_body, if_body.value /* debugging only */);

                    // Jump to else-body if predicate is false
                    // If the else-body is empty, this will jump to the end of the conditional
                    AddMicroOp<MicroOpGoto>(micro_ops, else_body, else_body.value /* debugging only */);
                    dont_link_to_next = true;
                    break;
                }

                case OpCode::Id::IFU:
                {
                    if (instr.flow_control.bool_uniform_id == 15) {
//                        throw std::runtime_error("b15 not implemented");
                    }

                    auto else_body = ShaderCodeOffset { instr.flow_control.dest_offset };
                    if (shader_uniforms.b[instr.flow_control.bool_uniform_id]) {
                        auto if_body = instr_offset.GetNext();
                        AddMicroOp<MicroOpGoto>(micro_ops, if_body, if_body.value /* debugging only */);
                    } else {
                        AddMicroOp<MicroOpGoto>(micro_ops, else_body, else_body.value /* debugging only */);
                    }
                    dont_link_to_next = true;
                    break;
                }

                case OpCode::Id::JMPC:
                {
                    auto predicate_result = EmitMicroOpEvalCondition(micro_ops, instr.flow_control);
                    AddMicroOp<MicroOpBranch>(micro_ops,
                                            predicate_result,
                                            ShaderCodeOffset { instr.flow_control.dest_offset },
                                            instr_offset.GetNext());
                    dont_link_to_next = true;
                    break;
                }

                case OpCode::Id::JMPU:
                    if (instr.flow_control.bool_uniform_id == 15) {
//                        throw std::runtime_error("b15 not implemented");
                    }

                    // NOTE: The lowest bit of the num_instructions field indicates an inverted condition for this instruction.
                    // TODO: Add a better-named field alias for this case
                    if (shader_uniforms.b[instr.flow_control.bool_uniform_id] == !(instr.flow_control.num_instructions & 1)) {
                        AddMicroOp<MicroOpGoto>(micro_ops, ShaderCodeOffset { instr.flow_control.dest_offset }, instr.flow_control.dest_offset /* debugging only */);
                    } else {
                        AddMicroOp<MicroOpGoto>(micro_ops, ShaderCodeOffset { instr_offset.GetNext() }, instr_offset.GetNext().value  /* debugging only */);
                    }
                    dont_link_to_next = true;
                    break;

                case OpCode::Id::BREAKC:
                    // TODO
                    break;

                case OpCode::Id::LOOP:
                {
                    AddMicroOp<WrapDefaultMicroOpHandler<MicroOpPrepareLoop>>(
                                            micro_ops,
                                            shader_uniforms.i[instr.flow_control.int_uniform_id].x,
                                            shader_uniforms.i[instr.flow_control.int_uniform_id].y,
                                            shader_uniforms.i[instr.flow_control.int_uniform_id].z);

                    auto last_loop_instr = ShaderCodeOffset { instr.flow_control.dest_offset };
                    AddMicroOp<MicroOpCall<true>>(  micro_ops,
                                                    instr_offset.GetNext(),
                                                    last_loop_instr - instr_offset,
                                                    last_loop_instr.GetNext());
                    dont_link_to_next = true;
                    break;
                }

                default:
                    throw std::runtime_error(fmt::format("TODO: Unrecognized instruction at offset {:#x}", 4 * instr_offset.value));
                }
            }
        }

        // TODO: Insert asserting micro ops past the last instruction


        if (block.second.post_exec_jump.FromCallStack()) {
            AddMicroOp<MicroOpCheckCallStack>(micro_ops, MicroOpOffset { static_cast<uint32_t>(micro_ops.size()) }, block.first.value /* Debugging only */);
            dont_link_to_next = true; // TODO: is this safe?
        } else if (auto target = block.second.post_exec_jump.GetStaticTarget()) {
            AddMicroOp<MicroOpGoto>(micro_ops, ShaderCodeOffset { *target }, target->value /* debugging only */);
            dont_link_to_next = true;
        }


        // Link with the next instruction in case we run off the end of a block
        // TODO: !dont_link_to_next should be equivalent to whether there is a fallthrough successor or not
        if (!dont_link_to_next && block.first != std::prev(blocks.end())->first /* TODO: Detect last block more cleanly */) {
            auto next_pica_instr = block.first.IncrementedBy(block.second.length);
            AddMicroOp<MicroOpGoto>(micro_ops, next_pica_instr, block.first.IncrementedBy(block.second.length).value /* debugging only */);
        }

        if (micro_ops.empty()) {
            throw std::runtime_error("No micro ops generated for block");
        }
    }
}

#if 0
static void ProcessShaderCodeOld(Context& context, ShaderEngineImpl& engine) {
    auto& shader_uniforms = context.shader_uniforms;

    while (true) {
        if (/*!state.call_stack.empty()*/ state.call_stack.size() > 1) {
            auto& top = state.call_stack.back();
            if (state.program_counter == top.final_address) {
                state.LoopRegister() += top.loop_increment;

                if (top.repeat_counter-- == 0) {
                    state.program_counter = top.return_address;
                    state.call_stack.pop_back();
                } else {
                    state.program_counter = top.loop_begin_address;
                }

                // TODO: Is "trying again" accurate to hardware?
                continue;
            }
        }

        bool exit_loop = false;
        const Instruction instr = state.program_counter.ReadFrom(context.shader_memory);

        // TODO: Evaluate performance impact of moving this up a scope
        auto call = [](VertexShaderState& state, ShaderCodeOffset offset, u32 num_instructions,
                        ShaderCodeOffset return_offset, u8 repeat_count, u8 loop_increment) {
            state.program_counter = offset.GetPrevious(); // Using previous offset to make sure when incrementing the PC we end up at the correct offset
            if (state.call_stack.size() >= state.call_stack.capacity()) {
                throw std::runtime_error("Shader call stack exhausted");
            }
            state.call_stack.push_back({ offset.IncrementedBy(num_instructions), return_offset, repeat_count, loop_increment, offset });
        };
        ShaderCodeOffset current_instr_offset = state.program_counter;

        state.debug.max_offset = std::max(state.debug.max_offset, current_instr_offset.GetNext());

        switch (instr.opcode.Value().GetInfo().type) {
        case OpCode::Type::Arithmetic:
        case OpCode::Type::MultiplyAdd:
            LegacyInterpretShaderOpcode(engine, instr.hex, 0, 0, context);
            break;

        default:
        {
            // TODO: Evaluate performance impact of moving this up a scope
            static auto evaluate_condition = [](const VertexShaderState& state, bool refx, bool refy, Instruction::FlowControlType flow_control) {
                // TODO: Evaluate performance impact of splitting this into two variables (might be easier on the optimizer)
                bool results[2] = { refx == state.conditional_code[0],
                                    refy == state.conditional_code[1] };

                switch (flow_control.op) {
                case flow_control.Or:
                    return results[0] || results[1];

                case flow_control.And:
                    return results[0] && results[1];

                case flow_control.JustX:
                    return results[0];

                case flow_control.JustY:
                    return results[1];
                }
            };

            // Handle each instruction on its own
            switch (instr.opcode.Value()) {
            case OpCode::Id::END:
                exit_loop = true;
                break;

            case OpCode::Id::JMPC:
                if (evaluate_condition(state, instr.flow_control.refx, instr.flow_control.refy, instr.flow_control)) {
                    state.program_counter = ShaderCodeOffset { instr.flow_control.dest_offset }.GetPrevious();
                }
                break;

            case OpCode::Id::JMPU:
                // NOTE: The lowest bit of the num_instructions field indicates an inverted condition for this instruction.
                // TODO: Add a better-named field alias for this case
                if (shader_uniforms.b[instr.flow_control.bool_uniform_id] == !(instr.flow_control.num_instructions & 1)) {
                    state.program_counter = ShaderCodeOffset { instr.flow_control.dest_offset }.GetPrevious();
                }
                break;

            case OpCode::Id::CALL:
                call(state,
                     ShaderCodeOffset { instr.flow_control.dest_offset },
                     instr.flow_control.num_instructions,
                     current_instr_offset.GetNext(), 0, 0);
                break;

            case OpCode::Id::CALLU:
                if (shader_uniforms.b[instr.flow_control.bool_uniform_id]) {
                    call(state,
                        ShaderCodeOffset { instr.flow_control.dest_offset },
                        instr.flow_control.num_instructions,
                        current_instr_offset.GetNext(), 0, 0);
                }
                break;

            case OpCode::Id::CALLC:
                if (evaluate_condition(state, instr.flow_control.refx, instr.flow_control.refy, instr.flow_control)) {
                    call(state,
                        ShaderCodeOffset { instr.flow_control.dest_offset },
                        instr.flow_control.num_instructions,
                        current_instr_offset.GetNext(), 0, 0);
                }
                break;

            case OpCode::Id::IFU:
                if (shader_uniforms.b[instr.flow_control.bool_uniform_id]) {
                    call(state,
                         current_instr_offset.GetNext(),
                         ShaderCodeOffset { instr.flow_control.dest_offset } - current_instr_offset.GetNext(),
                         ShaderCodeOffset { instr.flow_control.dest_offset }.IncrementedBy(instr.flow_control.num_instructions), 0, 0);
                } else {
                    call(state,
                         ShaderCodeOffset { instr.flow_control.dest_offset },
                         instr.flow_control.num_instructions,
                         ShaderCodeOffset { instr.flow_control.dest_offset }.IncrementedBy(instr.flow_control.num_instructions), 0, 0);
                }

                break;

            case OpCode::Id::IFC:
            {
                // TODO: Do we need to consider swizzlers here?

                if (evaluate_condition(state, instr.flow_control.refx, instr.flow_control.refy, instr.flow_control)) {
                    call(state,
                         current_instr_offset.GetNext(),
                         ShaderCodeOffset { instr.flow_control.dest_offset } - (current_instr_offset.GetNext()),
                         ShaderCodeOffset { instr.flow_control.dest_offset }.IncrementedBy(instr.flow_control.num_instructions), 0, 0);
                } else {
                    call(state,
                         ShaderCodeOffset { instr.flow_control.dest_offset },
                         instr.flow_control.num_instructions,
                         ShaderCodeOffset { instr.flow_control.dest_offset }.IncrementedBy(instr.flow_control.num_instructions), 0, 0);
                }

                break;
            }

            case OpCode::Id::LOOP:
            {
                state.LoopRegister() = shader_uniforms.i[instr.flow_control.int_uniform_id].y;

                call(state,
                     current_instr_offset.GetNext(),
                     ShaderCodeOffset { instr.flow_control.dest_offset } - current_instr_offset,
                     ShaderCodeOffset { instr.flow_control.dest_offset }.GetNext(),
                     shader_uniforms.i[instr.flow_control.int_uniform_id].x,
                     shader_uniforms.i[instr.flow_control.int_uniform_id].z);
                break;
            }

            default:
                LOG_ERROR(HW_GPU, "Unhandled instruction: 0x%02x (%s): 0x%08x",
                          (int)instr.opcode.Value().EffectiveOpCode(), instr.opcode.Value().GetInfo().name, instr.hex);
                break;
            }

            break;
        }
        }

        state.program_counter = state.program_counter.GetNext();

        if (exit_loop)
            break;
    }
}
#endif

static std::string DisassembleMicroOpBlock(
        const ShaderEngineImpl& engine,
        ShaderCodeOffset pica_offset,
        MicroOpOffset start_of_block,
        MicroOpOffset end_of_block,
        const char* line_separator = "\n") {
    namespace rv = ranges::view;

    uint32_t ops_to_indent = 0;
    auto format_micro_code = [&](const ShaderEngineImpl::MicroOp& micro_op) -> std::string {
        using EvalOp = Instruction::FlowControlType::Op;

        const char* indent = (ops_to_indent ? "  " : "");
        if (ops_to_indent) {
            --ops_to_indent;
        }

        auto format_binary_op = [&indent](const char* name, const ShaderEngineImpl::MicroOp& micro_op) {
            auto lookup_component = [](SwizzlePattern::Selector sel) {
                const char table[] = { 'x', 'y', 'z', 'w' };
                return table[Meta::to_underlying(sel)];
            };
            auto swizzle = SwizzlePattern { micro_op.arg2 };

            char dest_mask[] = "xyzw\0";
            for (auto i : ranges::view::iota(0, 4)) {
                if (!swizzle.DestComponentEnabled(i)) {
                    dest_mask[i] = '_';
                }
            }

            return fmt::format( "{}{} dest.{}, src1.{}{}{}{}, src2.{}{}{}{}", indent, name, dest_mask,
                                lookup_component(swizzle.GetSelectorSrc1(0)), lookup_component(swizzle.GetSelectorSrc1(1)),
                                lookup_component(swizzle.GetSelectorSrc1(2)), lookup_component(swizzle.GetSelectorSrc1(3)),
                                lookup_component(swizzle.GetSelectorSrc2(0)), lookup_component(swizzle.GetSelectorSrc2(1)),
                                lookup_component(swizzle.GetSelectorSrc2(2)), lookup_component(swizzle.GetSelectorSrc2(3)));
        };

        auto restore_scalar_swizzle = [](const ShaderEngineImpl::MicroOp& micro_op) {
            auto ret = micro_op;
            auto swizzle = SwizzlePattern { ret.arg2 };
            swizzle.dest_mask = 8 >> swizzle.dest_mask;
            ret.arg2 = swizzle.hex;
            return ret;
        };

        if (micro_op.handler == &LegacyInterpretShaderOpcode || micro_op.handler == &WrapDefaultMicroOpHandler<LegacyInterpretShaderOpcode>) {
            return fmt::format("{}Interpret      instr={:#010x}", indent, micro_op.arg0);
        } else if (micro_op.handler == &WrapDefaultMicroOpHandler<BurstMicroOpHandler>) {
            ops_to_indent += micro_op.arg0;
            return fmt::format("{}BurstProcess     count={:#x}", indent, micro_op.arg0);
        } else if (micro_op.handler == &MicroOpTypeErased<MicroOpCall<false>>) {
            return fmt::format("{}Call             target={:#05x}    num_instrs={:#x} return={:#x}", indent, micro_op.arg0, micro_op.arg1, micro_op.arg2);
        } else if (micro_op.handler == &MicroOpTypeErased<MicroOpCall<true>>) {
            // TODO: Read parameters set up by the preceding PrepareLoop micro op
            return fmt::format("{}EnterLoop        target={:#05x}    num_instrs={:#x} return={:#x}", indent, micro_op.arg0, micro_op.arg1, micro_op.arg2);
        } else if (micro_op.handler == &MicroOpTypeErased<MicroOpCheckCallStack>) {
            return fmt::format("{}CheckCallStack   current_offset={:#05x}", indent, micro_op.arg0);
        } else if (micro_op.handler == &MicroOpTypeErased<WrapDefaultMicroOpHandler<MicroOpPredicateNext>>) {
            ++ops_to_indent;
            return fmt::format("{}PredicateNext    reg={}", indent, micro_op.arg0);
        } else if (micro_op.handler == &MicroOpTypeErased<MicroOpGoto>) {
            return fmt::format("{}Goto             target={:#05x}", indent, micro_op.arg0);
        } else if (micro_op.handler == &WrapDefaultMicroOpHandler<MicroOpEvalCondition<EvalOp::Or>>) {
            return fmt::format("{}EvalCondition    refx={} OR  refy={}", indent, micro_op.arg0, micro_op.arg1);
        } else if (micro_op.handler == &WrapDefaultMicroOpHandler<MicroOpEvalCondition<EvalOp::And>>) {
            return fmt::format("{}EvalCondition    refx={} AND refy={}", indent, micro_op.arg0, micro_op.arg1);
        } else if (micro_op.handler == &WrapDefaultMicroOpHandler<MicroOpEvalCondition<EvalOp::JustX>>) {
            return fmt::format("{}EvalCondition    refx={}", indent, micro_op.arg0);
        } else if (micro_op.handler == &WrapDefaultMicroOpHandler<MicroOpEvalCondition<EvalOp::JustY>>) {
            return fmt::format("{}EvalCondition    refy={}", indent, micro_op.arg1);
        } else if (micro_op.handler == &EndMicroOpHandler) {
            return format_binary_op("END", micro_op);
        } else if (micro_op.handler == GetBinaryMicroOp<MicroOpADD>(false) || micro_op.handler == GetBinaryMicroOp<MicroOpADD>(true)) {
            return format_binary_op("ADD", micro_op);
        } else if (micro_op.handler == GetBinaryMicroOp<MicroOpMUL>(false) || micro_op.handler == GetBinaryMicroOp<MicroOpMUL>(true)) {
            return format_binary_op("MUL", micro_op);
        } else if (micro_op.handler == GetBinaryMicroOp<MicroOpMAX>(false) || micro_op.handler == GetBinaryMicroOp<MicroOpMAX>(true)) {
            return format_binary_op("MAX", micro_op);
        } else if (micro_op.handler == GetBinaryMicroOp<MicroOpMIN>(false) || micro_op.handler == GetBinaryMicroOp<MicroOpMIN>(true)) {
            return format_binary_op("MIN", micro_op);
        } else if (micro_op.handler == GetBinaryMicroOp<MicroOpDP<3>>(false) || micro_op.handler == GetBinaryMicroOp<MicroOpDP<3>>(true)) {
            return format_binary_op("DP3", micro_op);
        } else if (micro_op.handler == GetBinaryMicroOp<MicroOpDP<4>>(false) || micro_op.handler == GetBinaryMicroOp<MicroOpDP<4>>(true)) {
            return format_binary_op("DP4", micro_op);
        } else if (micro_op.handler == GetBinaryMicroOp<MicroOpDPScalar<3>>(false) || micro_op.handler == GetBinaryMicroOp<MicroOpDPScalar<3>>(true)) {
            return format_binary_op("DP3scalar", restore_scalar_swizzle(micro_op));
        } else if (micro_op.handler == GetBinaryMicroOp<MicroOpDPScalar<4>>(false) || micro_op.handler == GetBinaryMicroOp<MicroOpDPScalar<4>>(true)) {
            return format_binary_op("DP4scalar", restore_scalar_swizzle(micro_op));
        } else if ( micro_op.handler == GetCMPMicroOp<MicroOpMOV<false>>(false) || micro_op.handler == GetCMPMicroOp<MicroOpMOV<false>>(true)) {
            return format_binary_op("MOV", micro_op);
        } else if ( micro_op.handler == GetCMPMicroOp<MicroOpMOV<true>>(false) || micro_op.handler == GetCMPMicroOp<MicroOpMOV<true>>(true)) {
            return format_binary_op("MOVscalar", restore_scalar_swizzle(micro_op));
            // TODO: Disasm CMP
//        } else if (micro_op.handler == GetBinaryMicroOp<MicroOpCMP>(false) || micro_op.handler == GetBinaryMicroOp<MicroOpCMP>(true)) {
//            return fmt::format("{}CMP", indent);
        } else {
            return fmt::format("{}UNKNOWN          {:#x} {:#x} {:#x}", indent, micro_op.arg0, micro_op.arg1, micro_op.arg2);
        }
    };

    auto first_micro_op_offset = start_of_block.value;
    auto ret = fmt::format( "{:#05x}|{:#05x}:   {}{}",
                            pica_offset.value * 4,
                            first_micro_op_offset,
                            format_micro_code(engine.micro_ops[first_micro_op_offset]),
                            line_separator);
    for (auto micro_op_offset : rv::iota(start_of_block.GetNext(), end_of_block)) {
        auto& micro_op = engine.micro_ops[micro_op_offset.value];
        if (!micro_op.handler) {
            break;
        }

        ret += fmt::format( "      {:#05x}:   {}{}",
                            micro_op_offset.value,
                            format_micro_code(micro_op),
                            line_separator);
    }

    return ret;
}

static void DisassembleMicroOps(const ShaderEngineImpl& engine) {
    for (auto block_iter : ranges::view::iota(engine.micro_op_block_offsets.begin(), engine.micro_op_block_offsets.end())) {
        auto pica_offset = block_iter->first;

        namespace rv = ranges::view;
        const auto start_of_block = block_iter->second;
        auto end_of_block
                = (std::next(block_iter) == engine.micro_op_block_offsets.end())
                ? MicroOpOffset { static_cast<uint32_t>(engine.micro_ops.size()) }
                : std::next(block_iter)->second;

        std::cerr << DisassembleMicroOpBlock(engine, pica_offset, start_of_block, end_of_block);
    }
}

static void OutputControlFlowGraph(const ShaderEngineImpl& engine, const AnalyzedProgram& program) {
    fmt::print(stderr, "digraph {{\n");

    std::set<ShaderCodeOffset> function_blocks;

    auto print_block = [&engine](ShaderCodeOffset pica_start) {
        auto micro_op_start_it = engine.micro_op_block_offsets.find(pica_start);
        auto micro_op_start = micro_op_start_it->second;
        auto micro_op_start_next =
                (std::next(micro_op_start_it) == engine.micro_op_block_offsets.end())
                ? MicroOpOffset { static_cast<uint32_t>(engine.micro_ops.size()) }
                : std::next(micro_op_start_it)->second;
        fmt::print( stderr,
                    "  block{:#x}[fontname=\"monospace\" shape=box label=\"{}\"]\n",
                    4 * pica_start.value,
                    DisassembleMicroOpBlock(engine, pica_start, micro_op_start, micro_op_start_next, "\\l"));
    };

    for (auto& function : program.functions) {
        fmt::print(stderr, "\n subgraph cluster_{:#x} {{\n", 4 * function.entry.value);
        for (auto& block_start : function.blocks) {
            print_block(block_start);
            function_blocks.insert(block_start);
        }
        fmt::print(stderr, " }}\n\n");
    }

    for (auto& block : program.blocks) {
        if (!function_blocks.count(block.first)) {
            print_block(block.first);
        }
    }

    fmt::print(stderr, "\n");

    for (auto& block : program.blocks) {
        for (auto& successor : block.second.successors) {
            fmt::print( stderr,
                        "  block{:#x} -> block{:#x}",
                        4 * block.first.value, 4 * successor.first.value);

            using SuccessionVia = AnalyzedProgram::SuccessionVia;
            switch (successor.second) {
                case SuccessionVia::Return:
                    fmt::print(stderr, " [style=dotted]");
                    break;

                case SuccessionVia::BranchToIf:
                    fmt::print(stderr, " [color=green]");
                    break;

                case SuccessionVia::BranchToElse:
                    fmt::print(stderr, " [color=red]");
                    break;

                default:
                    ;
            }
            fmt::print(stderr, "\n");
        }
    }

    fmt::print(stderr, "}}");
}

static std::string GenerateGLSL(Context& context, ShaderEngineImpl& engine, const AnalyzedProgram& program);

ShaderEngineImpl::ShaderEngineImpl(
        Context& context, uint32_t entry_point_)
        : entry_point { entry_point_ }, num_input_attributes(0) {
    ZoneNamedN(ShaderCompile, "Shader Compiler Core", true)

    micro_ops.reserve(0x1000);
    micro_ops.clear(); // avoid allocations. TODO: Remove
    micro_op_block_offsets.clear(); // avoid allocations. TODO: Remove

//    std::cerr << "Shader micro op pre-disassembly:\n";
    ProcessShaderCode(context, micro_op_block_offsets, micro_ops, ShaderCodeOffset { entry_point_ });
//    DisassembleMicroOps(*this);
//    OutputControlFlowGraph(*this, analyzed_program);

    // Patch offsets from ShaderCodeOffset to MicroOpOffset
    for (auto micro_op_offset : ranges::views::iota(MicroOpOffset { 0 }, MicroOpOffset { static_cast<uint32_t>(micro_ops.size()) })) {
        auto& micro_op = micro_ops[micro_op_offset.value];

        if (micro_op.handler == &MicroOpTypeErased<MicroOpCall<false>> ||
            micro_op.handler == &MicroOpTypeErased<MicroOpCall<true>>) {
            auto new_target = micro_op_block_offsets.at(ShaderCodeOffset { micro_op.arg0 });
            auto pica_target = ShaderCodeOffset { micro_op.arg0 }.IncrementedBy(micro_op.arg1 - 1);

            auto next_block_it = micro_op_block_offsets.upper_bound(pica_target);
            auto block_it = std::prev(next_block_it);
            auto next_block_pos = (next_block_it == micro_op_block_offsets.end()) ? MicroOpOffset { static_cast<uint32_t>(micro_ops.size()) } : next_block_it->second;
            if (next_block_pos < block_it->second ||
                    !(pica_target < block_it->first.IncrementedBy(micro_op.arg1))/*
                    || !(pica_target < next_block_it->first)*/) {
                throw std::runtime_error("Could not find block");
            }

            auto new_exit_site = next_block_pos.GetPrevious();
            Mikage::ValidateContract(micro_ops[new_exit_site.value].handler == &MicroOpTypeErased<MicroOpCheckCallStack>);
            micro_op.arg1 = new_exit_site - new_target;
            micro_op.arg0 = new_target.value;
            micro_op.arg2 = micro_op_block_offsets.at(ShaderCodeOffset { micro_op.arg2 }).value;
            if (micro_op.arg0 == micro_op_offset.value) {
                throw std::runtime_error("Detected infinite loop");
            }
        } else if (micro_op.handler == &MicroOpTypeErased<MicroOpCheckCallStack>) {
            // micro op offset is known (and used) at generation time, so no need for patching
        } else if (micro_op.handler == &MicroOpTypeErased<MicroOpBranch>) {
            micro_op.arg1 = micro_op_block_offsets.at(ShaderCodeOffset { micro_op.arg1 }).value;
            micro_op.arg2 = micro_op_block_offsets.at(ShaderCodeOffset { micro_op.arg2 }).value;
        } else if (micro_op.handler == &MicroOpTypeErased<MicroOpGoto>) {
            micro_op.arg0 = micro_op_block_offsets.at(ShaderCodeOffset { micro_op.arg0 }).value;
        }
    }

    // To avoid emptiness checks, add a dummy element to the call stack
    call_stack.clear();
    call_stack.push_back({ MicroOpOffset { 0xffffffff }, {}, {}, {}, {} });
    last_call_stack_address = call_stack.back().final_address;


//    DebugUtils::DumpShader(context.shader_memory.data(), 1024, context.swizzle_data.data(),
//                           1024, context.registers.vs_main_offset,
//                           context.registers.vs_output_attributes);
//    std::cerr << "Shader micro op disassembly:\n";
//    DisassembleMicroOps(*this);
//    OutputControlFlowGraph(*this, analyzed_program);
    glsl = GenerateGLSL(context, *this, analyzed_program);
//    std::cerr << global_vertex_shader_code_todo;
}

void UpdateUniform(ShaderEngineImpl& engine, Math::Vec4<float24> uniform, std::size_t index) {
    engine.float_uniforms[index] = uniform;
}

void Reset(Context&, ShaderEngineImpl& engine, const Regs::VSInputRegisterMap& register_map, uint32_t num_attributes, InputVertex& input) {

    // TODO: If relative addressing is not available for input registers, can't we statically resolve references to remapped input registers?
    engine.num_input_attributes = num_attributes;
    ranges::fill(engine.input_register_table, &engine.dummy_register);
    if(engine.num_input_attributes > 0) engine.input_register_table[register_map.attribute0_register] = &input.attr[0];
    if(engine.num_input_attributes > 1) engine.input_register_table[register_map.attribute1_register] = &input.attr[1];
    if(engine.num_input_attributes > 2) engine.input_register_table[register_map.attribute2_register] = &input.attr[2];
    if(engine.num_input_attributes > 3) engine.input_register_table[register_map.attribute3_register] = &input.attr[3];
    if(engine.num_input_attributes > 4) engine.input_register_table[register_map.attribute4_register] = &input.attr[4];
    if(engine.num_input_attributes > 5) engine.input_register_table[register_map.attribute5_register] = &input.attr[5];
    if(engine.num_input_attributes > 6) engine.input_register_table[register_map.attribute6_register] = &input.attr[6];
    if(engine.num_input_attributes > 7) engine.input_register_table[register_map.attribute7_register] = &input.attr[7];
    if(engine.num_input_attributes > 8) engine.input_register_table[register_map.attribute8_register] = &input.attr[8];
    if(engine.num_input_attributes > 9) engine.input_register_table[register_map.attribute9_register] = &input.attr[9];
    if(engine.num_input_attributes > 10) engine.input_register_table[register_map.attribute10_register] = &input.attr[10];
    if(engine.num_input_attributes > 11) engine.input_register_table[register_map.attribute11_register] = &input.attr[11];
    if(engine.num_input_attributes > 12) engine.input_register_table[register_map.attribute12_register] = &input.attr[12];
    if(engine.num_input_attributes > 13) engine.input_register_table[register_map.attribute13_register] = &input.attr[13];
    if(engine.num_input_attributes > 14) engine.input_register_table[register_map.attribute14_register] = &input.attr[14];
    if(engine.num_input_attributes > 15) engine.input_register_table[register_map.attribute15_register] = &input.attr[15];

    global_vertex_shader_code_todo = engine.glsl;

}

void RunShader(Context& context, ShaderEngineImpl& engine, OutputVertex& ret) {
    TracyCZoneN(SetupInvocation, "Setup Invocation", true);

// TODO: Add member function Reset...
//    engine.Reset();

    engine.debug.max_offset = { 0 };
    engine.debug.max_opdesc_id = 0;

    engine.conditional_code[0] = false;
    engine.conditional_code[1] = false;

    engine.address_registers[0] = 0;
    engine.address_registers[1] = 0;
    engine.address_registers[2] = 0;
    engine.address_registers[3] = 0;

    memset(engine.temporary_registers, 0, sizeof(engine.temporary_registers));

    // Drop all but the topmost (i.e. dummy) call stack element
    engine.call_stack.resize(1);

    namespace rv = ranges::view;
    ranges::copy(   engine.input_register_table | rv::take(engine.num_input_attributes) | rv::indirect,
                    engine.input_registers.begin());

    // TODO: Re-enable shader dumping
//    DebugUtils::DumpShader(context.shader_memory.data(), 1024, context.swizzle_data.data(),
//                           1024, context.registers.vs_main_offset,
//                           context.registers.vs_output_attributes);

    engine.next_micro_op = &engine.micro_ops.at(engine.micro_op_block_offsets.at(engine.entry_point).value);
    auto first_micro_op = *engine.next_micro_op++;
    TracyCZoneEnd(SetupInvocation);
    TracyCZoneN(ShaderRun, "Run", true);
    first_micro_op.handler(engine, first_micro_op.arg0, first_micro_op.arg1, first_micro_op.arg2);
    TracyCZoneEnd(ShaderRun);


    // Setup output data
    // TODO: Move this into the generated micro code!
    TracyCZoneN(ShaderOutput, "Output", true);

    // Map output registers to (tightly packed) output attributes and
    // assign semantics
    // TODO(neobrain): Under some circumstances, up to 16 registers may be used. We need to
    // figure out what those circumstances are and enable the remaining registers then.
    for (unsigned output_reg = 0, output_attrib = 0; true; ++output_reg ) {
        if (output_reg >= 7) {
            throw Mikage::Exceptions::Invalid(  "Requested {} output attributes, but only {} were output by the shader",
                                                context.registers.vs_num_output_attributes.Value(), output_attrib);
        }

        if (!(context.registers.vs_output_register_mask.mask() & (1 << output_reg))) {
            continue;
        }

        const auto& output_register_map = context.registers.vs_output_attributes[output_attrib++];

        u32 semantics[4] = {
            output_register_map.map_x, output_register_map.map_y,
            output_register_map.map_z, output_register_map.map_w
        };

        for (int comp = 0; comp < 4; ++comp) {
            float24* out = ((float24*)&ret) + semantics[comp];
            if (semantics[comp] != Regs::VSOutputAttributes::INVALID) {
                *out = engine.output_registers[output_reg][comp];
            } else {
                // Zero output so that attributes which aren't output won't have denormals in them,
                // which would slow us down later.
                memset(out, 0, sizeof(*out));
            }
        }

        if (output_attrib == context.registers.vs_num_output_attributes) {
            break;
        }
    }

    LOG_TRACE(Render_Software, "Output vertex: pos (%.2f, %.2f, %.2f, %.2f), col(%.2f, %.2f, %.2f, %.2f), tc0(%.2f, %.2f)",
        ret.pos.x.ToFloat32(), ret.pos.y.ToFloat32(), ret.pos.z.ToFloat32(), ret.pos.w.ToFloat32(),
        ret.color.x.ToFloat32(), ret.color.y.ToFloat32(), ret.color.z.ToFloat32(), ret.color.w.ToFloat32(),
        ret.tc0.u().ToFloat32(), ret.tc0.v().ToFloat32());

    TracyCZoneEnd(ShaderOutput);
}

static std::string FunctionName(ShaderCodeOffset entry) {
    return fmt::format("func_{:#x}", 4 * entry.value);
}

// Maps PICA shader output register index to symbol name to use for that register.
// TODO: Un-global-ify
static std::map<int, std::string> reg_index_map;

// Maps PICA shader input register index to symbol name used for that register
// TODO: Un-global-ify
static std::map<int, std::string> input_reg_map;

static std::string TranslateToGLSL(Context& context, Instruction instr) {
    // TODO: Turn these into proper functions instead
    static auto select_input_register = [](SourceRegister reg, std::optional<unsigned> address_register_index) {
        if (address_register_index && reg.GetRegisterType() != RegisterType::FloatUniform) {
            throw std::runtime_error("Used relative register addressing with non-uniform register type");
        }

        switch (reg.GetRegisterType()) {
        case RegisterType::Input:
            return input_reg_map.at(reg.GetIndex());

        case RegisterType::Temporary:
            return fmt::format("temp_{}", reg.GetIndex());

        case RegisterType::FloatUniform:
            return fmt::format("uniforms.f[{}{}]", reg.GetIndex(), address_register_index ? fmt::format(" + a{}", *address_register_index) : "");

        default:
            __builtin_unreachable();
        }
    };

    auto select_output_register = [](nihstro::DestRegister reg) {
        switch (reg.GetRegisterType()) {
        case RegisterType::Output:
            return reg_index_map.at(reg.GetIndex());

        case RegisterType::Temporary:
            return fmt::format("temp_{}", reg.GetIndex());

        default:
            __builtin_unreachable();
        }
    };

    auto input_operand = [](SourceRegister reg, std::optional<unsigned> address_reg_index, bool negate, const char* swizzle) {
        std::string ret;
        if (negate) {
            ret += "(-";
        }
        ret += select_input_register(reg, address_reg_index);
        if (std::strcmp("xyzw", swizzle) != 0) {
            ret += '.';
            ret += swizzle;
        }
        if (negate) {
            ret += ')';
        }
        return ret;
    };

    switch (instr.opcode.Value().GetInfo().type) {
    case OpCode::Type::Arithmetic:
    {
        auto swizzle = SwizzlePattern { context.swizzle_data[instr.common.operand_desc_id] };

        bool is_inverted = 0 != (instr.opcode.Value().GetInfo().subtype & OpCode::Info::SrcInversed);
        if (is_inverted) {
            // TODO: We don't really support this properly: For instance, the address register
            //       offset needs to be applied to SRC2 instead, etc.
            //       For now, we just abort in this situation.
            LOG_CRITICAL(HW_GPU, "Bad condition...");
            exit(0);
        }

        // TODO: Add contract ensuring these indices are only used for uniform inputs
        std::optional<unsigned> src1_address_reg_index, src2_address_reg_index;
        if (instr.common.address_register_index != 0) {
            (is_inverted ? src2_address_reg_index : src1_address_reg_index)
                = instr.common.address_register_index.Value() - 1;
        }

        const bool negate_src1 = ((bool)swizzle.negate_src1 != false);
        const bool negate_src2 = ((bool)swizzle.negate_src2 != false);

        const char xyzw[] = "xyzw";

        // Swizzle masks constrained to active destination components
        char src1_swizzle[5] { };
        char src2_swizzle[5] { };
        char dest_swizzle[5] { };
        char* src1_swizzle_ptr = src1_swizzle;
        char* src2_swizzle_ptr = src2_swizzle;
        char* dest_swizzle_ptr = dest_swizzle;
        for (auto i : ranges::views::iota(0, 4)) {
            if (swizzle.DestComponentEnabled(i)) {
                *dest_swizzle_ptr++ = xyzw[i];
                *src1_swizzle_ptr++ = xyzw[Meta::to_underlying(swizzle.GetSelectorSrc1(i))];
                *src2_swizzle_ptr++ = xyzw[Meta::to_underlying(swizzle.GetSelectorSrc2(i))];
            }
        }

        // Full four-component swizzle masks
        char src1_swizzle_4[5] { };
        char src2_swizzle_4[5] { };
        src1_swizzle_ptr = src1_swizzle_4;
        src2_swizzle_ptr = src2_swizzle_4;
        for (auto i : ranges::views::iota(0, 4)) {
            *src1_swizzle_ptr++ = xyzw[Meta::to_underlying(swizzle.GetSelectorSrc1(i))];
            *src2_swizzle_ptr++ = xyzw[Meta::to_underlying(swizzle.GetSelectorSrc2(i))];
        }

        auto get_dest = [&]() {
            std::string ret;
            ret += select_output_register(instr.common.dest);
            if (swizzle.dest_mask != 0xf) {
                ret += '.';
                ret += dest_swizzle;
            }
            return ret;
        };

        auto get_src1 = [&](const char* swizzle) {
            return input_operand(instr.common.GetSrc1(is_inverted), src1_address_reg_index, negate_src1, swizzle);
        };

        auto get_src2 = [&](const char* swizzle) {
            return input_operand(instr.common.GetSrc2(is_inverted), src2_address_reg_index, negate_src2, swizzle);
        };

        auto num_dest_components = strlen(dest_swizzle);

        switch (instr.opcode.Value().EffectiveOpCode()) {
        case OpCode::Id::ADD:
            return get_dest() + " = " + get_src1(src1_swizzle) + " + " + get_src2(src2_swizzle) + ';';

        case OpCode::Id::DP3:
            return get_dest() + " = dot(" + get_src1(src1_swizzle_4) + ".xyz, " + get_src2(src2_swizzle_4) + fmt::format(".xyz).{};", std::string(num_dest_components, 'x'));

        case OpCode::Id::DP4:
            return get_dest() + " = dot(" + get_src1(src1_swizzle_4) + ", " + get_src2(src2_swizzle_4) + fmt::format(").{};", std::string(num_dest_components, 'x'));

        case OpCode::Id::MUL:
            return get_dest() + " = " + get_src1(src1_swizzle) + " * " + get_src2(src2_swizzle) + ';';

        case OpCode::Id::MAX:
            return get_dest() + " = max(" + get_src1(src1_swizzle) + ", " + get_src2(src2_swizzle) + ");";

        case OpCode::Id::MIN:
            return get_dest() + " = min(" + get_src1(src1_swizzle) + ", " + get_src2(src2_swizzle) + ");";

        case OpCode::Id::RCP:
        {
            auto one = (num_dest_components == 1) ? " = 1.0 / " : fmt::format(" = vec{}(1.0) / ", num_dest_components);
            return get_dest() + one + get_src1(src1_swizzle) + ".x;";
        }

        case OpCode::Id::RSQ:
            return get_dest() + " = " + "inversesqrt(" + get_src1(std::string(num_dest_components, src1_swizzle[0]).c_str()) + ");";

        case OpCode::Id::MOVA:
        {
            // TODO: Figure out how the rounding is done on hardware
            std::string ret;
            for (auto i : { 0, 1 }) {
                if (swizzle.DestComponentEnabled(i)) {
                    ret += fmt::format("a{} = int({}.{});\n", i, get_src1(src1_swizzle_4), ((i == 0) ? "x" : "y"));
                }
            }
            return ret;
        }

        case OpCode::Id::MOV:
            return get_dest() + " = " + get_src1(src1_swizzle) + ';';

        case OpCode::Id::CMP:
        {
            std::string ret;
            for (auto i : { 0, 1 }) {
                auto compare_op = instr.common.compare_op;
                auto op = (i == 0) ? compare_op.x.Value() : compare_op.y.Value();

                const char* op_str = std::invoke([op, compare_op]() {
                    switch (op) {
                    case compare_op.Equal:
                        return "==";

                    case compare_op.NotEqual:
                        return "!=";

                    case compare_op.LessThan:
                        return "<";

                    case compare_op.LessEqual:
                        return "<=";

                    case compare_op.GreaterThan:
                        return ">";

                    case compare_op.GreaterEqual:
                        return ">=";

                    default:
                        throw Mikage::Exceptions::NotImplemented("Unknown compare mode {:#x}", Meta::to_underlying(op));
                    }
                });
                auto component = (i == 0) ? "x" : "y";
                ret += fmt::format( "cond.{} = {}.{} {} {}.{};\n",
                                    component, get_src1(src1_swizzle_4), component,
                                    op_str, get_src2(src2_swizzle_4), component);
            }
            return ret;
        }

        default:
            return "// UNKNOWN ARITHMETIC";
        }
    }

    case OpCode::Type::MultiplyAdd:
    {
        auto swizzle = SwizzlePattern { context.swizzle_data[instr.mad.operand_desc_id] };

        if (instr.opcode.Value().EffectiveOpCode() != OpCode::Id::MAD) {
            throw std::runtime_error("Unsupported MAD-like shader opcode");
        }
        const bool is_inverted = false;
        if (is_inverted) {
            // TODO: We don't really support this properly: For instance, the address register
            //       offset needs to be applied to SRC2 instead, etc.
            //       For now, we just abort in this situation.
            LOG_CRITICAL(HW_GPU, "Bad condition...");
            exit(0);
        }

        // TODO: Add contract ensuring these indices are only used for uniform inputs
        std::optional<unsigned> src2_address_reg_index, src3_address_reg_index;
        if (instr.mad.address_register_index != 0) {
            (is_inverted ? src3_address_reg_index : src2_address_reg_index)
                = instr.mad.address_register_index.Value() - 1;
        }

        const bool negate_src1 = ((bool)swizzle.negate_src1 != false);
        const bool negate_src2 = ((bool)swizzle.negate_src2 != false);
        const bool negate_src3 = ((bool)swizzle.negate_src3 != false);

        const char xyzw[] = "xyzw";
        char src1_swizzle[5] { };
        char src2_swizzle[5] { };
        char src3_swizzle[5] { };
        char dest_swizzle[5] { };
        char* src1_swizzle_ptr = src1_swizzle;
        char* src2_swizzle_ptr = src2_swizzle;
        char* src3_swizzle_ptr = src3_swizzle;
        char* dest_swizzle_ptr = dest_swizzle;
        for (auto i : ranges::views::iota(0, 4)) {
            if (swizzle.DestComponentEnabled(i)) {
                *dest_swizzle_ptr++ = xyzw[i];
                *src1_swizzle_ptr++ = xyzw[Meta::to_underlying(swizzle.GetSelectorSrc1(i))];
                *src2_swizzle_ptr++ = xyzw[Meta::to_underlying(swizzle.GetSelectorSrc2(i))];
                *src3_swizzle_ptr++ = xyzw[Meta::to_underlying(swizzle.GetSelectorSrc3(i))];
            }
        }

        auto get_dest = [&]() {
            std::string ret;
            ret += select_output_register(instr.mad.dest);
            if (swizzle.dest_mask != 0xf) {
                ret += '.';
                ret += dest_swizzle;
            }
            return ret;
        };

        auto get_src1 = [&](const char* swizzle) {
            return input_operand(instr.mad.GetSrc1(is_inverted), std::nullopt, negate_src1, swizzle);
        };

        auto get_src2 = [&](const char* swizzle) {
            return input_operand(instr.mad.GetSrc2(is_inverted), src2_address_reg_index, negate_src2, swizzle);
        };

        auto get_src3 = [&](const char* swizzle) {
            return input_operand(instr.mad.GetSrc3(is_inverted), src3_address_reg_index, negate_src3, swizzle);
        };

        return get_dest() + " = " + get_src1(src1_swizzle) + " * " + get_src2(src2_swizzle) + " + " + get_src3(src3_swizzle) + ';';
    }

    default:
        switch (instr.opcode.Value().EffectiveOpCode()) {
        case OpCode::Id::NOP:
            return "";

        case OpCode::Id::END:
        {
            std::string glsl = "// END\n";
            for (unsigned output_reg = 0, output_attrib = 0; true; ++output_reg ) {
                if (!(context.registers.vs_output_register_mask.mask() & (1 << output_reg))) {
                    continue;
                }

                const auto& output_register_map = context.registers.vs_output_attributes[output_attrib++];

                // TODO: For now, we just always declare outputs such that they include the mapping directly. Instead, we should reassign output registers (or at least all writes to them) to consider this
                // Either way, we want a static output mapping so that we don't need to make the fragment shader inputs depend on vertex shader configuration
                u32 semantics[4] = {
                    output_register_map.map_x, output_register_map.map_y,
                    output_register_map.map_z, output_register_map.map_w
                };

                if (semantics[0] == Regs::VSOutputAttributes::POSITION_X) {
                    // gl_Position is implicitly defined
                    glsl += fmt::format("gl_Position = vec4({}.xy, -{}.z, {}.w);\n",
                                        reg_index_map.at(output_reg), reg_index_map.at(output_reg), reg_index_map.at(output_reg));
                    break;
                }
            }
            // TODO: Also must cause callers to stop executing any code after this one!
            return glsl;
        }

        case OpCode::Id::BREAKC:
            return "// TODO: Breakc";

        case OpCode::Id::CALL:
        case OpCode::Id::CALLC:
        case OpCode::Id::CALLU:
            if (instr.opcode.Value() != OpCode::Id::CALLU || context.shader_uniforms.b[instr.flow_control.bool_uniform_id]) {
                return FunctionName(ShaderCodeOffset { instr.flow_control.dest_offset }) + "();";
            } else {
                return "// Conditional call (statically omitted)";
            }

        case OpCode::Id::IFU:
        case OpCode::Id::IFC:
            // Already encoded in CFG; nothing to do
            return "// Conditional";

        case OpCode::Id::JMPC:
        case OpCode::Id::JMPU:
            // Already encoded in CFG; nothing to do
            return "// Jump";

        case OpCode::Id::LOOP:
            // Already encoded in CFG; nothing to do
            return "// Loop";

        default:
            return "// Unknown";
        }
    }
}

static std::string WriteCondition(Instruction::FlowControlType instr) {
    auto component = [&](int i) {
        auto ref = (i == 0) ? instr.refx.Value() : instr.refy.Value();
        return fmt::format("{}cond.{}", (ref ? "" : "!"), "xy"[i]);
    };

    using Op = Instruction::FlowControlType::Op;

    switch (instr.op) {
    case Op::Or:
        return component(0) + " || " + component(1);

    case Op::And:
        return component(0) + " && " + component(1);

    case Op::JustX:
        return component(0);

    case Op::JustY:
        return component(1);
    }
}

void WriteBlocksFromTo(Context& context, std::string& glsl, const AnalyzedProgram& program, ShaderCodeOffset block_offset, ShaderCodeOffset end) {
    auto& block = program.blocks.at(block_offset);

    // TODO: Merge with function above that also defines this lambda
    auto is_same_block = [&program](ShaderCodeOffset block_start, ShaderCodeOffset b) {
        auto block_end = block_start.IncrementedBy(program.blocks.at(block_start).length);
        return !(b < block_start) && (b < block_end);
    };
    auto instr_bound = block_offset.IncrementedBy(block.length);
    if (is_same_block(block_offset, end) && end.GetNext() < instr_bound) {
        instr_bound = end;
    }
    if (block_offset != instr_bound) {
        glsl += fmt::format("// block: {:#x}..{:#x}\n", 4 * block_offset.value, 4 * instr_bound.GetPrevious().value);
    }

    for (auto instr_offset : ranges::views::iota(block_offset, instr_bound)) {
        if (instr_offset == end.GetNext()) {
            return;
        }
        glsl += TranslateToGLSL(context, instr_offset.ReadFrom(context.shader_memory));
        glsl += '\n';
    }

    if (is_same_block(block_offset, end)) {
        return;
    }

    if (block.successors.size() == 0) {
        return;
    } else if (block.successors.size() == 1) {
        auto successor_offset = block.successors.begin()->first;
        WriteBlocksFromTo(context, glsl, program, successor_offset, end);
    } else {
        using SuccessionVia = AnalyzedProgram::SuccessionVia;
        Mikage::ValidateContract(block.successors.size() == 2);
        std::pair<ShaderCodeOffset, SuccessionVia> branch_offsets[] =
            { *block.successors.begin(), *std::next(block.successors.begin()) };

        fmt::print(stderr, "branch offsets: {:#x} {:#x}, {} {}\n", branch_offsets[0].first.value*4, branch_offsets[1].first.value*4, Meta::to_underlying(branch_offsets[0].second), Meta::to_underlying(branch_offsets[1].second));

        // Move the "more interesting" block to index 0; the other one is expected to be a simple jump
        if (branch_offsets[1].second == SuccessionVia::BranchToIf) {
            std::swap(branch_offsets[0], branch_offsets[1]);
        }
        if (branch_offsets[1].second == SuccessionVia::ExitLoop) {
            std::swap(branch_offsets[0], branch_offsets[1]);
        }
        if (branch_offsets[1].second == SuccessionVia::CondJumpTo) {
            std::swap(branch_offsets[0], branch_offsets[1]);
        }
        if (branch_offsets[1].second == SuccessionVia::LoopEnd) {
            std::swap(branch_offsets[0], branch_offsets[1]);
        }

        auto branch_offset = block_offset.IncrementedBy(block.length - 1);
        auto branch_instr = branch_offset.ReadFrom(context.shader_memory).flow_control;

        if (branch_offsets[0].second == SuccessionVia::BranchToIf) {
            Mikage::ValidateContract(branch_offsets[1].second == SuccessionVia::BranchToElse || branch_offsets[1].second == SuccessionVia::JumpTo);
            // IFU branch is statically evaluated and hence should never show up here
            Mikage::ValidateContract(branch_offset.ReadFrom(context.shader_memory).opcode.Value().EffectiveOpCode() == OpCode::Id::IFC);

            auto post_dominator = block.post_dominator;
            if (post_dominator == ShaderCodeOffset { 0xffffffff }) {
                post_dominator = end;
            }

            glsl += fmt::format("if ({}) {{\n", WriteCondition(branch_instr));
            WriteBlocksFromTo(context, glsl, program, branch_offsets[0].first, post_dominator.GetPrevious());
            // Check if we have a non-empty else-body
            if (branch_offsets[1].second == SuccessionVia::BranchToElse) {
                // No else branch should be generated for IFU
                glsl += "} else {\n";
                WriteBlocksFromTo(context, glsl, program, branch_offsets[1].first, post_dominator.GetPrevious());
            }
            glsl += "}\n";

            WriteBlocksFromTo(context, glsl, program, post_dominator, end);
        } else if (branch_offsets[0].second == SuccessionVia::ExitLoop) {
            Mikage::ValidateContract(branch_offsets[1].second == SuccessionVia::JumpTo);

            // TODO: Read int uniforms from UBO instead of baking it into the generated shader
            auto uniform = context.shader_uniforms.i[branch_instr.int_uniform_id];
            glsl += fmt::format("a2 = {};\nfor (uint count = 0; count <= uint({}); a2 += {}, ++count) {{\n", uniform.y, uniform.x, uniform.z);
            auto return_site = ShaderCodeOffset { branch_instr.dest_offset };
            WriteBlocksFromTo(context, glsl, program, branch_offsets[1].first, return_site);
            glsl += "}\n";

            WriteBlocksFromTo(context, glsl, program, branch_offsets[0].first, end);
        } else if (branch_offsets[0].second == SuccessionVia::CondJumpTo) {
            Mikage::ValidateContract(branch_offsets[1].second == SuccessionVia::Fallthrough);

            // Assert that both paths converge.
            // TODO: This need not hold in general (seems to work for the majority of titles, though)
            auto post_dominator = block.post_dominator;
            Mikage::ValidateContract(post_dominator != ShaderCodeOffset { 0xffffffff });

            // Inline the target blocks
            // TODO: Could we benefit from turning these into functions?
            glsl += fmt::format("if ({}) {{\n", WriteCondition(branch_instr));
            WriteBlocksFromTo(context, glsl, program, branch_offsets[0].first, post_dominator.GetPrevious());
            glsl += "} else {\n";
            WriteBlocksFromTo(context, glsl, program, branch_offsets[1].first, post_dominator.GetPrevious());
            glsl += "}\n";

            WriteBlocksFromTo(context, glsl, program, post_dominator, end);
        } else if (branch_offsets[0].second == SuccessionVia::LoopEnd) {
            // This was the last instruction of the loop: Nothing else to do
            Mikage::ValidateContract(branch_offsets[1].second == SuccessionVia::JumpTo);
            return;
        } else {
            Mikage::ValidateContract(false);
        }
    }
}

std::string GenerateGLSL(Context& context, ShaderEngineImpl& engine, const AnalyzedProgram& program) {
    std::string glsl = "#version 460 core\n\n";

    // TODO: Un-global-ize...
    reg_index_map.clear();
    input_reg_map.clear();

    // Declare inputs
    // TODO: Instead of remapping these registers, can we just adjust the VertexInputAttributeDescription accordingly?
    std::bitset<16> initialized_registers;
    for (auto attribute_index : ranges::views::iota(uint32_t { 0 }, context.registers.vertex_attributes.GetNumTotalAttributes())) {
        auto register_index = context.registers.vs_input_register_map.GetRegisterForAttribute(attribute_index);
        input_reg_map[register_index] = fmt::format("inreg_{}_attrib_{}", register_index, attribute_index);
        initialized_registers[register_index] = true;

        // NOTE: We assume no two input registers share the same attribute
        //       index; we check this implicitly by having the host shader
        //       compiler fail due two input locations being the same in
        //       that case.
        glsl += fmt::format("layout(location = {}) in vec4 {};\n", attribute_index, input_reg_map[register_index]);
    }
    // Some games read from uninitialized shader registers and then just never
    // use the result. Hence we declare all possibly used input registers.
    // Notably, this happens during the first render of Nano Assault EX.
    for (auto register_index : ranges::views::indices(initialized_registers.size())) {
        if (!initialized_registers[register_index]) {
            input_reg_map[register_index] = fmt::format("dummy_inreg_{}", register_index);
            glsl += fmt::format("vec4 {} = vec4(0.0);\n", input_reg_map[register_index]);
        }
    }
    glsl += '\n';

    // Declare outputs
    for (unsigned output_reg = 0, output_attrib = 0; true; ++output_reg ) {
        if (output_reg >= 7) {
            throw Mikage::Exceptions::Invalid(  "Requested {} output attributes, but only {} were output by the shader",
                                                context.registers.vs_num_output_attributes.Value(), output_attrib);
        }

        if (!(context.registers.vs_output_register_mask.mask() & (1 << output_reg))) {
            continue;
        }

        const auto& output_register_map = context.registers.vs_output_attributes[output_attrib++];

        // TODO: For now, we assume the components of each output register
        //       are "similar" (e.g. all position or all texture coordinate).
        //       However, emulated content could mix and match different
        //       semantics in a single register.
        //
        // Either way, we want a static output mapping so that we don't need to make the fragment shader inputs depend on vertex shader configuration
        u32 semantics[4] = {
            output_register_map.map_x, output_register_map.map_y,
            output_register_map.map_z, output_register_map.map_w
        };

        using VSOutAttr = Regs::VSOutputAttributes;
        static std::map<VSOutAttr::Semantic, std::pair<int, const char*>> semantic_map = {{
            { VSOutAttr::POSITION_X, { 4, "output_pos" } },
            { static_cast<VSOutAttr::Semantic>(4), { 4, "output_quat" } },
            { VSOutAttr::COLOR_R, { 4, "output_col" } },
            { VSOutAttr::TEXCOORD0_U, { 2, "output_tex0" } },
            { VSOutAttr::TEXCOORD1_U, { 2, "output_tex1" } },
            { VSOutAttr::TEXCOORD2_U, { 2, "output_tex2" } },
            { static_cast<VSOutAttr::Semantic>(18), { 3, "output_view" } },
        }};

        auto primary_semantic_it = semantic_map.find(VSOutAttr::Semantic { semantics[0] });
        if (primary_semantic_it == semantic_map.end()) {
            throw std::runtime_error(fmt::format("Unknown shader output semantic {}", semantics[0]));
        }
        auto primary_semantic = primary_semantic_it->first;

        // NOTE: We currently assume the output registers use non-shuffled
        //       components that all use the same kind of semantic.
        //       E.g. we always expect the output position to be stored in a
        //       single output register, and texture coordinates to be written
        //       to separate registers each.
        // This loop checks for this assumption
        //
        // TODO: Super Mario 3D Land breaks this assumption in many ways:
        // * it stores TEXCOORD0_W in the register's w component
        // * it stores TEXCOORD0_UV and TEXCOORD1_UV in a single register
        for (auto i : ranges::views::indices(std::size(semantics))) {
            if (semantics[i] == Regs::VSOutputAttributes::INVALID) {
                continue;
            }

            fmt::print(stderr, "Semantic for register {}.{}: {}\n", output_reg, "xyzw"[i], semantics[i]);

            auto expected_difference = i;
            if (semantics[i] - Meta::to_underlying(primary_semantic) != expected_difference && semantics[i] != VSOutAttr::TEXCOORD0_W) {
                // Commented out to make Super Mario 3D Land work
//                throw std::runtime_error(fmt::format("Output register uses multiple semantics or shuffled semantic components: {} {} {}", Meta::to_underlying(primary_semantic), semantics[i], expected_difference));
            }
        }

        reg_index_map[output_reg] = primary_semantic_it->second.second;

        if (output_attrib == context.registers.vs_num_output_attributes) {
            break;
        }
    }
    // TODO: Make sure the shader writes the correct number of components for these!
    glsl += fmt::format("vec4 output_pos;\n"); // Written to gl_Position at the end of the shader
    glsl += fmt::format("layout(location = 0) out vec4 output_col;\n");
    glsl += fmt::format("layout(location = 1) out vec4 output_tex0;\n");
    glsl += fmt::format("layout(location = 2) out vec4 output_tex1;\n");
    glsl += fmt::format("layout(location = 3) out vec4 output_tex2;\n");
    glsl += fmt::format("vec4 output_quat;\n"); // Not used currently
    glsl += fmt::format("vec4 output_view;\n"); // Not used currently
    glsl += '\n';

    // Declare temporaries
    // TODO: Consider only declaring those temporaries actually used
    for (auto i : ranges::views::iota(0, 16)) {
        glsl += fmt::format("vec4 temp_{};\n", i);
    }
    glsl += "\n";

    // Declare index registers for relative addressing
    for (auto i : { 0, 1, 2 }) {
        glsl += fmt::format("int a{} = 0;\n", i);
    }
    glsl += "\n";

    // Declare conditional code registers
    glsl += "bvec2 cond = bvec2(false);\n\n";

    // Declare unforms
    glsl += R"(layout(std140, binding = 0) uniform Uniforms {
vec4 f[96];
} uniforms;

)";

    // Forward-declare functions
    for (auto& func : program.functions) {
        if (func.entry == program.entry_point) {
            // Skip main
            continue;
        }
        glsl += "void ";
        glsl += FunctionName(func.entry);
        glsl += "();\n";
    }
    glsl += "\n";

    for (auto& func : program.functions) {
        auto func_name =
                (func.entry == program.entry_point)
                ? "main" : FunctionName(func.entry);
        glsl += fmt::format("void {}() {{\n", func_name);

        WriteBlocksFromTo(context, glsl, program, func.entry, func.exit);

        glsl += "}\n\n";
    }

    return glsl;
}

} // namespace

} // namespace
