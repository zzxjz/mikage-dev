#pragma once

#include "shader_private.hpp"

#include <platform/gpu/float.hpp>

#include <framework/math_vec.hpp>

#include <boost/container/static_vector.hpp>

#include <range/v3/view/any_view.hpp>

#include <array>
#include <bitset>
#include <vector>

namespace Pica::VertexShader {

enum class MicroOpType {
    Add,
    Mul,
    Max,
    Min,
    Dot3,
    Dot4,

    Mov,
    Floor,

    MovToAddressReg,
    Rcp,
    Rsq,

    Mad,

    // Order made to match the PICA200's CompareOpType::Op
    Compare,
    CompareEq = Compare,
    CompareNeq,
    CompareLess,
    CompareLeq,
    CompareGt,
    CompareGeq,
    CompareLast = CompareGeq,

//    LegacyInterpret,

    Goto,
    Branch,
    Call,
    PopCallStack, // Return from function or conditionally exit loop

    /**
     * Doesn't do anything on its own other than setting up parameters required
     * to enter a loop. This micro op should always be followed by a Call micro op,
     * which actually starts the loop.
     */
    PrepareLoop,
    EnterLoop,

    // Order made to match the PICA200's FlowControlType::Op
    EvalCondition,
    EvalConditionOr = EvalCondition,
    EvalConditionAnd,
    EvalConditionJustX,
    EvalConditionJustY,
    EvalConditionLast = EvalConditionJustY,

    /**
     * Conditionally executes the succeeding micro op depending on the value
     * of a condition register. If the register value is false, execution skips
     * past that micro op to its immediate successor.
     */
    PredicateNext,

    End,

    // Debug utility to print out the current MicroOpOffset
    Trace,
};

struct RecompilerRuntimeContext;
using ShaderMicrocodeHandler = void(*)(RecompilerRuntimeContext&, uint32_t, uint32_t, uint32_t);

struct MicroOp {
    /**
     * Two-step initialized handler: During microcode generation, the \a type
     * member is initialized, and in a later pass replaced by the actual
     * handler to use at runtime.
     */
    union {
        MicroOpType type;
        ShaderMicrocodeHandler handler;
    };

    uint32_t arg0 = 0;
    uint32_t arg1 = 0;
    uint32_t arg2 = 0;
};

using MicroOpOffset = ShaderCodeOffsetBase<struct MicroOpTag>;

struct MicroCode {
    std::map<ShaderCodeOffset, MicroOpOffset> block_offsets;
    std::vector<MicroOp> ops;

    // TODO: Do this cleaner. Should be in an entirely different backend!
    std::string glsl;
};

struct RecompilerRuntimeContext {
    std::array<Math::Vec4<float24>, 16> input_registers;

    Math::Vec4<float24> temporary_registers[16];

    std::array<Math::Vec4<float24>, 96> float_uniforms;

    Math::Vec4<float24> output_registers[16];

    MicroOp* micro_ops_start = nullptr;
    MicroOp* next_micro_op = nullptr;

    // Two conditional codes are defined by the PICA200 architecture; the third one is for internal use
    bool conditional_code[3];

    // Four address registers:
    // * First one is always 0
    // * Two PICA200 address registers
    // * One PICA200 loop counter
    // The dummy register in the beginning allows for quick (branch-predictable) lookup of these registers
    // TODO: How many bits do these actually have?
    int32_t address_registers[4] = { 0, 0, 0, 0 };

    int32_t& LoopRegister() {
        return address_registers[3];
    }

    enum {
        INVALID_ADDRESS = 0xFFFFFFFF
    };

    struct CallStackElement {
        MicroOpOffset final_address;  // Address upon which we jump to return_address. TODO: Deprecated
        MicroOpOffset return_address; // Where to jump when leaving scope
        uint8_t repeat_counter;  // How often to repeat until this call stack element is removed
        uint8_t loop_increment;  // Which value to add to the loop counter after an iteration
                                 // TODO: Should this be a signed value? Does it even matter?
        MicroOpOffset loop_begin_address; // The start address of the current loop (0 if we're not in a loop)
    };

    // Parameters for next call stack element
    uint8_t next_repeat_counter = 0;
    uint8_t next_loop_increment = 0;

    // TODO: Is there a maximal size for this?
    // NOTE: This stack is never empty. (It will always at least contain a
    //       dummy element that always compares false against the current
    //       offset)
    boost::container::static_vector<CallStackElement, 16> call_stack;
    MicroOpOffset last_call_stack_address;
};

MicroCode RecompileToMicroCode( const struct AnalyzedProgram& program,
                                const uint32_t* instructions,
                                uint32_t num_instructions,
                                const uint32_t* swizzle_data,
                                uint32_t num_swizzle_slots,
                                std::bitset<16> bool_uniforms,
                                const decltype(Regs::vs_int_uniforms)& int_uniforms);

std::string DisassembleMicroOp(const MicroOp&);

} // namespace Pica::VertexShader
