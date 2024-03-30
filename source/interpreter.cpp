#include "arm/thumb.hpp"
#include "arm/arm_meta.hpp"
#include "arm/processor_default.hpp"
#include "arm/processor_interpreter.hpp"
#include "os.hpp"

#include <framework/exceptions.hpp>

#include <boost/algorithm/clamp.hpp>
#include <boost/context/detail/exception.hpp>
#include <boost/endian/buffers.hpp>
#include <boost/range/size.hpp>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <fstream>

using BitField::v1::ViewBitField;

void show_backtrace();

namespace Interpreter {

CPUContext::CPUContext(HLE::OS::OS* os, Setup* setup) : os(os), setup(setup)/*, cfl(std::make_unique<ControlFlowLogger>())*/ {

}

// CPUContext::CPUContext(const CPUContext& oth) : cfl(std::make_unique<ControlFlowLogger>()),
//     cpu{}, os(oth.os), setup(oth.setup)
//     {}

CPUContext::~CPUContext() = default;

ExecutionContext::ExecutionContext(Processor& parent_) : parent(parent_) {
    parent.contexts.push_back(this);
}

ExecutionContext::~ExecutionContext() {
    parent.UnregisterContext(*this);
}

template<typename T>
static void WritePhysicalMemory(Memory::PhysicalMemory& mem, uint32_t address, const T value) {
    Memory::WriteLegacy<T>(mem, address, value);
}

template<typename T>
static const T ReadPhysicalMemory(Memory::PhysicalMemory& mem, uint32_t address) {
    return Memory::ReadLegacy<T>(mem, address);
}

Setup::Setup(LogManager& log_manager, const KeyDatabase& keydb_,
            std::unique_ptr<Loader::GameCard> gamecard_, Profiler::Profiler& profiler,
            Debugger::DebugServer& debug_server)
    : mem(log_manager), keydb(keydb_), gamecard(std::move(gamecard_)), profiler(profiler), debug_server(debug_server) {
    for (auto i : {0,1}) {
        std::memset(&cpus[i].cpu, 0, sizeof(cpus[i].cpu));
        cpus[i].cpu.cp15.CPUId().CPUID = i;
        cpus[i].cpu.cpsr.mode = ARM::InternalProcessorMode::Supervisor;
        cpus[i].os = os.get();
        cpus[i].setup = this;
    }
}

Setup::~Setup() = default;

// struct ControlFlowLogger {
//     uint32_t indent = 0;
//
//     void Branch(CPUContext& ctx, const char* kind, uint32_t addr) {
//         MakeSureFileIsOpen(ctx);
//         os << fmt::format("{}-> {} {:#x}", GetIndent(), kind, addr) << std::endl;
//         ++indent;
//     }
//
//     void Return(CPUContext& ctx, const char* kind) {
//         MakeSureFileIsOpen(ctx);
//         --indent;
//         os << fmt::format("{}<- ({})", GetIndent(), kind) << std::endl;
//     }
//
// private:
//     std::string GetIndent() {
//         return std::string(3 * indent, ' ');
//     }
//
//     void MakeSureFileIsOpen(CPUContext& ctx) {
//         if (os)
//             return;
//
//         auto filename = fmt::format("./cfl_{}_{}.txt", ctx.os->active_thread->GetParentProcess().GetId(), ctx.os->active_thread->GetId());
//         os.open(filename);
//     }
//
//     std::ofstream os;
// };

// #define CONTROL_FLOW_LOGGING 1

void ControlFlowLogger::Branch(CPUContext& ctx, const char* kind, uint32_t addr) {
#ifdef CONTROL_FLOW_LOGGING
    MakeSureFileIsOpen(ctx);
    if (indent > 50) {
        os << fmt::format("====== CUT") << std::endl;
        indent = 0;
    }
    os << fmt::format("{}-> {} {:#x}", GetIndent(), kind, addr) << std::endl;
    ++indent;
#endif
}

void ControlFlowLogger::Return(CPUContext& ctx, const char* kind) {
#ifdef CONTROL_FLOW_LOGGING
    MakeSureFileIsOpen(ctx);
    if (indent) {
        --indent;
        os << fmt::format("{}<- ({}) from {:#x}", GetIndent(), kind, ctx.cpu.reg[15]) << std::endl;
    } else {
        os << fmt::format("====== ({}) from {:#x}", kind, ctx.cpu.reg[15]) << std::endl;
    }
#endif
}

void ControlFlowLogger::SVC(CPUContext& ctx, uint32_t id) {
#ifdef CONTROL_FLOW_LOGGING
    MakeSureFileIsOpen(ctx);
    os << fmt::format("{}svc {:#x}", GetIndent(), id) << std::endl;
#endif
}

void ControlFlowLogger::Log(CPUContext& ctx, const std::string& str) {
#ifdef CONTROL_FLOW_LOGGING
    MakeSureFileIsOpen(ctx);
    os << fmt::format("{}{}", GetIndent(), str) << std::endl;
#endif
}

std::string ControlFlowLogger::GetIndent() {
    // TODO: Just store this string internally instead of creating it over and over again...
    return std::string(3 * indent, ' ');
}

void ControlFlowLogger::MakeSureFileIsOpen(CPUContext& ctx) {
#ifdef CONTROL_FLOW_LOGGING
    if (os)
        return;

    auto filename = fmt::format("./cfl_{}_{}.txt", ctx.os->active_thread->GetParentProcess().GetId(), ctx.os->active_thread->GetId());
    os.open(filename);
#endif
}

template<typename T>
/*[[deprecated]]*/ static T ReadVirtualMemory(InterpreterExecutionContext& ctx, uint32_t address) {
    return ctx.ReadVirtualMemory<T>(address);
}

template<typename T>
/*[[deprecated]]*/ static void WriteVirtualMemory(InterpreterExecutionContext& ctx, uint32_t address, T value) {
    return ctx.WriteVirtualMemory(address, value);
}

using InterpreterARMHandler = std::add_pointer<uint32_t(InterpreterExecutionContext&, ARM::ARMInstr)>::type;

static uint32_t HandlerStubWithMessage(CPUContext& ctx, ARM::ARMInstr instr, const std::string& message) {
    std::string error = fmt::format("Unknown instruction {:#010x} (PC is {:#x})", instr.raw, ctx.cpu.PC());
    if (!message.empty())
        error += ": " + message;
    error += '\n';

    throw std::runtime_error(error);
}

static uint32_t HandlerStub(CPUContext& ctx, ARM::ARMInstr instr) {
    return HandlerStubWithMessage(ctx, instr, "");
}

static uint32_t HandlerStubAnnotated(CPUContext& ctx, ARM::ARMInstr instr, unsigned line) {
    return HandlerStubWithMessage(ctx, instr, "(at line " + std::to_string(line) + ")");
}

static void Link(CPUContext& ctx) {
    ctx.cpu.LR() = ctx.cpu.PC() + 4;
}

static uint32_t NextInstr(CPUContext& ctx) {
    return ctx.cpu.PC() + 4;
}

static uint32_t HandlerSkip(CPUContext& ctx, ARM::ARMInstr instr, const std::string& message) {
//    std::cerr << "Skipping instruction 0x" << std::hex << std::setw(8) << std::setfill('0') << instr.raw;

//    if (!message.empty())
//        std::cerr << ": " << message;
//    std::cerr << std::endl;

    return NextInstr(ctx);
}

// Copies the MSB of the given value to the CPSR N flag
static void UpdateCPSR_N(CPUContext& ctx, uint32_t val) {
    ctx.cpu.cpsr.neg = (val >> 31);
}

// Updates the CPSR Z flag with the contents of the given value (sets the flag if the value is zero, unsets it otherwise)
static void UpdateCPSR_Z(CPUContext& ctx, uint32_t val) {
    ctx.cpu.cpsr.zero = (val == 0);
}

static void UpdateCPSR_C(CPUContext& ctx, bool val) {
    ctx.cpu.cpsr.carry = val;
}

static bool GetCarry(uint32_t left, uint32_t right) {
    return ((left >> 31) + (right >> 31) > ((left+right) >> 31));
}

static bool GetCarry(uint32_t left, uint32_t right, uint32_t cpsr_c) {
    return ((left >> 31) + (right >> 31) > ((left+right+cpsr_c) >> 31));
}

// TODO: Unify this with GetCarry!
template<typename T>
static bool GetCarryT(T left, T right) {
    static_assert(std::is_unsigned<T>::value, "Given type must be unsigned!");
    using sign_bit = std::integral_constant<size_t, sizeof(T) * CHAR_BIT - 1>;
    return ((left >> sign_bit::value) + (right >> sign_bit::value) > (static_cast<T>(left+right) >> sign_bit::value));
}

// TODO: Unify this with GetCarry!
template<typename T>
static bool GetCarryT(T left, T right, T cpsr_c) {
    static_assert(std::is_unsigned<T>::value, "Given type must be unsigned!");
    using sign_bit = std::integral_constant<size_t, sizeof(T) * CHAR_BIT - 1>;
    return ((left >> sign_bit::value) + (right >> sign_bit::value) > (static_cast<T>(left+right+cpsr_c) >> sign_bit::value));
}

static void UpdateCPSR_C_FromCarry(CPUContext& ctx, uint32_t left, uint32_t right) {
    ctx.cpu.cpsr.carry = GetCarry(left, right);
}

static void UpdateCPSR_C_FromCarry(CPUContext& ctx, uint32_t left, uint32_t right, uint32_t cpsr_c) {
    ctx.cpu.cpsr.carry = GetCarry(left, right, cpsr_c);
}

static void UpdateCPSR_C_FromBorrow(CPUContext& ctx, uint32_t left, uint32_t right) {
    bool borrow = left < right;
    ctx.cpu.cpsr.carry = !borrow;
}

static void UpdateCPSR_C_FromBorrow(CPUContext& ctx, uint32_t left, uint32_t right, uint32_t cpsr_c) {
    bool borrow = left < (static_cast<uint64_t>(right) + !cpsr_c);
    ctx.cpu.cpsr.carry = !borrow;
}

static bool GetOverflowFromAdd(uint32_t left, uint32_t right, uint32_t result) {
    return (~(left ^ right) & (left ^ result) & (right ^ result)) >> 31;
}

static void UpdateCPSR_V_FromAdd(CPUContext& ctx, uint32_t left, uint32_t right, uint32_t result) {
    // TODO: Not sure if this works fine for computations including the cpsr.carry!
    ctx.cpu.cpsr.overflow = GetOverflowFromAdd(left, right, result);
}

static void UpdateCPSR_V_FromSub(CPUContext& ctx, uint32_t left, uint32_t right, uint32_t result) {
    ctx.cpu.cpsr.overflow = ((left ^ right) & (left ^ result)) >> 31;
}

static void UpdateCPSR_V_FromSub(CPUContext& ctx, uint32_t left, uint32_t right, uint32_t result, uint32_t carry) {
    right = ~right;
    // TODO: Portability!
    uint64_t signed_sum = static_cast<int64_t>(static_cast<int32_t>(left)) + static_cast<int64_t>(static_cast<int32_t>(right)) + static_cast<uint64_t>(carry);
    ctx.cpu.cpsr.overflow = static_cast<int64_t>(static_cast<int32_t>(result)) != signed_sum;
}

// Evaluates the given condition based on CPSR
static bool EvalCond(CPUContext& ctx, uint32_t cond) {
    if (cond == 0xE/* || cond == 0xF*/) { // always (0xF apparently is never?)
        return true;
    } else if (cond == 0x0) { // Equal
        return (ctx.cpu.cpsr.zero == 1);
    } else if (cond == 0x1) { // Not Equal
        return (ctx.cpu.cpsr.zero == 0);
    } else if (cond == 0x2) { // Greater Equal (unsigned)
        return (ctx.cpu.cpsr.carry == 1);
    } else if (cond == 0x3) { // Less Than (unsigned)
        return (ctx.cpu.cpsr.carry == 0);
    } else if (cond == 0x4) { // Negative
        return (ctx.cpu.cpsr.neg == 1);
    } else if (cond == 0x5) { // Positive or Zero
        return (ctx.cpu.cpsr.neg == 0);
    } else if (cond == 0x8) { // Greater (unsigned)
        return (ctx.cpu.cpsr.carry == 1 && ctx.cpu.cpsr.zero == 0);
    } else if (cond == 0x9) { // Less Equal (unsigned)
        return (ctx.cpu.cpsr.carry == 0 || ctx.cpu.cpsr.zero == 1);
    } else if (cond == 0xa) { // Greater Equal (signed)
        return (ctx.cpu.cpsr.neg == ctx.cpu.cpsr.overflow);
    } else if (cond == 0xb) { // Less Than (signed)
        return (ctx.cpu.cpsr.neg != ctx.cpu.cpsr.overflow);
    } else if (cond == 0xc) { // Greater Than (signed)
        return (ctx.cpu.cpsr.zero == 0 && ctx.cpu.cpsr.neg == ctx.cpu.cpsr.overflow);
    } else if (cond == 0xd) { // Less Equal (signed)
        return (ctx.cpu.cpsr.zero == 1 || ctx.cpu.cpsr.neg != ctx.cpu.cpsr.overflow);
    }

    throw std::runtime_error("Condition not implemented");
}

void CPUContext::RecordCall(uint32_t source, uint32_t target, ARM::State state) {
//    Callsite entry;
//    entry.source = source;
//    entry.target = target;

//    // TODO: Fix ARM::State to allow a plain copy here!
//    memcpy(&entry.state, &state, sizeof(state));
//    entry.state.reg[15] = target;

//    backtrace.push_back(entry);
}

template<bool link>
static uint32_t HandlerBranch(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (link)
        Link(ctx);

    uint32_t target = ctx.cpu.PC() + 8 + 4 * instr.branch_target;
    if (link) {
        ctx.RecordCall(ctx.cpu.PC(), target, ctx.cpu);
        ctx.cfl.Branch(ctx, "bl", target);
    }
    return target;
}

template<bool link>
static uint32_t HandlerBranchExchange(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if ((instr.identifier_4_23 & ~0b10) != 0b0010'1111'1111'1111'0001)
        return HandlerStub(ctx, instr);

    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // The link register may be used as the target specifier, hence store
    // the register value before linking.
    auto target = ctx.cpu.reg[instr.idx_rm];

    if (instr.idx_rm == 14) {
        ctx.cfl.Return(ctx, "bx lr");
    }

    if (link)
        Link(ctx);

    ctx.cpu.cpsr.thumb = (target & 1);

    if (link) {
        ctx.RecordCall(ctx.cpu.PC(), target & ~UINT32_C(1), ctx.cpu);
        ctx.cfl.Branch(ctx, "blx", target);
    }

    return target & ~UINT32_C(1);
}

static uint32_t HandlerCPS(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Abort early for Unpredictable configurations
    if (instr.cps_imod_enable == 0 && instr.cps_imod_value == 0 && instr.cps_mmod == 0)
        return HandlerStubAnnotated(ctx, instr, __LINE__);
    if (instr.cps_imod_enable == 0 && instr.cps_imod_value == 1)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    if (!ctx.cpu.cpsr.InPrivilegedMode())
        return NextInstr(ctx);

    if (instr.cps_imod_enable) {
        if (instr.cps_A)
            ctx.cpu.cpsr.A = instr.cps_imod_value;
        if (instr.cps_I)
            ctx.cpu.cpsr.I = instr.cps_imod_value;
        if (instr.cps_F)
            ctx.cpu.cpsr.F = instr.cps_imod_value;
    }

    if (instr.cps_mmod) {
        ctx.cpu.cpsr.mode = MakeInternal(instr.cps_mode.Value());
        // TODO: Changing to an reserved system mode is Unpredictable
    }

    return NextInstr(ctx);
}

static uint32_t RotateRight(uint32_t value, uint32_t bits) {
    // NOTE: shifting by 32 bits is undefined behavior, hence we add a special
    //       case for these values here.
    if (bits == 0 || bits == 32)
        return value;
    else
        return (value >> bits) | (value << (32 - bits));
}

struct ShifterOperand {
    uint32_t value;
    bool carry_out;
};

template<typename T>
static uint32_t ArithmeticShiftRight(T val, uint32_t bits) = delete;

/**
 * Preconditions:
 * - 0 < bits < 32
 */
static uint32_t ArithmeticShiftRight(uint32_t val, uint32_t bits) {
    uint32_t msb = val >> 31;
    return (val >> bits) | (msb * (0xFFFFFFFF << (32 - bits)));
}

// NOTE: For immediate shifts, use CalcShifterOperandFromImmediate instead!
// NOTE: ROR_RRX is always executed as a rotate in this function.
// NOTE: Only the least significant 8 bits of shift_value are considered
static std::optional<ShifterOperand> CalcShifterOperand(uint32_t value, uint32_t shift_value, ARM::OperandShifterMode mode, bool carry) {
    ShifterOperand ret;

    // Mask out upper bits
    shift_value &= 0xFF;

    switch (mode) {
    case ARM::OperandShifterMode::LSL:
        ret.value     = shift_value <  32 ? (value << shift_value)
                      :                     0;
        ret.carry_out = shift_value ==  0 ? carry
                      : shift_value <= 32 ? ((value << (shift_value - 1)) >> 31)
                      :                     0;
        return ret;

    case ARM::OperandShifterMode::LSR:
        ret.value     = shift_value <  32 ? (value >> shift_value)
                      :                     0;
        ret.carry_out = shift_value ==  0 ? carry
                      : shift_value <= 32 ? ((value >> (shift_value - 1)) & 1)
                      :                     0;
        return ret;

    case ARM::OperandShifterMode::ASR:
        ret.value     = shift_value ==  0 ? value
                      : shift_value <= 31 ? ArithmeticShiftRight(value, shift_value)
                      :                     (0xFFFFFFFF * (value >> 31));
        ret.carry_out = shift_value ==  0 ? carry
                      : shift_value <  32 ? (ArithmeticShiftRight(value, shift_value - 1) & 1)
                      :                     (value >> 31);

        return ret;

    case ARM::OperandShifterMode::ROR_RRX:
        // This mode only considers the least significant 5 bits in shift_value
        ret.value     = (shift_value & 0x1F) == 0 ? value
                      :                             RotateRight(value, shift_value & 0x1F);
        ret.carry_out =  shift_value         == 0 ? carry
                      : (shift_value & 0x1F) == 0 ? (value >> 31)
                      :                             (RotateRight(value, (shift_value & 0x1F) - 1) & 1);
        return ret;

    default:
        return {};
    }

    return ret;
}

static std::optional<ShifterOperand> CalcShifterOperandFromImmediate(uint32_t value, uint32_t shift_value, ARM::OperandShifterMode mode, bool carry) {
    switch (mode) {
    case ARM::OperandShifterMode::LSL:
        return CalcShifterOperand(value, shift_value, mode, carry);

    case ARM::OperandShifterMode::LSR:
    case ARM::OperandShifterMode::ASR:
        return CalcShifterOperand(value, shift_value ? shift_value : 32, mode, carry);

    case ARM::OperandShifterMode::ROR_RRX:
        if (shift_value != 0) {
            return CalcShifterOperand(value, shift_value, mode, carry);
        } else {
            // Rotate Right with Extend by 33 bits with C as the 33rd bit
            ShifterOperand ret;
            ret.value     = ((uint32_t)carry << 31) | (value >> 1);
            ret.carry_out = value & 1;
            return ret;
        }

    default:
        return {};
    }
}

static std::optional<ShifterOperand> GetAddr1ShifterOperand(CPUContext& ctx, ARM::ARMInstr instr) {
    switch (ARM::GetAddrMode1Encoding(instr)) {
    case ARM::AddrMode1Encoding::Imm:
    {
        // Rotate immediate by an even amount of bits
        auto result = RotateRight(instr.immed_8, 2 * instr.rotate_imm);
        bool carry_out = instr.rotate_imm ? (result >> 31) : ctx.cpu.cpsr.carry.Value();
        return { {result, carry_out} };
    }

    case ARM::AddrMode1Encoding::ShiftByImm:
    {
        auto reg = ctx.cpu.FetchReg(instr.idx_rm);
        return CalcShifterOperandFromImmediate(reg, instr.addr1_shift_imm, instr.addr1_shift, ctx.cpu.cpsr.carry);
    }

    case ARM::AddrMode1Encoding::ShiftByReg:
    {
        // NOTE: Chosing R15 for Rd, Rm, Rn, or Rs has Unpredictable results.
        auto reg = ctx.cpu.FetchReg(instr.idx_rm);
        return CalcShifterOperand(reg, ctx.cpu.FetchReg(instr.idx_rs), instr.addr1_shift, ctx.cpu.cpsr.carry);
    }

    default:
        return {};
    }
}

static uint32_t HandlerMov(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.addr1_S && instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    ctx.cpu.reg[instr.idx_rd] = shifter_operand->value;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_C(ctx, shifter_operand->carry_out);
        // V unaffected
    }

    if (instr.idx_rd == ARM::Regs::PC) {
        return ctx.cpu.PC();
    } else {
        return NextInstr(ctx);
    }
}

// Move Not
static uint32_t HandlerMvn(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    ctx.cpu.reg[instr.idx_rd] = ~shifter_operand->value;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_C(ctx, shifter_operand->carry_out);
        // V unaffected
    }

    return NextInstr(ctx);
}

// Bit Clear
static uint32_t HandlerBic(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    ctx.cpu.reg[instr.idx_rd] = ctx.cpu.reg[instr.idx_rn] & ~shifter_operand->value;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_C(ctx, shifter_operand->carry_out);
        // V unaffected
    }

    return NextInstr(ctx);
}

// Exclusive OR
static uint32_t HandlerEor(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    ctx.cpu.reg[instr.idx_rd] = ctx.cpu.reg[instr.idx_rn] ^ shifter_operand->value;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_C(ctx, shifter_operand->carry_out);
        // V unaffected
    }

    return NextInstr(ctx);
}

static uint32_t HandlerMul(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    // sic: Rn defines the output here.
    ctx.cpu.reg[instr.idx_rn] = ctx.cpu.FetchReg(instr.idx_rm) * ctx.cpu.FetchReg(instr.idx_rs);

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rn]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rn]);
        // On ARMv4 and earlier, C is unpredictable, while on newer ISAs it's unaffected.
        // V unaffected
    }

    return NextInstr(ctx);
}

static uint32_t HandlerMla(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    // sic: Rn defines the output here, while Rd is an input
    ctx.cpu.reg[instr.idx_rn] = ctx.cpu.FetchReg(instr.idx_rm) * ctx.cpu.FetchReg(instr.idx_rs) + ctx.cpu.FetchReg(instr.idx_rd);

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rn]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rn]);
        // On ARMv4 and earlier, C is unpredictable, while on newer ISAs it's unaffected.
        // V unaffected
    }

    return NextInstr(ctx);
}

static uint32_t HandlerAnd(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    ctx.cpu.reg[instr.idx_rd] = ctx.cpu.FetchReg(instr.idx_rn) & shifter_operand->value;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_C(ctx, shifter_operand->carry_out);
        // V unaffected
    }

    return NextInstr(ctx);
}

// Logical OR
static uint32_t HandlerOrr(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    ctx.cpu.reg[instr.idx_rd] = ctx.cpu.FetchReg(instr.idx_rn) | shifter_operand->value;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_C(ctx, shifter_operand->carry_out);
        // V unaffected
    }

    return NextInstr(ctx);
}

// Test Equivalence
static uint32_t HandlerTeq(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    uint32_t alu_out = ctx.cpu.FetchReg(instr.idx_rn) ^ shifter_operand->value;

    UpdateCPSR_N(ctx, alu_out);
    UpdateCPSR_Z(ctx, alu_out);
    UpdateCPSR_C(ctx, shifter_operand->carry_out);
    // V unaffected

    return NextInstr(ctx);
}

// Test
static uint32_t HandlerTst(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    uint32_t alu_out = ctx.cpu.FetchReg(instr.idx_rn) & shifter_operand->value;

    UpdateCPSR_N(ctx, alu_out);
    UpdateCPSR_Z(ctx, alu_out);
    UpdateCPSR_C(ctx, shifter_operand->carry_out);
    // V unaffected

    return NextInstr(ctx);
}

static uint32_t HandlerAdd(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.addr1_S && instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    uint32_t rn = ctx.cpu.FetchReg(instr.idx_rn);
    ctx.cpu.reg[instr.idx_rd] = rn + shifter_operand->value;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_C_FromCarry(ctx, rn, shifter_operand->value);
        UpdateCPSR_V_FromAdd(ctx, rn, shifter_operand->value, ctx.cpu.reg[instr.idx_rd]);
    }

    if (instr.idx_rd == ARM::Regs::PC) {
        return ctx.cpu.reg[ARM::Regs::PC];
    } else {
        return NextInstr(ctx);
    }
}

static uint32_t HandlerAdc(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    uint32_t rn = ctx.cpu.FetchReg(instr.idx_rn);
    ctx.cpu.reg[instr.idx_rd] = rn + shifter_operand->value + ctx.cpu.cpsr.carry;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_C_FromCarry(ctx, rn, shifter_operand->value, ctx.cpu.cpsr.carry);
        UpdateCPSR_V_FromAdd(ctx, rn, shifter_operand->value, ctx.cpu.reg[instr.idx_rd]);
    }

    return NextInstr(ctx);
}

// Subtract with carry
static uint32_t HandlerSbc(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    uint32_t rn = ctx.cpu.FetchReg(instr.idx_rn);
    ctx.cpu.reg[instr.idx_rd] = rn - shifter_operand->value - !ctx.cpu.cpsr.carry;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_V_FromSub(ctx, rn, shifter_operand->value, ctx.cpu.reg[instr.idx_rd], ctx.cpu.cpsr.carry);
        UpdateCPSR_C_FromBorrow(ctx, rn, shifter_operand->value, ctx.cpu.cpsr.carry);
    }

    return NextInstr(ctx);
}

// Reverse Subtract with Carry
static uint32_t HandlerRsc(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    uint32_t rn = ctx.cpu.FetchReg(instr.idx_rn);
    ctx.cpu.reg[instr.idx_rd] = shifter_operand->value - rn - !ctx.cpu.cpsr.carry;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_V_FromSub(ctx, shifter_operand->value, rn, ctx.cpu.reg[instr.idx_rd], ctx.cpu.cpsr.carry);
        UpdateCPSR_C_FromBorrow(ctx, shifter_operand->value, rn, ctx.cpu.cpsr.carry);
    }

    return NextInstr(ctx);
}

static uint32_t HandlerSub(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    uint32_t rn = ctx.cpu.FetchReg(instr.idx_rn);
    ctx.cpu.reg[instr.idx_rd] = rn - shifter_operand->value;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_C_FromBorrow(ctx, rn, shifter_operand->value);
        UpdateCPSR_V_FromSub(ctx, rn, shifter_operand->value, ctx.cpu.reg[instr.idx_rd]);
    }

    return NextInstr(ctx);
}

// Reverse Subtract
static uint32_t HandlerRsb(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Using PC as Rd yields unpredictable behavior in some cases");

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    uint32_t rn = ctx.cpu.FetchReg(instr.idx_rn);
    ctx.cpu.reg[instr.idx_rd] = shifter_operand->value - rn;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd]);
        UpdateCPSR_C_FromBorrow(ctx, shifter_operand->value, rn);
        UpdateCPSR_V_FromSub(ctx, shifter_operand->value, rn, ctx.cpu.reg[instr.idx_rd]);
    }

    return NextInstr(ctx);
}

template<ARM::AddrMode3AccessType AccessType>
static uint32_t HandlerAddrMode3(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.idx_rd == 15)
        return HandlerStubWithMessage(ctx, instr, "Configuration not implemented");

    if (!instr.ldr_P && instr.ldr_W)
        return HandlerStubWithMessage(ctx, instr, "Unpredictable configuration - maybe this isn't LDR/STR?");

    // P=0: Memory access using base register; after the access, the base register has the offset applied to it (post-indexed addressing)
    // P=0, W=0: normal memory access using base register
    // P=0, W=1: unpredictable
    // P=1, W=0: memory access using base register with applied offset (base register remains unchanged).
    // P=1, W=1: memory access using base register with applied offset (base register will be updated).

    uint32_t address = ctx.cpu.reg[instr.idx_rn];

    bool Imm = BitField::v1::ViewBitField<22, 1, uint32_t>(instr.raw);
    uint32_t offset = (instr.ldr_U ? 1 : -1)
                      * (Imm ? ((instr.addr3_immed_hi << 4) | instr.addr3_immed_lo)
                             : ctx.cpu.reg[instr.idx_rm]);
    if (instr.ldr_P)
        address += offset;

    // TODO: Need to take care of shared memory magic for store instructions!
    switch (AccessType) {
    case ARM::AddrMode3AccessType::LoadSignedByte:
        // Load with sign extend
        ctx.cpu.reg[instr.idx_rd] = (int8_t)ReadVirtualMemory<uint8_t>(ctx, address);
        break;

    case ARM::AddrMode3AccessType::StoreByte:
        WriteVirtualMemory<uint8_t>(ctx, address, ctx.cpu.reg[instr.idx_rd]);
        break;

    case ARM::AddrMode3AccessType::LoadSignedHalfword:
        // Load with sign extend
        // TODO: If CP15 is configured appropriately, bit0 of address may be non-zero
//        if ((address & 0x1) != 0)
//            return HandlerStubWithMessage(ctx, instr, "Unpredictable configuration");

        ctx.cpu.reg[instr.idx_rd] = (int16_t)ReadVirtualMemory<uint16_t>(ctx, address);
        break;

    case ARM::AddrMode3AccessType::LoadUnsignedHalfword:
        // TODO: If CP15 is configured appropriately, bit0 of address may be non-zero
//        if ((address & 0x1) != 0)
//            return HandlerStubWithMessage(ctx, instr, "Unpredictable configuration");

        ctx.cpu.reg[instr.idx_rd] = ReadVirtualMemory<uint16_t>(ctx, address);
        break;

    case ARM::AddrMode3AccessType::StoreHalfword:
        // TODO: If CP15 is configured appropriately, bit0 of address may be non-zero
        // NOTE: CP15 by default is configured appropriately to support this on the 3DS!
        //if ((address & 0x1) != 0)
        //    return HandlerStubWithMessage(ctx, instr, "Unpredictable configuration");

        WriteVirtualMemory<uint16_t>(ctx, address, ctx.cpu.reg[instr.idx_rd]);
        break;

    case ARM::AddrMode3AccessType::LoadDoubleword:
        // TODO: If CP15 is configured appropriately, bit2 of address may be non-zero
        if ((instr.idx_rd % 2) && instr.idx_rd != 14 && (address & 0x7) != 0)
            return HandlerStubWithMessage(ctx, instr, "Unpredictable configuration");

        ctx.cpu.reg[instr.idx_rd]   = ReadVirtualMemory<uint32_t>(ctx, address);
        ctx.cpu.reg[instr.idx_rd+1] = ReadVirtualMemory<uint32_t>(ctx, address+4);
        break;

    case ARM::AddrMode3AccessType::StoreDoubleword:
        // TODO: If CP15 is configured appropriately, bit2 of address may be non-zero
        if ((instr.idx_rd % 2) && instr.idx_rd != 14 && (address & 0x7) != 0)
            return HandlerStubWithMessage(ctx, instr, "Unpredictable configuration");

        WriteVirtualMemory<uint32_t>(ctx, address,   ctx.cpu.reg[instr.idx_rd]);
        WriteVirtualMemory<uint32_t>(ctx, address+4, ctx.cpu.reg[instr.idx_rd+1]);
        break;

    default:
        return HandlerStubWithMessage(ctx, instr, "Not an addressing mode 3 instruction - configuration not implemented");
    }

    if (!instr.ldr_P)
        address += offset;

    // Update base register if necessary
    if (!instr.ldr_P || instr.ldr_W) {
        ctx.cpu.reg[instr.idx_rn] = address;

        if (instr.idx_rn == 15) {
            // TODO: Unknown behavior for PC
            return HandlerStubAnnotated(ctx, instr, __LINE__);
        }
    }

    return NextInstr(ctx);
}

// Signed Multiply
static uint32_t HandlerSmulxx(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // If either of the operand registers are PC, abort - this is unpredictable behavior!
    if (instr.idx_rn == 15 || instr.idx_rs == 15 || instr.idx_rm == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // TODO: These computations have a chance of overflowing, which is ill-defined in C++ code!

    // Extract lower or bottom 16 bit depending on the given instruction and then convert to a signed integer and sign-extend to 32-bit
    uint32_t input1_shift = 16 * ViewBitField<5,1,uint32_t>(instr.raw);
    int32_t input1 = static_cast<int16_t>(static_cast<uint16_t>(ctx.cpu.FetchReg(instr.idx_rm) >> input1_shift));

    bool input2_shift = ViewBitField<6,1,uint32_t>(instr.raw);
    int32_t input2 = static_cast<int16_t>(static_cast<uint16_t>(ctx.cpu.FetchReg(instr.idx_rs) >> input2_shift));

    // sic: indeed, rd is used as the input operand, while rn is the output operand
    // TODO: This multiplication may not be well-defined C++ due to signed overflow
    // NOTE: The result indeed is stored in Rn
    ctx.cpu.reg[instr.idx_rn] = input1 * input2;

    return NextInstr(ctx);
}

// Unsigned Multiply Accumulate Accumulate Long
static uint32_t HandlerUmaal(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // If either of the destination registers are PC, abort - this is unpredictable behavior!
    if (instr.idx_rm == 15 || instr.idx_rs == 15 || instr.idx_rd == 15 || instr.idx_rn == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // If the destination registers are equal, abort - this is unpredictable behavior!
    if (instr.idx_rd == instr.idx_rn)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // First compute the result using the original register values, then write it back
    uint64_t result = static_cast<uint64_t>(ctx.cpu.FetchReg(instr.idx_rs)) * static_cast<uint64_t>(ctx.cpu.FetchReg(instr.idx_rm));
    result += ctx.cpu.reg[instr.idx_rd];
    result += ctx.cpu.reg[instr.idx_rn];
    ctx.cpu.reg[instr.idx_rd] = result & 0xFFFFFFFF;
    ctx.cpu.reg[instr.idx_rn] = result >> 32;

    return NextInstr(ctx);
}

// Signed Multiply Long
static uint32_t HandlerSmull(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // If either of the destination registers are PC, abort - this is unpredictable behavior!
    if (instr.idx_rd == 15 || instr.idx_rn == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // NOTE: This is actually not unpredictable, apparently.
    if (instr.idx_rd == instr.idx_rn)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // Cast from uint32_t to int32_t before casting to 64-bit to have proper sign-extension.
    uint64_t result = static_cast<int64_t>(static_cast<int32_t>(ctx.cpu.FetchReg(instr.idx_rs))) * static_cast<int64_t>(static_cast<int32_t>(ctx.cpu.FetchReg(instr.idx_rm)));
    ctx.cpu.reg[instr.idx_rd] = result & 0xFFFFFFFF;
    ctx.cpu.reg[instr.idx_rn] = result >> 32;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rn]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd] | ctx.cpu.reg[instr.idx_rn]);
        // C and V are unpredictable on ARMv4 and earlier (otherwise, they are unaffected)
    }

    return NextInstr(ctx);
}

// Unsigned Multiply Long
static uint32_t HandlerUmull(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // If either of the destination registers are PC, abort - this is unpredictable behavior!
    if (instr.idx_rd == 15 || instr.idx_rn == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // NOTE: This is actually not unpredictable, apparently.
    if (instr.idx_rd == instr.idx_rn)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    uint64_t result = static_cast<uint64_t>(ctx.cpu.FetchReg(instr.idx_rs)) * static_cast<uint64_t>(ctx.cpu.FetchReg(instr.idx_rm));
    ctx.cpu.reg[instr.idx_rd] = result & 0xFFFFFFFF;
    ctx.cpu.reg[instr.idx_rn] = result >> 32;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rn]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd] | ctx.cpu.reg[instr.idx_rn]);
        // C and V are unpredictable on ARMv4 and earlier (otherwise, they are unaffected)
    }

    return NextInstr(ctx);
}

// Signed Multiply Accumulate (16-bit)
static uint32_t HandlerSmlaxx(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // If either of the operand registers are PC, abort - this is unpredictable behavior!
    if (instr.idx_rd == 15 || instr.idx_rn == 15 || instr.idx_rs == 15 || instr.idx_rm == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // TODO: These computations have a chance of overflowing, which is ill-defined in C++ code!

    // Extract lower or bottom 16 bit depending on the given instruction and then convert to a signed integer and sign-extend to 32-bit
    uint32_t input1_shift = 16 * ViewBitField<5,1,uint32_t>(instr.raw);
    int32_t input1 = static_cast<int16_t>(static_cast<uint16_t>(ctx.cpu.FetchReg(instr.idx_rm) >> input1_shift));

    bool input2_shift = ViewBitField<6,1,uint32_t>(instr.raw);
    int32_t input2 = static_cast<int16_t>(static_cast<uint16_t>(ctx.cpu.FetchReg(instr.idx_rs) >> input2_shift));

    // sic: indeed, rd is used as the input operand, while rn is the output operand
    uint32_t result = input1 * input2 + static_cast<int32_t>(ctx.cpu.FetchReg(instr.idx_rd));

    if (GetOverflowFromAdd(input1 * input2, static_cast<int32_t>(ctx.cpu.FetchReg(instr.idx_rd)), result))
        ctx.cpu.cpsr.q = 1;

    ctx.cpu.reg[instr.idx_rn] = result;

    return NextInstr(ctx);
}


// Signed Multiply Accumulate Long
static uint32_t HandlerSmlal(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // If either of the destination registers are PC, abort - this is unpredictable behavior!
    if (instr.idx_rd == 15 || instr.idx_rn == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // NOTE: This is actually not unpredictable, apparently.
    if (instr.idx_rd == instr.idx_rn)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // Cast from uint32_t to int32_t before casting to 64-bit to have proper sign-extension.
    uint64_t result = static_cast<int64_t>(static_cast<int32_t>(ctx.cpu.FetchReg(instr.idx_rs))) * static_cast<int64_t>(static_cast<int32_t>(ctx.cpu.FetchReg(instr.idx_rm)));
    result += ctx.cpu.reg[instr.idx_rd]; // Accumulate low part
    result += static_cast<uint64_t>(ctx.cpu.reg[instr.idx_rn]) << 32; // Accumulate high part
    ctx.cpu.reg[instr.idx_rd] = result & 0xFFFFFFFF;
    ctx.cpu.reg[instr.idx_rn] = result >> 32;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rn]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd] | ctx.cpu.reg[instr.idx_rn]);
        // C and V are unpredictable on ARMv4 and earlier (on later versions, they are unaffected)
    }

    return NextInstr(ctx);
}
// Unsigned Multiply Accumulate Long
static uint32_t HandlerUmlal(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // If either of the destination registers are PC, abort - this is unpredictable behavior!
    if (instr.idx_rd == 15 || instr.idx_rn == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // If the destination registers are equal, abort - this is unpredictable behavior!
    if (instr.idx_rd == instr.idx_rn)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // First compute the result, then add the prior uint64_t value of the destination registers, then store back the result.
    uint64_t result = static_cast<uint64_t>(ctx.cpu.FetchReg(instr.idx_rs)) * static_cast<uint64_t>(ctx.cpu.FetchReg(instr.idx_rm));
    result += ctx.cpu.reg[instr.idx_rd];
    result += static_cast<uint64_t>(ctx.cpu.reg[instr.idx_rn]) << 32;
    ctx.cpu.reg[instr.idx_rd] = result & 0xFFFFFFFF;
    ctx.cpu.reg[instr.idx_rn] = result >> 32;

    if (instr.addr1_S) {
        UpdateCPSR_N(ctx, ctx.cpu.reg[instr.idx_rn]);
        UpdateCPSR_Z(ctx, ctx.cpu.reg[instr.idx_rd] | ctx.cpu.reg[instr.idx_rn]);
        // C and V are unpredictable on ARMv4 and earlier (otherwise, they are unaffected)
    }

    return NextInstr(ctx);
}

static uint32_t Handler00x0(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if ((instr.raw & 0b1111'1110'0000'0000'0000'1111'0000) == 0b0000'0010'0000'0000'0000'1001'0000) {
        return HandlerMla(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'0000'1111'0000) == 0b0000'0100'0000'0000'0000'1001'0000) {
        return HandlerUmaal(ctx, instr);
    } else if ((instr.raw & 0b1111'1110'0000'0000'0000'1111'0000) == 0b0000'1100'0000'0000'0000'1001'0000) {
        return HandlerSmull(ctx, instr);
    } else if ((instr.raw & 0b1111'1110'0000'0000'0000'1111'0000) == 0b0000'1110'0000'0000'0000'1001'0000) {
        return HandlerSmlal(ctx, instr);
    } else if ((instr.raw & 0b1111'1110'0000'0000'0000'1111'0000) == 0b0000'1000'0000'0000'0000'1001'0000) {
        return HandlerUmull(ctx, instr);
    } else if ((instr.raw & 0b1111'1110'0000'0000'0000'1111'0000) == 0b0000'1010'0000'0000'0000'1001'0000) {
        return HandlerUmlal(ctx, instr);
    } else {
        return HandlerStubAnnotated(ctx, instr, __LINE__);
    }
}

// Move *PSR to Register (CPSR if R=0, current SPSR otherwise)
static uint32_t HandlerMRS(CPUContext& ctx, ARM::ARMInstr instr) {
    // Unpredictable in all circumstances
    if (instr.idx_rd == ARM::Regs::PC)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // Unpredictable
    if (instr.R == 1 && !ctx.cpu.HasSPSR())
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    ctx.cpu.reg[instr.idx_rd] = instr.R ? ctx.cpu.GetSPSR(ctx.cpu.cpsr.mode).ToNativeRaw32() : ctx.cpu.cpsr.ToNativeRaw32();

    return NextInstr(ctx);
}

// Move to *PSR from Register
static uint32_t HandlerMSR(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Unpredictable
    if (instr.R == 1 && !ctx.cpu.HasSPSR())
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // TODO: Figure out if this may be used just like any other Addressing Mode 1 instruction! (e.g. applying different shift modes, etc)
    uint32_t operand = instr.msr_I ? RotateRight(instr.immed_8, 2 * instr.rotate_imm) : ctx.cpu.FetchReg(instr.idx_rm);

    // TODO: These are ARMv6-specific!
    const uint32_t UnallocMask = 0x06F0FC00;
    const uint32_t UserMask    = 0xF80F0200; // writeable from any mode. N, Z, C, V, Q, G[3:0], E.
    const uint32_t PrivMask    = 0x000001DF; // writeable from privileged modes. A, I, F, M[4:0]
    const uint32_t StateMask   = 0x01000020; // writeable from privileged modes. ignores writes from user mode.

    // Unpredictable
    if (operand & UnallocMask)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    auto& spr = instr.R ? ctx.cpu.GetSPSR(ctx.cpu.cpsr.mode) : ctx.cpu.cpsr;
    auto spr_raw = spr.ToNativeRaw32();

    uint32_t spr_mask = UserMask;
    if (instr.R == 0) {
        if (ctx.cpu.InPrivilegedMode()) {
            if (operand & StateMask)
                return HandlerStubAnnotated(ctx, instr, __LINE__);

            spr_mask |= PrivMask;
        }
    } else {
        spr_mask |= PrivMask | StateMask;
    }

    uint32_t mask = instr.ExpandMSRFieldMask() & spr_mask;
    auto spr_new = ARM::State::ProgramStatusRegister::FromNativeRaw32((spr_raw & ~mask) | (operand & mask));

    if (!instr.R) {
        ctx.cpu.ReplaceCPSR(spr_new);
    } else {
        spr.RawCopyFrom(spr_new);
    }

    return NextInstr(ctx);
}

static uint32_t HandlerCmp(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    uint32_t alu_out = ctx.cpu.reg[instr.idx_rn] - shifter_operand->value;

    UpdateCPSR_N(ctx, alu_out);
    UpdateCPSR_Z(ctx, alu_out);
    UpdateCPSR_C_FromBorrow(ctx, ctx.cpu.reg[instr.idx_rn], shifter_operand->value);
    UpdateCPSR_V_FromSub(ctx, ctx.cpu.reg[instr.idx_rn], shifter_operand->value, alu_out);

    return NextInstr(ctx);
}

static uint32_t HandlerCmn(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    auto shifter_operand = GetAddr1ShifterOperand(ctx, instr);
    if (!shifter_operand)
        return HandlerStubWithMessage(ctx, instr, "Unknown shifter operand format");

    uint32_t alu_out = ctx.cpu.reg[instr.idx_rn] + shifter_operand->value;

    UpdateCPSR_N(ctx, alu_out);
    UpdateCPSR_Z(ctx, alu_out);
    UpdateCPSR_C_FromCarry(ctx, ctx.cpu.reg[instr.idx_rn], shifter_operand->value);
    UpdateCPSR_V_FromAdd(ctx, ctx.cpu.reg[instr.idx_rn], shifter_operand->value, alu_out);

    return NextInstr(ctx);
}

// TODOTEST: What granularity does the 3DS use?
const uint32_t monitor_address_mask = 0xffffff8;

static void ClearExclusive(InterpreterExecutionContext& ctx) {
    ctx.monitor_address = {};
}

static void MarkExclusive(InterpreterExecutionContext& ctx, uint32_t new_address) {
    ctx.monitor_address = (new_address & monitor_address_mask);
}

// Returns true if the store can be performed
static bool PrepareExclusiveStore(InterpreterExecutionContext& ctx, uint32_t addr) {
    if (!ctx.monitor_address) {
        return false;
    }

    if (*ctx.monitor_address != (addr & monitor_address_mask)) {
        throw Mikage::Exceptions::Invalid("STREX(B/H/D) to non-exclusive address is implementation defined");
    }

    ctx.monitor_address = {};
    return true;
}

static uint32_t HandlerLdrex(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    auto addr = ctx.cpu.FetchReg(instr.idx_rn);
    MarkExclusive(ctx, addr);
    ctx.cpu.reg[instr.idx_rd] = ReadVirtualMemory<uint32_t>(ctx, addr);

    return NextInstr(ctx);
}

static uint32_t HandlerStrex(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    auto addr = ctx.cpu.FetchReg(instr.idx_rn);
    if (PrepareExclusiveStore(ctx, addr)) {
        WriteVirtualMemory<uint32_t>(ctx, addr, ctx.cpu.FetchReg(instr.idx_rm));
        ctx.cpu.reg[instr.idx_rd] = 0;
    } else {
        // Not in exclusive state => Not updating memory
        ctx.cpu.reg[instr.idx_rd] = 1;
    }

    return NextInstr(ctx);
}

static uint32_t HandlerLdrexb(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    auto addr = ctx.cpu.FetchReg(instr.idx_rn);
    MarkExclusive(ctx, addr);
    ctx.cpu.reg[instr.idx_rd] = ReadVirtualMemory<uint8_t>(ctx, addr);

    return NextInstr(ctx);
}

static uint32_t HandlerStrexb(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    auto addr = ctx.cpu.FetchReg(instr.idx_rn);
    if (PrepareExclusiveStore(ctx, addr)) {
        WriteVirtualMemory<uint8_t>(ctx, addr, ctx.cpu.FetchReg(instr.idx_rm));
        ctx.cpu.reg[instr.idx_rd] = 0;
    } else {
        // Not in exclusive state => Not updating memory
        ctx.cpu.reg[instr.idx_rd] = 1;
    }

    return NextInstr(ctx);
}

static uint32_t HandlerLdrexh(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    auto addr = ctx.cpu.FetchReg(instr.idx_rn);
    MarkExclusive(ctx, addr);
    ctx.cpu.reg[instr.idx_rd] = ReadVirtualMemory<uint16_t>(ctx, addr);

    return NextInstr(ctx);
}

static uint32_t HandlerStrexh(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    auto addr = ctx.cpu.FetchReg(instr.idx_rn);
    if (PrepareExclusiveStore(ctx, addr)) {
        WriteVirtualMemory<uint16_t>(ctx, addr, ctx.cpu.FetchReg(instr.idx_rm));
        ctx.cpu.reg[instr.idx_rd] = 0;
    } else {
        // Not in exclusive state => Not updating memory
        ctx.cpu.reg[instr.idx_rd] = 1;
    }

    return NextInstr(ctx);
}

static uint32_t HandlerLdrexd(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if ((instr.idx_rd % 2) != 0 || instr.idx_rd == ARM::Regs::LR || instr.idx_rn == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Unpredictable configuration");

    auto addr = ctx.cpu.FetchReg(instr.idx_rn);
    MarkExclusive(ctx, addr);
    ctx.cpu.reg[instr.idx_rd    ] = ReadVirtualMemory<uint32_t>(ctx, addr    );
    ctx.cpu.reg[instr.idx_rd + 1] = ReadVirtualMemory<uint32_t>(ctx, addr + 4);

    return NextInstr(ctx);
}

static uint32_t HandlerStrexd(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if ((instr.idx_rm % 2) != 0 || instr.idx_rm == ARM::Regs::LR || instr.idx_rn == ARM::Regs::PC || instr.idx_rd == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "Unpredictable configuration");

    if (instr.idx_rd == instr.idx_rn || instr.idx_rd == instr.idx_rm || instr.idx_rd == instr.idx_rm + 1)
        return HandlerStubWithMessage(ctx, instr, "Unpredictable configuration");

    auto addr = ctx.cpu.FetchReg(instr.idx_rn);
    if (PrepareExclusiveStore(ctx, addr)) {
        WriteVirtualMemory<uint32_t>(ctx, addr    , ctx.cpu.FetchReg(instr.idx_rm    ));
        WriteVirtualMemory<uint32_t>(ctx, addr + 4, ctx.cpu.FetchReg(instr.idx_rm + 1));
        ctx.cpu.reg[instr.idx_rd] = 0;
    } else {
        // Not in exclusive state => Not updating memory
        ctx.cpu.reg[instr.idx_rd] = 1;
    }

    return NextInstr(ctx);
}

// Count Leading Zeros
static uint32_t HandlerClz(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Unpredictable
    if (instr.idx_rm == ARM::Regs::PC || instr.idx_rd == ARM::Regs::PC)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    auto rm = ctx.cpu.FetchReg(instr.idx_rm);
    ctx.cpu.reg[instr.idx_rd] = 32;
    while (rm != 0) {
        rm >>= 1;
        --ctx.cpu.reg[instr.idx_rd];
    }

    return NextInstr(ctx);
}

static uint32_t Handler0001(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    // TODO: Find a cool way to handle these masks in a unified way and compile-time asserting that they are non-ambiguous

    if ((instr.raw & 0b1111'1111'1111'0000'1111'1111'0000) == 0b0001'0110'1111'0000'1111'0001'0000) {
        return HandlerClz(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'0000) == 0b0001'1000'0000'0000'1111'1001'0000) {
        return HandlerStrex(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'1111) == 0b0001'1001'0000'0000'1111'1001'1111) {
        return HandlerLdrex(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'0000) == 0b0001'1010'0000'0000'1111'1001'0000) {
        return HandlerStrexd(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'1111) == 0b0001'1011'0000'0000'1111'1001'1111) {
        return HandlerLdrexd(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'0000) == 0b0001'1100'0000'0000'1111'1001'0000) {
        return HandlerStrexb(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'1111) == 0b0001'1101'0000'0000'1111'1001'1111) {
        return HandlerLdrexb(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'0000) == 0b0001'1110'0000'0000'1111'1001'0000) {
        return HandlerStrexh(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'1111) == 0b0001'1111'0000'0000'1111'1001'1111) {
        return HandlerLdrexh(ctx, instr);
    } else if ((instr.raw & 0b1111'0100'0000'0000'1111'1001'0000) == 0b0001'0000'0000'0000'0000'1001'0000) {
        throw std::runtime_error("Should not be hit anymore with new dispatcher");
    } else if ((instr.raw & 0b1111'0100'0000'0000'0000'1001'0000) == 0b0001'0100'0000'0000'0000'1001'0000) {
        throw std::runtime_error("Should not be hit anymore with new dispatcher");
    } else if ((instr.identifier_4_23 & ~0b10) == 0b0010'1111'1111'1111'0001) {
        if (instr.identifier_4_23 & 0b10) {
            return HandlerBranchExchange<true>(ctx, instr);
        } else {
            return HandlerBranchExchange<false>(ctx, instr);
        }
    } else if ((instr.raw & 0b1111'1111'0001'1111'1110'0010'0000) == 0b0001'0000'0000'0000'0000'0000'0000) {
        return HandlerCPS(ctx, instr);
    } else if ((instr.raw & 0b1111'1011'1111'0000'1111'1111'1111) == 0b0001'0000'1111'0000'0000'0000'0000) {
        return HandlerMRS(ctx, instr);
    } else if ((instr.raw & 0b1111'1011'0000'1111'1111'1111'0000) == 0b0001'0010'0000'1111'0000'0000'0000) {
        return HandlerMSR(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'1111'0000'0000'0000) == 0b0001'0101'0000'0000'0000'0000'0000) {
        return HandlerCmp(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'1111'0000'0000'0000) == 0b0001'0111'0000'0000'0000'0000'0000) {
        return HandlerCmn(ctx, instr);
    } else if ((instr.raw & 0b1111'1110'1111'0000'0000'0000'0000) == 0b0001'1010'0000'0000'0000'0000'0000) {
        return HandlerMov(ctx, instr);
    } else if ((instr.raw & 0b1111'1110'1111'0000'0000'0000'0000) == 0b0001'1110'0000'0000'0000'0000'0000) {
        return HandlerMvn(ctx, instr);
    } else if ((instr.raw & 0b1111'1110'0000'0000'0000'0000'0000) == 0b0001'1000'0000'0000'0000'0000'0000) {
        return HandlerOrr(ctx, instr);
    } else if ((instr.raw & 0b1111'1110'0000'0000'0000'0000'0000) == 0b0001'1100'0000'0000'0000'0000'0000) {
        return HandlerBic(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'1111'0000'0000'0000) == 0b0001'0001'0000'0000'0000'0000'0000) {
        return HandlerTst(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'1111'0000'0000'0000) == 0b0001'0011'0000'0000'0000'0000'0000) {
        return HandlerTeq(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'1111'0000'0000'0000) == 0b0001'0010'0000'0000'0000'0000'0000) {
        // Technically UNPREDICTABLE due to missing S flag, but Luigi's Mansion 2 uses this, and my guess is it's not a NOP, so let's just do a plain TEQ...
        return HandlerTeq(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'0000'1001'0000) == 0b0001'0000'0000'0000'0000'1000'0000) {
        return HandlerSmlaxx(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'1111'0000'1001'0000) == 0b0001'0110'0000'0000'0000'1000'0000) {
        return HandlerSmulxx(ctx, instr);
    } else {
        return HandlerStub(ctx, instr);
    }
}

static uint32_t Handler0011(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if ((instr.raw & 0b1111'1110'1111'0000'0000'0000'0000) == 0b0011'1010'0000'0000'0000'0000'0000) {
        return HandlerMov(ctx, instr);
    } else if ((instr.raw & 0b1111'1110'1111'0000'0000'0000'0000) == 0b0011'1110'0000'0000'0000'0000'0000) {
        return HandlerMvn(ctx, instr);
    } else if ((instr.raw & 0b1111'1110'0000'0000'0000'0000'0000) == 0b0011'1000'0000'0000'0000'0000'0000) {
        return HandlerOrr(ctx, instr);
    } else if ((instr.raw & 0b1111'1110'0000'0000'0000'0000'0000) == 0b0011'1100'0000'0000'0000'0000'0000) {
        return HandlerBic(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'1111'0000'0000'0000) == 0b0011'0001'0000'0000'0000'0000'0000) {
        return HandlerTst(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'1111'0000'0000'0000) == 0b0011'0011'0000'0000'0000'0000'0000) {
        return HandlerTeq(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'1111'0000'0000'0000) == 0b0011'0010'0000'0000'0000'0000'0000) {
        // Technically UNPREDICTABLE due to missing S flag, but Luigi's Mansion 2 uses this, and my guess is it's not a NOP, so let's just do a plain TEQ...
        return HandlerTeq(ctx, instr);
    } else if ((instr.raw & 0b1111'1011'0000'1111'0000'0000'0000) == 0b0011'0010'0000'1111'0000'0000'0000) {
        return HandlerMSR(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'1111'0000'0000'0000) == 0b0011'0101'0000'0000'0000'0000'0000) {
        return HandlerCmp(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'1111'0000'0000'0000) == 0b0011'0111'0000'0000'0000'0000'0000) {
        return HandlerCmn(ctx, instr);
    } else {
        return HandlerStub(ctx, instr);
    }
}

template<bool byte_access, bool store>
static uint32_t HandlerMemoryAccess(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // P=0: Memory access using base register; after the access, the base register has the offset applied to it (post-indexed addressing)
    // P=0, W=0: normal memory access (LDR, LDRB, STR, STRB) using base register
    // P=0, W=1: unpriviliged memory access (LDRBT, LDRT, STRBT, STRT)
    // P=1, W=0: memory access using base register with applied offset (base register remains unchanged).
    // P=1, W=1: memory access using base register with applied offset (base register will be updated).

    // Not actually a memory access instruction in this case!
    if (instr.ldr_I && (instr.raw & 0x10) != 0)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    if (instr.idx_rn == instr.idx_rd && (!instr.ldr_P || instr.ldr_W)) {
        // Unknown instruction behavior for Rd == Rn: Which of the register writes has higher priority?
        return HandlerStubAnnotated(ctx, instr, __LINE__);
    }

    uint32_t base = ctx.cpu.FetchReg(instr.idx_rn);

    // lazy address offset - TODO: Catch exceptions! (or better not use exceptions here at all!)
    auto addr_offset = [&]{return CalcShifterOperandFromImmediate(ctx.cpu.FetchReg(instr.idx_rm), instr.ldr_shift_imm, instr.ldr_shift, ctx.cpu.cpsr.carry).value().value; };

    uint32_t offset = (instr.ldr_U ? 1 : -1)
                      * ((instr.ldr_I) ? addr_offset() : instr.ldr_offset.Value());

    if (instr.ldr_P)
        base += offset;

    if (store) {
        // Store memory
        if (byte_access) {
            WriteVirtualMemory<uint8_t>(ctx, base, ctx.cpu.FetchReg(instr.idx_rd));
        } else {
            WriteVirtualMemory<uint32_t>(ctx, base, ctx.cpu.FetchReg(instr.idx_rd));
        }

        // TODO: Magic for shared memory??
    } else {
        // Load memory
        uint32_t value = byte_access
                         ? ReadVirtualMemory<uint8_t>(ctx, base)
                         : ReadVirtualMemory<uint32_t>(ctx, base);

        // When loading to PC, clear bit0 to 0 and copy its old value to the thumb field
        if (instr.idx_rd != 15) {
            ctx.cpu.reg[instr.idx_rd] = value;
        } else {
            // Switch to Thumb mode if the LSB of the loaded value is set, but
            // if it isn't then make sure we are not branching to a non-word
            // aligned address (since that is UNPREDICTABLE in ARM mode).
            if ((value & 3) == 0b10)
                return HandlerStubAnnotated(ctx, instr, __LINE__);

            ctx.cpu.cpsr.thumb = value & 1;
            ctx.cpu.reg[instr.idx_rd] = value & 0xFFFFFFFE;
        }
    }

    if (!instr.ldr_P || instr.ldr_W) {
        if (!instr.ldr_P)
            base += offset;

        ctx.cpu.reg[instr.idx_rn] = base;

        if (instr.idx_rn == 15) {
            // TODO: Unknown behavior for PC
            return HandlerStubAnnotated(ctx, instr, __LINE__);
        }
    }

    if (!store && instr.idx_rd == 15) {
        return ctx.cpu.reg[15];
    } else {
        return NextInstr(ctx);
    }
}

template<bool Load>
static uint32_t HandlerLDM_STM(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Unpredictable
    if (instr.addr4_registers == 0)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // TODO: if L and S and User/System mode: Unpredictable
    if (Load && instr.addr4_S && !ctx.cpu.HasSPSR())
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // TODO: Should always start at the smallest address
    uint32_t addr = ctx.cpu.FetchReg(instr.idx_rn);

    uint32_t registers_accessed = [&]{
        uint32_t ret = 0;
        for (unsigned i = 0; i < 16; ++i)
            ret += ((instr.addr4_registers >> i) & 1);
        return ret;
    }();

    // NOTE: Registers are always accessed starting from the lowest address, regardless of whether we are increasing or decreasing.
    uint32_t addr2 = addr - ((instr.addr4_U ? 0 : 1) * 4 * (registers_accessed - 1)) + ((instr.addr4_U ? 4 : -4) * instr.addr4_P);

    uint32_t next_pc = NextInstr(ctx);

    for (unsigned i = 0; i < 16; ++i) {
        if (((instr.addr4_registers >> i) & 1) == 0)
            continue;

        if (instr.addr4_P)
            addr += 4 * (instr.addr4_U ? 1 : -1);

        if (i == ARM::Regs::PC) {
            if (Load) {
                auto val = ReadVirtualMemory<uint32_t>(ctx, addr2);
                next_pc = val & ~1;

                ctx.cpu.cpsr.thumb = val & 1;

                // Move SPSR to CPSR
                if (instr.addr4_S) {
                    ctx.cpu.ReplaceCPSR(ctx.cpu.GetSPSR(ctx.cpu.cpsr.mode));
                }

                ctx.cfl.Return(ctx, "pop");
            } else {
                // NOTE: The stored value is ImplementationDefined!
                WriteVirtualMemory<uint32_t>(ctx, addr2, ctx.cpu.FetchReg(i));
            }
        } else {
            if (Load) {
                ctx.cpu.reg[i] = ReadVirtualMemory<uint32_t>(ctx, addr2);
            } else {
                // if !Load and in privileged mode, use user mode banked registers instead
                if (i >= 8 && !Load && ctx.cpu.InPrivilegedMode()) {
                    WriteVirtualMemory<uint32_t>(ctx, addr2, ctx.cpu.banked_regs_user[i-8]);
                } else {
                    WriteVirtualMemory<uint32_t>(ctx, addr2, ctx.cpu.FetchReg(i));
                }
            }
        }

        // TODO: Compute the final addr value statically rather than updating it each time in this loop!
        if (!instr.addr4_P)
            addr += 4 * (instr.addr4_U ? 1 : -1);

        addr2 += 4;
    }

    if (instr.addr4_W)
        ctx.cpu.reg[instr.idx_rn] = addr;

    return next_pc;
}

// unsigned 8 bit additions
static uint32_t HandlerUadd8(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Unpredictable configuration
    if (instr.idx_rn == ARM::Regs::PC ||
        instr.idx_rm == ARM::Regs::PC ||
        instr.idx_rd == ARM::Regs::PC)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    auto val_rn = ctx.cpu.FetchReg(instr.idx_rn);
    auto val_rm = ctx.cpu.FetchReg(instr.idx_rm);

    uint32_t result = 0;
    result |= static_cast<uint8_t>((val_rn & 0xFF) + (val_rm & 0xFF));
    result |= static_cast<uint8_t>((val_rn & 0xFF00) + (val_rm & 0xFF00)) << 8;
    result |= static_cast<uint8_t>((val_rn & 0xFF0000) + (val_rm & 0xFF0000)) << 16;
    result |= static_cast<uint8_t>((val_rn & 0xFF000000) + (val_rm & 0xFF000000)) << 24;

    ctx.cpu.reg[instr.idx_rd] = result;

    ctx.cpu.cpsr.ge0 = GetCarryT<uint8_t>(val_rn, val_rm);
    ctx.cpu.cpsr.ge1 = GetCarryT<uint8_t>(val_rn >> 8, val_rm >> 8);
    ctx.cpu.cpsr.ge2 = GetCarryT<uint8_t>(val_rn >> 16, val_rm >> 16);
    ctx.cpu.cpsr.ge3 = GetCarryT<uint8_t>(val_rn >> 24, val_rm >> 24);

    return NextInstr(ctx);
}

// saturated 8 bit additions
static uint32_t HandlerUqadd8(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Unpredictable configuration
    if (instr.idx_rn == ARM::Regs::PC ||
        instr.idx_rm == ARM::Regs::PC ||
        instr.idx_rd == ARM::Regs::PC)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    auto val_rn = ctx.cpu.FetchReg(instr.idx_rn);
    auto val_rm = ctx.cpu.FetchReg(instr.idx_rm);

    uint32_t result = 0;
    result |= std::min(uint32_t { 255 }, (val_rn + val_rm) & 0xFF);
    result |= std::min(uint32_t { 255 }, ((val_rn >> 8u) + (val_rm >> 8u)) & 0xFF) << 8;
    result |= std::min(uint32_t { 255 }, ((val_rn >> 16u) + (val_rm >> 16u)) & 0xFF) << 16;
    result |= std::min(uint32_t { 255 }, ((val_rn >> 24u) + (val_rm >> 24u)) & 0xFF) << 24;
    ctx.cpu.reg[instr.idx_rd] = result;

    return NextInstr(ctx);
}

// unsigned 8 bit additions (halfed)
static uint32_t HandlerUhadd8(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Unpredictable configuration
    if (instr.idx_rn == ARM::Regs::PC ||
        instr.idx_rm == ARM::Regs::PC ||
        instr.idx_rd == ARM::Regs::PC)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    auto val_rn = ctx.cpu.FetchReg(instr.idx_rn);
    auto val_rm = ctx.cpu.FetchReg(instr.idx_rm);

    uint32_t result = 0;
    result |= static_cast<uint8_t>(((val_rn & 0xFF) + (val_rm & 0xFF)) / 2);
    result |= static_cast<uint8_t>(((val_rn & 0xFF00) + (val_rm & 0xFF00)) / 2) << 8;
    result |= static_cast<uint8_t>(((val_rn & 0xFF0000) + (val_rm & 0xFF0000)) / 2) << 16;
    result |= static_cast<uint8_t>(((val_rn & 0xFF000000) + (val_rm & 0xFF000000)) / 2) << 24;

    ctx.cpu.reg[instr.idx_rd] = result;

    return NextInstr(ctx);
}

// 8 bit subtractions
template<bool Signed>
static uint32_t HandlerXsub8(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Unpredictable configuration
    if (instr.idx_rn == ARM::Regs::PC ||
        instr.idx_rm == ARM::Regs::PC ||
        instr.idx_rd == ARM::Regs::PC)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    auto val_rn = ctx.cpu.FetchReg(instr.idx_rn);
    auto val_rm = ctx.cpu.FetchReg(instr.idx_rm);

    uint32_t result = 0;
    result |= static_cast<uint8_t>((val_rn & 0xFF) - (val_rm & 0xFF));
    result |= static_cast<uint8_t>((val_rn & 0xFF00) - (val_rm & 0xFF00)) << 8;
    result |= static_cast<uint8_t>((val_rn & 0xFF0000) - (val_rm & 0xFF0000)) << 16;
    result |= static_cast<uint8_t>((val_rn & 0xFF000000) - (val_rm & 0xFF000000)) << 24;

    ctx.cpu.reg[instr.idx_rd] = result;

    using ComparisonType = std::conditional_t<Signed, int8_t, uint8_t>;
    ctx.cpu.cpsr.ge0 = static_cast<ComparisonType>(val_rn >> 0) >= static_cast<ComparisonType>(val_rm >> 0);
    ctx.cpu.cpsr.ge0 = static_cast<ComparisonType>(val_rn >> 8) >= static_cast<ComparisonType>(val_rm >> 8);
    ctx.cpu.cpsr.ge0 = static_cast<ComparisonType>(val_rn >> 16) >= static_cast<ComparisonType>(val_rm >> 16);
    ctx.cpu.cpsr.ge0 = static_cast<ComparisonType>(val_rn >> 24) >= static_cast<ComparisonType>(val_rm >> 24);

    return NextInstr(ctx);
}

// saturated signed 8 bit subtractions (-128...127)
static uint32_t HandlerQsub8(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Unpredictable configuration
    if (instr.idx_rn == ARM::Regs::PC ||
        instr.idx_rm == ARM::Regs::PC ||
        instr.idx_rd == ARM::Regs::PC)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    auto val_rn = ctx.cpu.FetchReg(instr.idx_rn);
    auto val_rm = ctx.cpu.FetchReg(instr.idx_rm);

    uint32_t result = 0;
    if ((val_rn & 0xFF) + 0x80 >= (val_rm & 0xFF))
        result |= ((val_rn & 0xFF) - (val_rm & 0xFF)) & 0xFF;
    if (((val_rn >> 8) & 0xFF) + 0x80 >= ((val_rm >> 8) & 0xFF))
        result |= ((val_rn & 0xFF00) - (val_rm & 0xFF00)) & 0xFF00;
    if (((val_rn >> 16) & 0xFF) + 0x80 >= ((val_rm >> 16) & 0xFF))
        result |= ((val_rn & 0xFF0000) - (val_rm& 0xFF0000)) & 0xFF0000;
    if ((val_rn >> 24) + 0x80 >= (val_rm >> 24))
        result |= ((val_rn & 0xFF000000) - (val_rm& 0xFF000000)) & 0xFF000000;

    ctx.cpu.reg[instr.idx_rd] = result;

    return NextInstr(ctx);
}

static uint32_t HandlerUqsub8(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Unpredictable configuration
    if (instr.idx_rn == ARM::Regs::PC ||
        instr.idx_rm == ARM::Regs::PC ||
        instr.idx_rd == ARM::Regs::PC)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    auto val_rn = ctx.cpu.FetchReg(instr.idx_rn);
    auto val_rm = ctx.cpu.FetchReg(instr.idx_rm);

    uint32_t result = 0;
    if ((val_rn & 0xFF) >= (val_rm & 0xFF))
        result |= ((val_rn & 0xFF) - (val_rm & 0xFF));
    if ((val_rn & 0xFF00) >= (val_rm & 0xFF00))
        result |= ((val_rn & 0xFF00) - (val_rm & 0xFF00));
    if ((val_rn & 0xFF0000) >= (val_rm & 0xFF0000))
        result |= ((val_rn & 0xFF0000) - (val_rm & 0xFF0000));
    if ((val_rn & 0xFF000000) >= (val_rm & 0xFF000000))
        result |= ((val_rn & 0xFF000000) - (val_rm & 0xFF000000));

    ctx.cpu.reg[instr.idx_rd] = result;

    return NextInstr(ctx);
}

static uint32_t HandlerSel(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Unpredictable configuration
    if (instr.idx_rn == ARM::Regs::PC ||
        instr.idx_rm == ARM::Regs::PC ||
        instr.idx_rd == ARM::Regs::PC)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    uint32_t result = 0;
    result |=       0xFF & (ctx.cpu.cpsr.ge0 ? ctx.cpu.reg[instr.idx_rn] : ctx.cpu.reg[instr.idx_rm]);
    result |=     0xFF00 & (ctx.cpu.cpsr.ge1 ? ctx.cpu.reg[instr.idx_rn] : ctx.cpu.reg[instr.idx_rm]);
    result |=   0xFF0000 & (ctx.cpu.cpsr.ge2 ? ctx.cpu.reg[instr.idx_rn] : ctx.cpu.reg[instr.idx_rm]);
    result |= 0xFF000000 & (ctx.cpu.cpsr.ge3 ? ctx.cpu.reg[instr.idx_rn] : ctx.cpu.reg[instr.idx_rm]);
    ctx.cpu.reg[instr.idx_rd] = result;

    return NextInstr(ctx);
}

// Unsigned Saturate
static uint32_t HandlerUsat(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == 15 || instr.idx_rm == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    auto operand = CalcShifterOperand(ctx.cpu.FetchReg(instr.idx_rm), instr.addr1_shift_imm, instr.addr1_shift, 0)->value;


    uint32_t max_value = (UINT32_C(1) << instr.sat_imm) - 1;

    ctx.cpu.reg[instr.idx_rd] = boost::algorithm::clamp<int32_t>(operand, 0, max_value);

    if (ctx.cpu.reg[instr.idx_rd] != operand)
        ctx.cpu.cpsr.q = 1;

    return NextInstr(ctx);
}

// Signed Saturate
static uint32_t HandlerSsat(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == 15 || instr.idx_rm == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    auto operand = CalcShifterOperand(ctx.cpu.FetchReg(instr.idx_rm), instr.addr1_shift_imm, instr.addr1_shift, 0)->value;


    int32_t min_value = -(int32_t { 1 } << instr.sat_imm);
    int32_t max_value = (int32_t { 1 } << instr.sat_imm) - 1;

    ctx.cpu.reg[instr.idx_rd] = boost::algorithm::clamp<int32_t>(operand, min_value, max_value);

    if (ctx.cpu.reg[instr.idx_rd] != operand)
        ctx.cpu.cpsr.q = 1;

    return NextInstr(ctx);
}

// Extract two bytes and repack them as two zero-extended half-words
static uint32_t HandlerUxtb16(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == 15 || instr.idx_rm == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    ctx.cpu.reg[instr.idx_rd] = RotateRight(ctx.cpu.FetchReg(instr.idx_rm), 8 * instr.uxtb_rotate) & 0xFF00FF;

    return NextInstr(ctx);
}

// Extract a byte value from a register
static uint32_t HandlerUxtb(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == 15 || instr.idx_rm == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    ctx.cpu.reg[instr.idx_rd] = RotateRight(ctx.cpu.FetchReg(instr.idx_rm), 8 * instr.uxtb_rotate) & 0xFF;

    return NextInstr(ctx);
}

static uint32_t HandlerUxtab(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == ARM::Regs::PC || instr.idx_rm == ARM::Regs::PC)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    if (instr.idx_rn == ARM::Regs::PC)
        return HandlerStubWithMessage(ctx, instr, "UXTB incorrectly recognized as UXTAB!");

    uint32_t operand = RotateRight(ctx.cpu.FetchReg(instr.idx_rm), 8 * instr.uxtb_rotate) & 0xFF;
    ctx.cpu.reg[instr.idx_rd] = ctx.cpu.reg[instr.idx_rn] + operand;

    return NextInstr(ctx);
}

// Signed eXTract Byte
static uint32_t HandlerSxtb(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == 15 || instr.idx_rm == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // Compute result of rotation and then cast to int8_t to get proper sign extension
    ctx.cpu.reg[instr.idx_rd] = static_cast<int8_t>(RotateRight(ctx.cpu.FetchReg(instr.idx_rm), 8 * instr.uxtb_rotate) & 0xFF);

    return NextInstr(ctx);
}

static uint32_t HandlerUxtah(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == 15 || instr.idx_rm == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    uint32_t offset = ctx.cpu.reg[instr.idx_rn];
    if (instr.idx_rn == 15) {
        // This case is actually an UXTH instruction, so drop the offset
        offset = 0;
    }

    ctx.cpu.reg[instr.idx_rd] = offset + (RotateRight(ctx.cpu.FetchReg(instr.idx_rm), 8 * instr.uxtb_rotate) & 0xFFFF);

    return NextInstr(ctx);
}

static uint32_t HandlerSxtab(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == 15 || instr.idx_rm == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // NOTE: This case is actually an SXTB instruction
    if (instr.idx_rn == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // Rotate and sign extend from 8 to 32 bits, and add the result to Rn
    ctx.cpu.reg[instr.idx_rd] = ctx.cpu.reg[instr.idx_rn] + (int8_t)(uint8_t)(RotateRight(ctx.cpu.FetchReg(instr.idx_rm), 8 * instr.uxtb_rotate));

    return NextInstr(ctx);
}

static uint32_t HandlerSxtah(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == 15 || instr.idx_rm == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    uint32_t offset = ctx.cpu.reg[instr.idx_rn];
    if (instr.idx_rn == 15) {
        // This case is actually an SXTH instruction, so drop the offset
        offset = 0;
    }

    // Rotate and sign extend from 16 to 32 bits, and add the result to Rn
    ctx.cpu.reg[instr.idx_rd] = offset + (int16_t)(uint16_t)(RotateRight(ctx.cpu.FetchReg(instr.idx_rm), 8 * instr.uxtb_rotate));

    return NextInstr(ctx);
}

// PacK Halfword Bottom Top
static uint32_t HandlerPkhbt(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == 15 || instr.idx_rm == 15 || instr.idx_rn == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    ctx.cpu.reg[instr.idx_rd] = ctx.cpu.reg[instr.idx_rn] & 0xFFFF;
    ctx.cpu.reg[instr.idx_rd] |= (ctx.cpu.reg[instr.idx_rm] << instr.addr1_shift_imm) & 0xFFFF0000;

    return NextInstr(ctx);
}

// PacK Halfword Top Bottom
static uint32_t HandlerPkhtb(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == 15 || instr.idx_rm == 15 || instr.idx_rn == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    ctx.cpu.reg[instr.idx_rd] = 0;
    // shift_imm=0 encodes a 32-bit shift, hence only the sign of rm is relevant
    if (instr.addr1_shift_imm == 0) {
        ctx.cpu.reg[instr.idx_rd] |= (reinterpret_cast<int32_t&>(ctx.cpu.reg[instr.idx_rm]) >> 31) & 0xFFFF;
    } else {
        ctx.cpu.reg[instr.idx_rd] |= ArithmeticShiftRight(ctx.cpu.reg[instr.idx_rm], instr.addr1_shift_imm);
    }
    ctx.cpu.reg[instr.idx_rd] |= ctx.cpu.reg[instr.idx_rn] & 0xFFFF0000;

    return NextInstr(ctx);
}

// Byte-Reverse Word
static uint32_t HandlerRev(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == 15 || instr.idx_rm == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    const uint32_t bytes[] = {
         ctx.cpu.reg[instr.idx_rm]        & 0xFF,
        (ctx.cpu.reg[instr.idx_rm] >>  8) & 0xFF,
        (ctx.cpu.reg[instr.idx_rm] >> 16) & 0xFF,
        (ctx.cpu.reg[instr.idx_rm] >> 24) & 0xFF
    };
    ctx.cpu.reg[instr.idx_rd] = bytes[3];
    ctx.cpu.reg[instr.idx_rd] |= bytes[2] << 8;
    ctx.cpu.reg[instr.idx_rd] |= bytes[1] << 16;
    ctx.cpu.reg[instr.idx_rd] |= bytes[0] << 24;

    return NextInstr(ctx);
}

// Byte-Reverse Packed Halfword
static uint32_t HandlerRev16(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Using PC is Unpredictable
    if (instr.idx_rd == 15 || instr.idx_rm == 15)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    const uint32_t bytes[] = {
         ctx.cpu.reg[instr.idx_rm]        & 0xFF,
        (ctx.cpu.reg[instr.idx_rm] >>  8) & 0xFF,
        (ctx.cpu.reg[instr.idx_rm] >> 16) & 0xFF,
        (ctx.cpu.reg[instr.idx_rm] >> 24) & 0xFF
    };
    ctx.cpu.reg[instr.idx_rd] = (bytes[1] | (bytes[0] << 8));
    ctx.cpu.reg[instr.idx_rd] |= (bytes[3] | (bytes[2] << 8)) << 16;

    return NextInstr(ctx);
}

static uint32_t Handler01xx(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (instr.cond == 0xF) {
        // TODO: Isn't this handled before the table lookup now?
        if ((instr.raw & 0b1101'0111'0000'1111'0000'0000'0000) == 0b0101'0101'0000'1111'0000'0000'0000) {
            // PLD - Preload Data
            // This is just a hint about memory access, hence we don't need to emulate it.
            return NextInstr(ctx);
        }

        // Otherwise, this is an unknown instruction
        return HandlerStubAnnotated(ctx, instr, __LINE__);
    }

    if ((instr.raw & 0b1111'1110'0000'0000'0000'0011'0000) == 0b0110'1110'0000'0000'0000'0001'0000) {
        return HandlerUsat(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'1111'0000'0011'1111'0000) == 0b0110'1100'1111'0000'0000'0111'0000) {
        throw std::runtime_error("Should not be hit anymore with new dispatcher");
    } else if ((instr.raw & 0b1111'1111'1111'0000'0011'1111'0000) == 0b0110'1110'1111'0000'0000'0111'0000) {
        return HandlerUxtb(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'0011'1111'0000) == 0b0110'1110'0000'0000'0000'0111'0000) {
        return HandlerUxtab(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'1111'0000'0011'1111'0000) == 0b0110'1111'1111'0000'0000'0111'0000) {
        throw std::runtime_error("Should not be hit anymore with new dispatcher");
    } else if ((instr.raw & 0b1111'1111'1111'0000'0011'1111'0000) == 0b0110'1010'1111'0000'0000'0111'0000) {
        return HandlerSxtb(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'1111'0000'0011'1111'0000) == 0b0110'1011'1111'0000'0000'0111'0000) {
        throw std::runtime_error("Should not be hit anymore with new dispatcher");
    } else if ((instr.raw & 0b1111'1111'0000'0000'0011'1111'0000) == 0b0110'1010'0000'0000'0000'0111'0000) {
        return HandlerSxtab(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'0011'1111'0000) == 0b0110'1011'0000'0000'0000'0111'0000) {
        throw std::runtime_error("Should not be hit anymore with new dispatcher");
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'0000) == 0b0110'0101'0000'0000'1111'1001'0000) {
        throw std::runtime_error("Should not be hit anymore with new dispatcher");
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'0000) == 0b0110'0110'0000'0000'1111'1001'0000) {
        throw std::runtime_error("Should not be hit anymore with new dispatcher");
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'0000) == 0b0110'0111'0000'0000'1111'1001'0000) {
        throw std::runtime_error("Should not be hit anymore with new dispatcher");
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'0000) == 0b0110'0001'0000'0000'1111'1111'0000) {
        throw std::runtime_error("Should not be hit anymore with new dispatcher");
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'0000) == 0b0110'0101'0000'0000'1111'1111'0000) {
        throw std::runtime_error("Should not be hit anymore with new dispatcher");
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'0000) == 0b0110'0110'0000'0000'1111'1111'0000) {
        throw std::runtime_error("Should not be hit anymore with new dispatcher");
    } else if ((instr.raw & 0b1111'1111'0000'0000'1111'1111'0000) == 0b0110'1000'0000'0000'1111'1011'0000) {
        return HandlerSel(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'0000'0111'0000) == 0b0110'1000'0000'0000'0000'0001'0000) {
        return HandlerPkhbt(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'0000'0000'0000'0111'0000) == 0b0110'1000'0000'0000'0000'0101'0000) {
        return HandlerPkhtb(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'1111'0000'1111'1111'0000) == 0b0110'1011'1111'0000'1111'0011'0000) {
        return HandlerRev(ctx, instr);
    } else if ((instr.raw & 0b1111'1111'1111'0000'1111'1111'0000) == 0b0110'1011'1111'0000'1111'1011'0000) {
        return HandlerRev16(ctx, instr);
    } else {
        return HandlerStubAnnotated(ctx, instr, __LINE__);
    }
}

static void LoadOrStoreFloat(InterpreterExecutionContext& ctx, uint32_t address, float& reg_value, bool load) {
    if (load) {
        auto value = ReadVirtualMemory<uint32_t>(ctx, address);
        memcpy(&reg_value, &value, sizeof(value));
    } else {
        uint32_t value = 0;
        memcpy(&value, &reg_value, sizeof(value));
        WriteVirtualMemory<uint32_t>(ctx, address, value);
    }
}

static uint32_t LoadStoreFloatSingle(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond)) {
        return NextInstr(ctx);
    }

    const bool is_double = ViewBitField<8, 1, uint32_t>(instr.raw);

    // FLDD/FLDS/FSTD/FSTS: Single register, no writeback

    unsigned idx_fd = (instr.idx_rd << 1) | instr.addr5_D;

    uint32_t address = ctx.cpu.FetchReg(instr.idx_rn);
    if (instr.ldr_U)
        address += 4 * instr.addr5_offset;
    else
        address -= 4 * instr.addr5_offset;

    // TODO: May need fixing when big-endian mode support is added
    for (unsigned reg = 0; reg < (is_double ? 2 : 1); ++reg)
        LoadOrStoreFloat(ctx, address + 4 * reg, ctx.cpu.fpreg[idx_fd + reg], instr.addr4_L);

    return NextInstr(ctx);
}

static uint32_t LoadStoreFloatMultiple(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond)) {
        return NextInstr(ctx);
    }

    const bool is_double = ViewBitField<8, 1, uint32_t>(instr.raw);

    // TODO: Should always start at the smallest address
    uint32_t start_addr = ctx.cpu.FetchReg(instr.idx_rn);
    if (instr.ldr_P && !instr.ldr_U && instr.ldr_W)
        start_addr -= 4 * instr.addr5_offset;
    uint32_t word_count = instr.addr5_offset - (is_double && (instr.addr5_offset & 1));

    uint32_t updated_rn = ctx.cpu.FetchReg(instr.idx_rn);
    if(!instr.ldr_P && instr.ldr_U && instr.ldr_W)
        updated_rn += 4 * instr.addr5_offset;
    if(instr.ldr_P && !instr.ldr_U && instr.ldr_W)
        updated_rn -= 4 * instr.addr5_offset;

    // FLDMD: offset&1 must be 0 (?); offset must be != 0; d + offset/2 must be <=32
    // FLDMD: offset&1 must be 0 (?); offset must be != 0; d + offset must be <=32

    unsigned idx_fd = (instr.idx_rd << 1) | instr.addr5_D;
    for (unsigned reg = 0; reg < word_count; ++reg)
        LoadOrStoreFloat(ctx, start_addr + 4 * reg, ctx.cpu.fpreg[idx_fd + reg], instr.addr4_L);

    // TODO: Assert that rn is not contained in the registers list
    if (instr.addr4_W) {
        ctx.cpu.reg[instr.idx_rn] = updated_rn;

        // Behavior unknown for PC
        if (instr.idx_rn == ARM::Regs::PC)
            return HandlerStubAnnotated(ctx, instr, __LINE__);
    }

    return NextInstr(ctx);
}

static uint32_t Handler1101(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    // Handled via interpreter_dispatch_table instead
    return HandlerStubAnnotated(ctx, instr, __LINE__);
}

static uint32_t HandlerFMDRR(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Unknown behavior when PC is used
    if (instr.idx_rn == ARM::Regs::PC || instr.idx_rd == ARM::Regs::PC)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    // TODO: Single precision is not implemented, assert for that!

    memcpy(&ctx.cpu.dpreg_raw[instr.idx_rm].raw_high, &ctx.cpu.reg[instr.idx_rn], sizeof(uint32_t));
    memcpy(&ctx.cpu.dpreg_raw[instr.idx_rm].raw_low, &ctx.cpu.reg[instr.idx_rd], sizeof(uint32_t));
    return NextInstr(ctx);
}

static uint32_t HandlerFMRDD(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Move to two Registers from Double Precision

    // TODO: Single precision is not implemented, assert for that!

    if (instr.idx_rn == ARM::Regs::PC || instr.idx_rd == ARM::Regs::PC)
        return HandlerStubAnnotated(ctx, instr, __LINE__);

    memcpy(&ctx.cpu.reg[instr.idx_rn], &ctx.cpu.dpreg_raw[instr.idx_rm].raw_high, sizeof(uint32_t));
    memcpy(&ctx.cpu.reg[instr.idx_rd], &ctx.cpu.dpreg_raw[instr.idx_rm].raw_low, sizeof(uint32_t));
    return NextInstr(ctx);
}

static uint32_t Handler1100(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    // Handled via interpreter_dispatch_table instead
    return HandlerStubAnnotated(ctx, instr, __LINE__);
}

// Move to Register from Coprocessor
static uint32_t HandlerMRC(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (instr.coproc_id != 15)
        return HandlerStubWithMessage(ctx, instr, "Only cp15 supported currently");

    // TODO: At least furthermore recognize 5 (TLB) and 7 (Debug)
    if (instr.coproc_opcode1 != 0)
        return HandlerStubWithMessage(ctx, instr, "Only cp15 opcode1=0 supported currently");

    uint32_t data = 0;

    switch ((instr.idx_rn << 8) | (instr.idx_rm << 4) | instr.coproc_opcode2) {
    // CPU ID register
    case 0x005:
        data = ctx.cpu.cp15.CPUId().raw;
        break;

    // Control Register
    case 0x100:
        data = ctx.cpu.cp15.Control().raw;
        break;

    // Auxiliary Control Register
    case 0x101:
        data = ctx.cpu.cp15.AuxiliaryControl().raw;
        break;

    case 0xd03:
        data = ctx.cpu.cp15.ThreadLocalStorage().virtual_addr;
        break;

    default:
    {
        std::stringstream ss;
        ss << "Unknown CRn/CRm/opcode2 combination: " << std::hex << instr.idx_rn << ", " << instr.idx_rm << ", " << instr.coproc_opcode2;
        return HandlerStubWithMessage(ctx, instr, ss.str());
    }
    }

    if (instr.idx_rd == ARM::Regs::PC) {
        // TODO:
        // N = data[31]
        // Z = data[30]
        // C = data[29]
        // V = data[28]
        return HandlerStubWithMessage(ctx, instr, "Rd==PC not supported");
    } else {
        ctx.cpu.reg[instr.idx_rd] = data;
    }

    return NextInstr(ctx);
}

// Move to Coprocessor from Register
static uint32_t HandlerMCR(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // Implemented according to Citra's CP15 code for now... TODO: Test all of this!

    if (instr.coproc_id != 15)
        return HandlerStubWithMessage(ctx, instr, "Only cp15 supported currently");

    // TODO: At least furthermore recognize 5 (TLB) and 7 (Debug)
    if (instr.coproc_opcode1 != 0)
        return HandlerStubWithMessage(ctx, instr, "Only cp15 opcode1=0 supported currently");

    if (instr.idx_rn == 0)
        return HandlerStubWithMessage(ctx, instr, "Rn=R0 is expected to be read-only");

    if (instr.idx_rd == 15)
        return HandlerStubWithMessage(ctx, instr, "Rd=PC not supported currently");

    switch ((instr.idx_rn << 8) | (instr.idx_rm << 4) | instr.coproc_opcode2) {
    // Control Register
    case 0x100:
        ctx.cpu.cp15.Control().raw = ctx.cpu.reg[instr.idx_rd];
        break;

    // Auxiliary Control Register
    case 0x101:
        ctx.cpu.cp15.AuxiliaryControl().raw = ctx.cpu.reg[instr.idx_rd];
        break;

    // Invalidate Entire Instruction Cache Register
    case 0x750:
        return HandlerSkip(ctx, instr, "No instruction cache emulation");

    // Flush Prefetch Buffer Register
    case 0x754:
        return HandlerSkip(ctx, instr, "No prefetch buffer emulation");

    // Invalidate Entire Data Cache Register
    case 0x760:
        return HandlerSkip(ctx, instr, "No data cache emulation");

    // Data Synchronization Barrier Register
    case 0x7a4:
        return HandlerSkip(ctx, instr, "No data synchronization emulation");

    // Data Memory Barrier Register
    case 0x7a5:
        return HandlerSkip(ctx, instr, "No data memory barrier emulation");

    default:
    {
        std::stringstream ss;
        ss << "Unknown CRn/CRm/opcode2 combination: " << std::hex << instr.idx_rn << ", " << instr.idx_rm << ", " << instr.coproc_opcode2;
        return HandlerStubWithMessage(ctx, instr, ss.str());
    }
    }

    return NextInstr(ctx);
}

// large integral numbers may not be representable accurately by 32-bit
// floating point numbers. This function provides a safe way to clamp a
// floating point number to the given integer range.
template<typename FloatType, typename IntType>
static IntType ClampToIntegerRange(FloatType value, IntType min, IntType max) {
    static_assert(std::is_floating_point<FloatType>::value, "");
    static_assert(std::is_integral<IntType>::value, "");

    // Get largest floating point number within the given range
    FloatType min_float = std::nextafter(min, IntType(0));
    // TODO: This returns the wrong value! for 0xffffff80, converting to float will yield 0x100000000...
    FloatType max_float = std::nextafter(max, IntType(0));
    // TODO: Instead, should use nexttowardf(float{0x10000000}, 0.f)
//    auto max_float = std::nexttoward(FloatType { 0x100000000 }, static_cast<long double>(0)); //
//    static_assert(std::is_same_v<FloatType, decltype(max_float)>); // Make sure we got the right overload for nexttoward

    if (value < min_float)
        return min;

    if (value > max_float)
        return max;

    return static_cast<IntType>(value);
}

template<bool IsDouble>
static auto& GetVFPRegisters(CPUContext& ctx);

template<>
auto& GetVFPRegisters<false>(CPUContext& ctx) {
    return ctx.cpu.fpreg;
}
template<>
auto& GetVFPRegisters<true>(CPUContext& ctx) {
    return ctx.cpu.dpreg;
}

template<bool IsDouble>
static uint32_t HandlerVFPDataProcessing(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    if (IsDouble) {
        // UNDEFINED: TODO: N != 1 && M != 1 only applies to some instructions.
        if (instr.addr5_D/* || instr.vfp_data_N || instr.vfp_data_M*/) {
//            return HandlerStubAnnotated(ctx, instr, __LINE__);
        }
    }

    auto idx_single_d = (instr.idx_rd << 1) | instr.addr5_D;
    auto idx_single_n = (instr.idx_rn << 1) | instr.vfp_data_N;
    auto idx_single_m = (instr.idx_rm << 1) | instr.vfp_data_M;
    auto idx_double_d = instr.idx_rd;
    auto idx_double_n = instr.idx_rn;
    auto idx_double_m = instr.idx_rm;
    auto idx_fd = IsDouble ? idx_double_d.Value() : idx_single_d;
    auto idx_fn = IsDouble ? idx_double_n.Value() : idx_single_n;
    auto idx_fm = IsDouble ? idx_double_m.Value() : idx_single_m;
    auto& regs = GetVFPRegisters<IsDouble>(ctx);
    using RegType = std::remove_reference_t<decltype(regs[idx_fd])>;
    static_assert(std::is_same<RegType, float>::value || std::is_same<RegType, double>::value, "");

    if (!instr.vfp_data_opcode_p && !instr.vfp_data_opcode_q) {
        // FMAC/FNMAC/FMSC/FNMSC - multiply-accumulate-like
        if (!instr.vfp_data_opcode_r && !instr.vfp_data_opcode_s) {
            regs[idx_fd] = std::fma(regs[idx_fn], regs[idx_fm], regs[idx_fd]);
        } else if (!instr.vfp_data_opcode_r && instr.vfp_data_opcode_s) {
            regs[idx_fd] = std::fma(regs[idx_fn], -regs[idx_fm], regs[idx_fd]);
        } else if (instr.vfp_data_opcode_r && !instr.vfp_data_opcode_s) {
            regs[idx_fd] = std::fma(regs[idx_fn], regs[idx_fm], -regs[idx_fd]);
        } else {
            regs[idx_fd] = std::fma(regs[idx_fn], -regs[idx_fm], -regs[idx_fd]);
        }
    } else if (!instr.vfp_data_opcode_p && instr.vfp_data_opcode_q && instr.vfp_data_opcode_r) {
        // FSUB/FADD
        if (instr.vfp_data_opcode_s)
            regs[idx_fd] = regs[idx_fn] - regs[idx_fm];
        else
            regs[idx_fd] = regs[idx_fn] + regs[idx_fm];
    } else if (!instr.vfp_data_opcode_p && instr.vfp_data_opcode_q && !instr.vfp_data_opcode_r) {
        // FNMUL/FMUL
        if (instr.vfp_data_opcode_s)
            regs[idx_fd] = -regs[idx_fn] * regs[idx_fm];
        else
            regs[idx_fd] = regs[idx_fn] * regs[idx_fm];
    } else if (instr.vfp_data_opcode_p && !instr.vfp_data_opcode_q && !instr.vfp_data_opcode_r && !instr.vfp_data_opcode_s) {
        // FDIV
        regs[idx_fd] = regs[idx_fn] / regs[idx_fm];
    } else if (instr.vfp_data_opcode_p && instr.vfp_data_opcode_q && instr.vfp_data_opcode_r && instr.vfp_data_opcode_s) {
        // Use extension opcode (given by idx_fn)
        switch (idx_single_n) {
        case 0b00000:
            // FCPY
            // TODO: Consider FPSCR.LEN
            regs[idx_fd] = regs[idx_fm];
            break;

        case 0b00001:
            // FABS
            // TODO: Consider FPSCR.LEN
            regs[idx_fd] = (regs[idx_fm] > 0) ? regs[idx_fm] : (-regs[idx_fm]);
            break;

        case 0b00011:
            // FSQRT
            // TODO: Consider FPSCR.LEN
            // TODO: Consider rounding mode from FPSCR
            regs[idx_fd] = std::sqrt(regs[idx_fm]);
            break;

        case 0b01000: // FCMP
        case 0b01001: // FCMP
        {
            // FCMP(E) - Compare (with Exceptions on quiet NaNs)
            // TODO: Verify remainders of the instruction?

            // TODO: Raise exceptions if Sd or Sm are NaN
            //       (mind the differences between FCMP and FCMPS though)
            ctx.cpu.fpscr.less = regs[idx_fd] < regs[idx_fm];
            ctx.cpu.fpscr.equal = regs[idx_fd] == regs[idx_fm];
            ctx.cpu.fpscr.greater_equal_unordered = !(regs[idx_fd] < regs[idx_fm]);

            // TODO: No idea whether this works as intended:
            //       We here assume that if we are "greater_equal_unordered" but neither greater nor equal, then we are unordered
            ctx.cpu.fpscr.unordered = ctx.cpu.fpscr.greater_equal_unordered && !(regs[idx_fd] >= regs[idx_fm]);

            break;
        }

        case 0b01010: // FCMPZ
        case 0b01011: // FCMPEZ
        {
            // FCMP(E)Z - Compare (with Exceptions on quiet NaNs) with Zero
            // TODO: Verify remainders of the instruction?

            // TODO: Raise exceptions if Sd or Sm are NaN
            //       (mind the differences between FCMPZ and FCMPSZ though)
            ctx.cpu.fpscr.less = regs[idx_fd] < 0.f;
            ctx.cpu.fpscr.equal = regs[idx_fd] == 0.f;
            ctx.cpu.fpscr.greater_equal_unordered = !(regs[idx_fd] < 0.f);

            // TODO: No idea whether this works as intended:
            //       We here assume that if we are "greater_equal_unordered" but neither greater nor equal, then we are unordered
            ctx.cpu.fpscr.unordered = ctx.cpu.fpscr.greater_equal_unordered && !(regs[idx_fd] >= 0.f);

            break;
        }

        case 0b10000:
        {
            // FUITO - Unsigned Integer TO Single/Double

            // First, get the integer stored in the single-precision register
            uint32_t integer;
            memcpy(&integer, &ctx.cpu.fpreg[idx_single_m], sizeof(integer));

            // Cast the integer to a single-/double-precision float
            regs[idx_fd] = static_cast<RegType>(integer);
            break;
        }

        case 0b10001:
        {
            // FSITO - Signed Integer TO Single/Double

            // First, get the integer stored in the single-precision register
            int32_t integer;
            memcpy(&integer, &ctx.cpu.fpreg[idx_single_m], sizeof(integer));

            // Cast the integer to a single-/double-precision float
            regs[idx_fd] = static_cast<RegType>(integer);
            break;
        }

        case 0b11000:
        case 0b11001:
        {
            // FTOUI - Float TO Unsigned Integer
            // TODO: The lowest bit in the extended opcode defines that we should use RZ mode rather than the rounding mode given by FPSCR

            // TODO: If NaN, an invalid operation exception is raised and the result is zero if the exception is untrapped.

            auto value = ClampToIntegerRange<RegType, uint32_t>(regs[idx_fm], 0, 0xFFFFFFFF);
            memcpy(&ctx.cpu.fpreg[idx_single_d], &value, sizeof(value));
            break;
        }

        case 0b11010:
        case 0b11011:
        {
            // FTOSI - Float TO Signed Integer
            // TODO: The lowest bit in the extended opcode defines that we should use RZ mode rather than the rounding mode given by FPSCR

            // TODO: If NaN, an invalid operation exception is raised and the result is zero if the exception is untrapped.

            auto value = ClampToIntegerRange<RegType, int32_t>(regs[idx_fm], static_cast<int32_t>(0x80000000), 0x7FFFFFFF);
            memcpy(&ctx.cpu.fpreg[idx_single_d], &value, sizeof(value));
            break;
        }

        case 0b00010:
        {
            // FNEG - Negate
            // TODO: Consider FPSCR.LEN
            regs[idx_fd] = -regs[idx_fm];
            break;
        }

        case 0b01111:
        {
            // FCVT
            if (IsDouble) {
                // double -> single
                ctx.cpu.fpreg[idx_single_d] = ctx.cpu.dpreg[idx_double_m];
            } else {
                // single -> double
                ctx.cpu.dpreg[idx_double_d] = ctx.cpu.fpreg[idx_single_m];
            }
            break;
        }

        default:
            return HandlerStubAnnotated(ctx, instr, __LINE__);
        }
    } else {
        return HandlerStubAnnotated(ctx, instr, __LINE__);
    }

    return NextInstr(ctx);
}

static uint32_t HandlerVFPRegisterTransfer(CPUContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    bool is_double = ViewBitField<8, 1, uint32_t>(instr.raw);

    auto idx_fn = (instr.idx_rn << 1) | instr.vfp_data_N;

    switch ((instr.raw >> 20) & 0xF) {
    case 0b0000:
    {
        if (is_double)
            return HandlerStubWithMessage(ctx, instr, "Double code path not implemented");

        // FMSR / FMDLR (Floating-point Move to Double-precision Low from Register)
        auto value = ctx.cpu.FetchReg(instr.idx_rd);
        memcpy(&ctx.cpu.fpreg[idx_fn], &value, sizeof(value));
        break;
    }

    case 0b0001:
    {
        // FMRS - Move to Register
        if (instr.idx_rd == ARM::Regs::PC)
            return HandlerStubWithMessage(ctx, instr, "Unpredictable configuration");

        if (is_double)
            return HandlerStubWithMessage(ctx, instr, "Double code path not implemented");

        uint32_t value;
        memcpy(&value, &ctx.cpu.fpreg[idx_fn], sizeof(value));
        ctx.cpu.reg[instr.idx_rd] = value;
        break;
    }

    case 0b0010:
    {
        // FMDHR - Floating-point Move to Double-precision High from Register
        if (instr.idx_rd == ARM::Regs::PC)
            return HandlerStubWithMessage(ctx, instr, "Unpredictable configuration");

        if (!is_double)
            return HandlerStubWithMessage(ctx, instr, "Unknown instruction");

        // Load register into high part of the double register
        memcpy(&ctx.cpu.fpreg[idx_fn+1], &ctx.cpu.reg[instr.idx_rd], sizeof(uint32_t));
        break;
    }

    case 0b1110:
    {
        // FMXR - Move to System Register
        // System register is determined by idx_fn
        if (is_double)
            return HandlerStubWithMessage(ctx, instr, "Double code path not implemented");

        auto value = ctx.cpu.FetchReg(instr.idx_rd);
        if (idx_fn == 0b00010) {
            ctx.cpu.fpscr.raw = value;
        } else {
            // Unhandled system register
            return HandlerStubAnnotated(ctx, instr, __LINE__);
        }
        break;
    }

    case 0b1111:
    {
        // TODO: Check FMSTAT (Rd=15?)

        // FMRX - Move from System Register
        // (If Rd=15, this is referred to as FMSTAT)
        // System register is determined by idx_fn
        if (is_double)
            return HandlerStubWithMessage(ctx, instr, "Double code path not implemented");

        uint32_t value;
        if (idx_fn == 0b00010) {
            value = ctx.cpu.fpscr.raw;
        } else {
            // Unhandled system register
            return HandlerStubAnnotated(ctx, instr, __LINE__);
        }

        // FMRX from FPSCR to the PC is actually an FMSTAT instruction
        if (idx_fn == 0b00010 && instr.idx_rd == ARM::Regs::PC) {
            // Copy condition flags from FPSCR to CPSR (discard other 28 bits)
            ctx.cpu.cpsr.neg = ctx.cpu.fpscr.less.Value();
            ctx.cpu.cpsr.zero = ctx.cpu.fpscr.equal.Value();
            ctx.cpu.cpsr.carry = ctx.cpu.fpscr.greater_equal_unordered.Value();
            ctx.cpu.cpsr.overflow = ctx.cpu.fpscr.unordered.Value();
        } else {
            // Otherwise, just copy the value
            ctx.cpu.reg[instr.idx_rd] = value;
        }
        break;
    }

    default:
        return HandlerStubAnnotated(ctx, instr, __LINE__);
    }

    return NextInstr(ctx);
}

static uint32_t Handler1110(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if ((instr.raw & 0b1111'0000'0000'0000'1111'0001'0000) == 0b1110'0000'0000'0000'1010'0000'0000) {
        return HandlerVFPDataProcessing<false>(ctx, instr);
    } else if ((instr.raw & 0b1111'0000'0000'0000'1111'0001'0000) == 0b1110'0000'0000'0000'1011'0000'0000) {
        return HandlerVFPDataProcessing<true>(ctx, instr);
    } else if ((instr.raw & 0b1111'0000'0000'0000'1110'0111'1111) == 0b1110'0000'0000'0000'1010'0001'0000) {
        return HandlerVFPRegisterTransfer(ctx, instr);
    } else if ((instr.raw & 0x100010) == 0x100010) {
        return HandlerMRC(ctx, instr);
    } else if ((instr.raw & 0x100010) == 0x10) {
        return HandlerMCR(ctx, instr);
    } else {
        return HandlerStubWithMessage(ctx, instr, "Unknown 0b1110 instruction");
    }
}

static uint32_t Handler100P(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    // Handled via interpreter_dispatch_table instead
    return HandlerStubAnnotated(ctx, instr, __LINE__);
}

// SWI/SVC - software interrupt / supervisor call
static uint32_t HandlerSWI(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    if (!EvalCond(ctx, instr.cond))
        return NextInstr(ctx);

    // TODO: Actually, we should be moving to supervisor mode before deferring to the OS

    // Reset monitor address to avoid thread tearing issues where an
    // LDREX-STREX pair is interrupted by a rescheduling system call
    ClearExclusive(ctx);

    ctx.cfl.SVC(ctx, instr.raw & 0xFFFFFF);

    auto* thread = ctx.os->active_thread;
    try {
        thread->YieldForSVC(instr.raw & 0xFFFFFF);
    } catch (HLE::OS::Thread* stopped_thread) {
        ctx.setup->os->SwitchToSchedulerFromThread(*thread);
        throw std::runtime_error("Attempted to resume stopped thread");
    }

    return NextInstr(ctx);
}

static uint32_t Handler1111(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    // Handled via interpreter_dispatch_table instead
    return HandlerStubAnnotated(ctx, instr, __LINE__);
}

static InterpreterARMHandler handlers_arm_prim[] = {
    Handler00x0, Handler0001, Handler00x0, Handler0011,
    Handler01xx, Handler01xx, Handler01xx, Handler01xx,
    Handler100P, Handler100P, HandlerBranch<false>, HandlerBranch<true>,
    Handler1100, Handler1101, Handler1110, Handler1111
};
static_assert(sizeof(handlers_arm_prim) / sizeof(handlers_arm_prim[0]) == 16, "Must have exactly 16 primary ARM instruction handlers");

static uint32_t LegacyHandler(InterpreterExecutionContext& ctx, ARM::ARMInstr arminstr) {
    return handlers_arm_prim[arminstr.opcode_prim](ctx, arminstr);
}

using InterpreterARMHandlerForJIT = std::add_pointer<uint32_t(ExecutionContext&, ARM::ARMInstr)>::type;

// TODO: Remove once all handlers have been changed to take an InterpreterExecutionContext argument
template<auto F>
inline constexpr InterpreterARMHandlerForJIT Wrap =
    +[](ExecutionContext& ctx, ARM::ARMInstr instr) -> uint32_t {
        return F(static_cast<InterpreterExecutionContext&>(ctx), instr);
};

InterpreterARMHandlerForJIT LookupHandler(ARM::Instr instr) {
    switch (instr) {
    case ARM::Instr::AND: return Wrap<HandlerAnd>;
    case ARM::Instr::EOR: return Wrap<HandlerEor>;
    case ARM::Instr::SUB: return Wrap<HandlerSub>;
    case ARM::Instr::RSB: return Wrap<HandlerRsb>;
    case ARM::Instr::ADD: return Wrap<HandlerAdd>;
    case ARM::Instr::ADC: return Wrap<HandlerAdc>;
    case ARM::Instr::SBC: return Wrap<HandlerSbc>;
    case ARM::Instr::RSC: return Wrap<HandlerRsc>;
    case ARM::Instr::TST: return Wrap<HandlerTst>;
    case ARM::Instr::TEQ: return Wrap<HandlerTeq>;
    case ARM::Instr::CMP: return Wrap<HandlerCmp>;
    case ARM::Instr::CMN: return Wrap<HandlerCmn>;
    case ARM::Instr::ORR: return Wrap<HandlerOrr>;
    case ARM::Instr::MOV: return Wrap<HandlerMov>;
    case ARM::Instr::BIC: return Wrap<HandlerBic>;
    case ARM::Instr::MVN: return Wrap<HandlerMvn>;

    case ARM::Instr::MUL: return Wrap<HandlerMul>;

    case ARM::Instr::SSUB8: return Wrap<HandlerXsub8<true>>;
    case ARM::Instr::QSUB8: return Wrap<HandlerQsub8>;
    case ARM::Instr::UADD8: return Wrap<HandlerUadd8>;
    case ARM::Instr::USUB8: return Wrap<HandlerXsub8<false>>;
    case ARM::Instr::UQADD8: return Wrap<HandlerUqadd8>;
    case ARM::Instr::UQSUB8: return Wrap<HandlerUqsub8>;
    case ARM::Instr::UHADD8: return Wrap<HandlerUhadd8>;
    case ARM::Instr::SSAT: return Wrap<HandlerSsat>;
    case ARM::Instr::USAT: return Wrap<HandlerUsat>;

    case ARM::Instr::SXTAH: return Wrap<HandlerSxtah>;
    case ARM::Instr::UXTB16: return Wrap<HandlerUxtb16>;
    case ARM::Instr::UXTAH: return Wrap<HandlerUxtah>;

    case ARM::Instr::B: return Wrap<HandlerBranch<false>>;
    case ARM::Instr::BL: return Wrap<HandlerBranch<true>>;
    case ARM::Instr::BX: return Wrap<HandlerBranchExchange<false>>;
//    case ARM::Instr::BLX: return Wrap<HandlerBranchExchange<true>>;

    case ARM::Instr::LDR: return Wrap<HandlerMemoryAccess<false, false>>;
    case ARM::Instr::LDRB: return Wrap<HandlerMemoryAccess<true, false>>;
    case ARM::Instr::LDRH: return Wrap<HandlerAddrMode3<ARM::AddrMode3AccessType::LoadUnsignedHalfword>>;
    case ARM::Instr::LDRSH: return Wrap<HandlerAddrMode3<ARM::AddrMode3AccessType::LoadSignedHalfword>>;
    case ARM::Instr::LDRSB: return Wrap<HandlerAddrMode3<ARM::AddrMode3AccessType::LoadSignedByte>>;
    case ARM::Instr::LDRD: return Wrap<HandlerAddrMode3<ARM::AddrMode3AccessType::LoadDoubleword>>;
    case ARM::Instr::STR: return Wrap<HandlerMemoryAccess<false, true>>;
    case ARM::Instr::STRB: return Wrap<HandlerMemoryAccess<true, true>>;
    case ARM::Instr::STRH: return Wrap<HandlerAddrMode3<ARM::AddrMode3AccessType::StoreHalfword>>;
    case ARM::Instr::STRD: return Wrap<HandlerAddrMode3<ARM::AddrMode3AccessType::StoreDoubleword>>;

    case ARM::Instr::LDM: return Wrap<HandlerLDM_STM<true>>;
    case ARM::Instr::STM: return Wrap<HandlerLDM_STM<false>>;

    case ARM::Instr::MSR: return Wrap<HandlerMSR>;
//    case ARM::Instr::MRS: return Wrap<HandlerMRS;

    case ARM::Instr::VLDR: return Wrap<LoadStoreFloatSingle>;
    case ARM::Instr::VSTR: return Wrap<LoadStoreFloatSingle>;
    case ARM::Instr::VLDM: return Wrap<LoadStoreFloatMultiple>;
    case ARM::Instr::VSTM: return Wrap<LoadStoreFloatMultiple>;

    case ARM::Instr::VFP_S:
        return +[](ExecutionContext& ctx_, ARM::ARMInstr arminstr) -> uint32_t {
            auto& ctx = static_cast<InterpreterExecutionContext&>(ctx_);
            if (ViewBitField<4, 1, uint32_t>(arminstr.raw)) {
                return HandlerVFPRegisterTransfer(ctx, arminstr);
            } else {
                return HandlerVFPDataProcessing<false>(ctx, arminstr);
            }
        };

    case ARM::Instr::VFP_D:
        return +[](ExecutionContext& ctx_, ARM::ARMInstr arminstr) -> uint32_t {
            auto& ctx = static_cast<InterpreterExecutionContext&>(ctx_);
            if (ViewBitField<4, 1, uint32_t>(arminstr.raw)) {
                return HandlerVFPRegisterTransfer(ctx, arminstr);
            } else {
                return HandlerVFPDataProcessing<true>(ctx, arminstr);
            }
        };

    case ARM::Instr::MCRR_VFP: return Wrap<HandlerFMDRR>;
    case ARM::Instr::MRRC_VFP: return Wrap<HandlerFMRDD>;

    case ARM::Instr::SWI: return Wrap<HandlerSWI>;

    default:
        return Wrap<LegacyHandler>;
    }
}

static const auto default_dispatch_table = GenerateDispatchTable(LookupHandler, Wrap<LegacyHandler>);

static uint32_t HandlerStubThumb(CPUContext& ctx, ARM::ThumbInstr instr, const std::string& message) {
    std::stringstream err;
    err << "Unknown instruction 0x" << std::hex << std::setw(4) << std::setfill('0') << instr.raw;
    if (!message.empty())
        err << ": " << message;

    throw std::runtime_error("Unknown Thumb instruction: " + err.str());
}

// NOTE: Only use the return value of this function for instructions that never access the PC!
template<auto interpreter_dispatch_table>
static uint32_t ForwardThumbToARM(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    // Forward the call to the ARM instruction:
    (void)(*interpreter_dispatch_table)[ARM::BuildDispatchTableKey(instr.raw)](ctx, instr);

    // Return current instruction + 2 for the next instruction
    return ctx.cpu.reg[15] + 2;
}

// Variant of ForwardThumbToARM. This function may be called for ARM instructions that read the PC: In this case, we make sure that the read returns the instruction address plus 4 instead of plus 8.
// NOTE: Only use the return value of this function for instructions that never modify the PC!
template<auto interpreter_dispatch_table>
static uint32_t ForwardThumbToARMMayReadPC(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    // Subtract 4 from the PC
    auto old_pc = ctx.cpu.reg[15];
    ctx.cpu.reg[15] -= 4;

    // Forward the call to the ARM instruction:
    (void)(*interpreter_dispatch_table)[ARM::BuildDispatchTableKey(instr.raw)](ctx, instr);

    // Return current instruction + 2 for the next instruction
    return old_pc + 2;
}

template<auto arm_dispatch_table>
static uint32_t DispatchThumb(InterpreterExecutionContext& ctx, ARM::ThumbInstr instr) {
    if (auto decoded = DecodeThumb(instr); decoded.arm_equivalent) {
        uint32_t next_instr =
                decoded.may_read_pc
                ? ForwardThumbToARMMayReadPC<arm_dispatch_table>(ctx, *decoded.arm_equivalent)
                : ForwardThumbToARM<arm_dispatch_table>(ctx, *decoded.arm_equivalent);

        if (decoded.may_modify_pc) {
            return ctx.cpu.reg[15];
        } else {
            return next_instr;
        }
    } else if (instr.opcode_upper5 == 0b10100) {
        // ADD (5)
        // This is slightly different from the ARM ADD since it ignores the lowest two PC bits before adding
        // TODO: This should be possible to achieve by modifying PC in place like we do for LDR (3) below
        auto result = ((ctx.cpu.reg[15] + 4) & 0xfffffffc);
        result += instr.immed_low_8 * 4;
        ctx.cpu.reg[instr.idx_rd_high] = result;
        return ctx.cpu.reg[15] + 2;
    } else if (instr.opcode_upper5 == 0b01001) {
        // LDR (3)
        // NOTE: There is a subtle differences between the Thumb encoding of this instruction and the equivalent ARM encoding!
        //       In particular, this instruction ignores bit1 in the program counter, while on ARM having bit1 set causes unpredictable behavior.
        ARM::ARMInstr arm_instr;
        arm_instr.raw = (0b1110'0101'1001'1111ul << 16)
                        | (instr.idx_rd_high << 12)
                        | (instr.immed_low_8 << 2);
        // The Thumb encoding of this instruction ignores bit1 in the PC. Hence, let's emulate this here
        auto actual_pc = ctx.cpu.reg[15];
        ctx.cpu.reg[15] &= ~0x2;
        // Furthermore, reads must return instr_offset+4 rather than instr_offset+8 (returned by FetchReg). Hence, subtract 4 here.
        ctx.cpu.reg[15] -= 4;
        (void)HandlerMemoryAccess<false, false>(ctx, arm_instr);
        return actual_pc + 2;
    } else if (instr.opcode_upper4 == 0b1101) {
        // B (1) - conditional branch
        if (!EvalCond(ctx, instr.cond))
            return ctx.cpu.reg[15] + 2;

        // Sign-extend offset
        uint32_t offset = instr.signed_immed_low_8;
        return ctx.cpu.reg[15] + 4 + (offset << 1);
    } else if (instr.opcode_upper3 == 0b111) {
        switch (ViewBitField<11, 2, uint16_t>(instr.raw)) {
        case 0b00:
        {
            // B (2) - unconditional Branch
            // Sign-extend offset
            uint32_t offset = instr.signed_immed_11.Value();
            return ctx.cpu.reg[15] + 4 + (offset << 1);
        }

        case 0b10:
            // First instruction constituting a BL or BLX (1) sequence
            // NOTE: This implements a far jump by splitting the instruction into two.
            //       The first instruction stores the first half of the target offset in the LR register,
            ctx.cpu.LR() = (ctx.cpu.PC() + 4) + (static_cast<int32_t>(instr.signed_immed_11) << 12);
            return ctx.cpu.PC() + 2;

        case 0b01:
        case 0b11:
        {
            // Second instruction constituting a BL or BLX (1) sequence
            // Combines the embedded offset with the LR value (initialized in the first instruction) and stores the result in PC.
            bool thumb = ViewBitField<12, 1, uint16_t>(instr.raw);
            uint32_t target = ctx.cpu.LR() + (instr.unsigned_immed_11 << 1);
            if (!thumb)
                target &= 0xFFFFFFFC;

            ctx.cpu.LR() = (ctx.cpu.PC() + 2) | 1; // Address of next instruction
            ctx.cpu.cpsr.thumb = thumb;
            ctx.RecordCall(ctx.cpu.PC(), target, ctx.cpu);
            ctx.cfl.Branch(ctx, "bl(x)", target);
            return target;
        }

        default:
            return 0; // TODO: UNREACHABLE
        }
    } else if (instr.opcode_upper9 == 0b0100'0111'1) {
        // BLX (2) - Branch with Link and Exchange
        auto idx_rm = instr.idx_rm | (instr.idx_rm_upperbit << 3);
        if (idx_rm == ARM::Regs::PC)
            return HandlerStubThumb(ctx, instr, "Unpredictable configuration");

        // Link - make sure to save the LR in case it's used as the target specifier
        auto target = ctx.cpu.reg[idx_rm];
        ctx.cpu.LR() = (ctx.cpu.reg[15] + 2) | 1;

        // Exchange and Branch
        ctx.cpu.cpsr.thumb = target & 1;

        ctx.RecordCall(ctx.cpu.PC(), target & 0xFFFFFFFE, ctx.cpu);
        ctx.cfl.Branch(ctx, "blx_t", target);

        return target & 0xFFFFFFFE;
    } else if (instr.opcode_upper9 == 0b0100'0111'0) {
        // BX - Branch with Exchange
        auto idx_rm = instr.idx_rm | (instr.idx_rm_upperbit << 3);
        if (idx_rm == ARM::Regs::PC)
            return HandlerStubThumb(ctx, instr, "Unimplemented configuration");

        ctx.cfl.Return(ctx, "bx reg t");

        // Exchange and Branch
        auto target = ctx.cpu.reg[idx_rm];
        ctx.cpu.cpsr.thumb = target & 1;
        return target & 0xFFFFFFFE;
    } else if (instr.opcode_upper7 == 0b1011'110) {
        // POP - Pop Multiple Registers
        // NOTE: This modifies the PC if bit8 is set
        ARM::ARMInstr arm_instr;
        arm_instr.raw = (0b1110'1000'1011'1101ul << 16)
                        | ((instr.raw & 0x100) << 7) // bit8 denotes whether to pop PC
                        | instr.register_list;
//        auto ret = ForwardThumbToARM(ctx, arm_instr);
        // Call the handler directly to get the jump target address
        auto ret = HandlerLDM_STM<true>(ctx, arm_instr);
        if (instr.raw & 0x100) {
            // If we loaded to PC, jump to the return value
//             ctx.cfl.Return(ctx, "pop_t");
            return ret;
        } else {
            // If we didn't load to PC, jump to the next instruction (which
            // is 2 minus the return value since HandlerLDM_STM, being
            // an ARM handler, added 4 to the current PC)
            return ret - 2;
        }
    } else {
        return HandlerStubThumb(ctx, instr, "");
    }
}

void Processor::UnregisterContext(ExecutionContext& context) {
    auto ctx_it = std::find(contexts.begin(), contexts.end(), &context);
    if (ctx_it == contexts.end()) {
        throw std::runtime_error("Attempted to unregister unknown ExecutionContext");
    }
    contexts.erase(ctx_it);
}

struct Interpreter final : public ProcessorWithDefaultMemory {
    Interpreter(Setup& setup_) : ProcessorWithDefaultMemory(setup_) {
    }

    ~Interpreter() override = default;

    void Run(ExecutionContext& ctx, ProcessorController& controller, uint32_t process_id, uint32_t thread_id) override;

    InterpreterExecutionContext* CreateExecutionContextImpl2() override;
};

InterpreterExecutionContext* Interpreter::CreateExecutionContextImpl2() {
    return new InterpreterExecutionContext(*this, setup);
}

template<auto arm_dispatch_table>
static void StepWithDispatchTable(ExecutionContext& ctx_) try {
    auto& ctx = static_cast<InterpreterExecutionContext&>(ctx_);
//    if (!ctx.backtrace.empty() && ctx.cpu.PC() == ctx.backtrace.back().source + (ctx.cpu.cpsr.thumb ? 2 : 4))
//        ctx.backtrace.pop_back();

    // TODO: Instead of translating the PC here over and over again, just have the OS allocate linear .text memory instead and just get a pointer to it using Memory::LookupContiguousMemoryBackedPage!
    uint32_t pc_phys = *ctx.TranslateVirtualAddress(ctx.cpu.PC());

    // Fetch and process next instruction
    if (ctx.cpu.cpsr.thumb) {

// TODO: Do this check when a jump happens!
//         if (pc_phys % 2)
//             throw std::runtime_error("Unaligned THUMB PC");

        ARM::ThumbInstr instr = { ReadPhysicalMemory<uint16_t>(ctx.setup->mem, pc_phys) };
        ctx.cpu.PC() = DispatchThumb<arm_dispatch_table>(ctx, instr);
    } else {
// TODO: Do this check when a jump or thumb/arm mode switch happens!
//         if (pc_phys % 4)
//             throw std::runtime_error("Unaligned ARM PC");

        // TODO: This is always an aligned read. We can considerably speed up this operation with that in mind!
        ARM::ARMInstr instr = { ReadPhysicalMemory<uint32_t>(ctx.setup->mem, pc_phys) };
        if (instr.cond != 0xf) {
            ctx.cpu.PC() = (*arm_dispatch_table)[ARM::BuildDispatchTableKey(instr.raw)](ctx_, instr);
        } else {
            // Handle unconditional instructions explicitly

            if (instr.opcode_prim == 0b1010 || instr.opcode_prim == 0b1011) {
                // Branch with Link and Exchange
                Link(ctx);

                ctx.cpu.cpsr.thumb = 1;

                // bit24 determines the halfword at which to resume execution
                uint32_t target = ctx.cpu.PC() + 8 + ((4 * instr.branch_target) | ((instr.raw & 0x1000000) >> 23));
                ctx.RecordCall(ctx.cpu.PC(), target, ctx.cpu);
                ctx.cfl.Branch(ctx, "bx_0xf", target);
                ctx.cpu.PC() = target;
            } else if (instr.raw == 0xf57ff01f) {
                // CLREX
                ClearExclusive(ctx);
                ctx.cpu.PC() = NextInstr(ctx);
            } else if ((instr.raw & 0b1111'1101'0111'0000'1111'0000'0000'0000) == 0b1111'0101'0101'0000'1111'0000'0000'0000) {
                // pld variants, not sure what this does specifically, but we probably don't need to implement it.
                ctx.cpu.PC() = NextInstr(ctx);
            } else {
                std::stringstream ss;
                ss << std::hex << std::setw(8) << std::setfill('0') << instr.raw;
                throw std::runtime_error("Unknown unconditional instruction 0x" + ss.str());
            }
        }
    }
} catch (const boost::context::detail::forced_unwind&) {
    throw;
} catch (...) {
    fmt::print( "Exception thrown while running interpreter at PC {:#x} (process id {})\n",
                static_cast<InterpreterExecutionContext&>(ctx_).cpu.PC(), static_cast<InterpreterExecutionContext&>(ctx_).os->active_thread->GetParentProcess().GetId());
    throw;
}

void Step(ExecutionContext& ctx) {
    StepWithDispatchTable<&default_dispatch_table>(ctx);
}

static void TriggerPreemption(InterpreterExecutionContext& ctx) {
    // NS shared font loading thread. This may not be preempted, otherwise we don't finish loading the font by the time applications want to access it
    if (ctx.setup->os->active_thread->GetParentProcess().GetId() == 7 &&
        ctx.setup->os->active_thread->GetId() == 2) {
        return;
    }


    // Reset monitor address to avoid thread tearing issues where an
    // LDREX-STREX pair is interrupted by preemption
    ClearExclusive(ctx);

    // Preempt the current thread every now and then to make sure we don't end
    // up stuck in infinite loops waiting for other threads to do something.
    // NOTE: Technically, the CPU core applications generally run on does not
    //       use preemptive scheduling, however threads on that core regardless
    //       may be preempted under certain cirumstances. Hence, this isn't as
    //       much of a hack as it might seem to be.
    // NOTE: This seems to be commonly used to spinlock for HID to update
    //       shared memory fields
    // TODO: This might cause issues with some of our HLE code. Make sure to support ldrex/strex in HLE code to prevent race conditions!
    ctx.os->active_thread->callback_for_svc = [](std::shared_ptr<HLE::OS::Thread> thread) {
        thread->GetOS().Reschedule(thread);
    };
    ctx.os->SwitchToSchedulerFromThread(*ctx.os->active_thread);
}

void Interpreter::Run(ExecutionContext& ctx_, ProcessorController& controller, uint32_t process_id, uint32_t thread_id) try {
    auto& ctx = static_cast<InterpreterExecutionContext&>(ctx_);
    ctx.controller = &controller;
    for (;;) {
        if (!ctx.debugger_attached) {
            // Run a bunch of instructions at a time, then check the debugging state again
            for (int i = 0; i < 10000; ++i) {
//            for (int i = 0; i < ctx.os->active_thread->GetParentProcess().GetId() == 17 ? 10 : 10000; ++i) {
                ++ctx.cpu.cycle_count;
                StepWithDispatchTable<&default_dispatch_table>(ctx);
            }

            TriggerPreemption(ctx);
        } else {
            for (auto& bp : ctx.breakpoints) {
                if (bp.address == ctx.cpu.PC()) {
                    std::cerr << "INTERPRETER HIT BREAKPOINT" << std::endl;
                    // Notify debugger about the breakpoint
                    controller.NotifyBreakpoint(process_id, thread_id);
                    controller.paused = true;
                    break;
                }
            }

            // Check software breakpoints written by GDB
            // TODO: This adds one redundant memory read per iteration... instead change Step() to ProcessInstruction()!
            if (!ctx.cpu.cpsr.thumb) {
                ARM::ARMInstr instr = { ctx.ReadVirtualMemory<uint32_t>(ctx.cpu.PC()) };

                // "Trap"
                if (instr.raw == 0xe7ffdefe) {
                    controller.NotifyBreakpoint(process_id, thread_id);
                    controller.paused = true;
                }
            } else {
                // "Trap"
                ARM::ThumbInstr instr = { ctx.ReadVirtualMemory<uint16_t>(ctx.cpu.PC()) };
                if ((instr.raw & 0xff00) == 0xbe00) {
                    controller.NotifyBreakpoint(process_id, thread_id);
                    controller.paused = true;
                }
            }

            if (ctx.trap_on_resume) {
                ctx.trap_on_resume = false;
                ctx.controller->NotifyBreakpoint(ctx.os->active_thread->GetParentProcess().GetId(), ctx.os->active_thread->GetId());
                ctx.controller->paused = true;
            }

            if (controller.ShouldPause(process_id, thread_id)) {
                // Set paused and wait for acknowledgement
                controller.paused = true;
                while (controller.request_pause) {
                }
            }

            if (controller.paused) {
                // Wait until we are requested to continue, then unpause, then wait until the unpausing has been noticed
                while (!controller.request_continue) {
                }
                controller.paused = false;
                while (controller.request_continue) {
                }
            }

            // Single step
            StepWithDispatchTable<&default_dispatch_table>(ctx);
            ++ctx.cpu.cycle_count;
            if ((ctx.cpu.cycle_count & 0xFFF) == 0) {
                TriggerPreemption(ctx);
            }
        }
    }
} catch (const boost::context::detail::forced_unwind&) {
    throw;
} catch (...) {
    fmt::print(stderr, "Exception thrown while running interpreter at PC {:#x}\n", static_cast<InterpreterExecutionContext&>(ctx_).cpu.PC());
    throw;
}

std::unique_ptr<Processor> CreateInterpreter(Setup& setup) {
    return std::make_unique<Interpreter>(setup);
}

/**
 * Fallback interpreter usable by a JIT while translating binary code in the background
 *
 * This interpreter works just like the usual one, with the difference that on a branch it will yield control back to the given coroutine, passing it the branch target address
 */
struct TemporaryInterpreterForJIT final : public ProcessorWithDefaultMemory {
    TemporaryInterpreterForJIT(Setup& setup_)
        : ProcessorWithDefaultMemory(setup_) {
    }

    ~TemporaryInterpreterForJIT() override = default;

    void Run(ExecutionContext& ctx, ProcessorController& controller, uint32_t process_id, uint32_t thread_id) override;

    InterpreterExecutionContext* CreateExecutionContextImpl2() override;
};

// TODO: Come up with a cleaner interface...
void SetParentCoroutine(ExecutionContext& ctx, boost::coroutines2::coroutine<uint32_t>::push_type& coro) {
    static_cast<InterpreterExecutionContext&>(ctx).coro = &coro;
}

InterpreterExecutionContext* TemporaryInterpreterForJIT::CreateExecutionContextImpl2() {
    return new InterpreterExecutionContext(*this, setup);
}

template<bool link>
static uint32_t HandlerBranchForTemporaryInterpreter(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    // Jumping to a function; notify JIT about this

    auto target = HandlerBranch<link>(ctx, instr);
    // TODO: Fix for HostThreadBasedThreadControl
    (*ctx.coro)(target);
    // TODO: What if JIT decides to stop interpreting here? Upon resume, will assign this target to PC...
    return target;
}

static uint32_t HandlerSWIForTemporaryInterpreter(InterpreterExecutionContext& ctx, ARM::ARMInstr instr) {
    // Switch back to the JIT since OS uses the JIT's ExecutionContext when processing system calls
    // TODO: Fix for HostThreadBasedThreadControl
    (*ctx.coro)(ctx.cpu.reg[15]);

    return NextInstr(ctx);
}

InterpreterARMHandlerForJIT LookupHandlerForTemporaryInterpreter(ARM::Instr instr) {
    // TODO: Enable other instructions, but take care that the branch target is always a *function* rather than just a basic block

    switch (instr) {
    // TODO: Consider supporting branches through these!
//    case ARM::Instr::AND: return Wrap<HandlerAnd>;
//    case ARM::Instr::EOR: return Wrap<HandlerEor>;
//    case ARM::Instr::SUB: return Wrap<HandlerSub>;
//    case ARM::Instr::RSB: return Wrap<HandlerRsb>;
//    case ARM::Instr::ADD: return Wrap<HandlerAdd>;
//    case ARM::Instr::ADC: return Wrap<HandlerAdc>;
//    case ARM::Instr::SBC: return Wrap<HandlerSbc>;
//    case ARM::Instr::RSC: return Wrap<HandlerRsc>;
//    case ARM::Instr::TST: return Wrap<HandlerTst>;
//    case ARM::Instr::TEQ: return Wrap<HandlerTeq>;
//    case ARM::Instr::CMP: return Wrap<HandlerCmp>;
//    case ARM::Instr::CMN: return Wrap<HandlerCmn>;
//    case ARM::Instr::ORR: return Wrap<HandlerOrr>;
//    case ARM::Instr::MOV: return Wrap<HandlerMov>;
//    case ARM::Instr::BIC: return Wrap<HandlerBic>;
//    case ARM::Instr::MVN: return Wrap<HandlerMvn>;

//    case ARM::Instr::B: return Wrap<HandlerBranchForTemporaryInterpreter<false>>;
    case ARM::Instr::BL: return Wrap<HandlerBranchForTemporaryInterpreter<true>>;
//    case ARM::Instr::BX: return Wrap<HandlerBranchExchangeForTemporaryInterpreter<false>>;
//    case ARM::Instr::BLX: return Wrap<HandlerBranchExchange<true>>;

    // TODO: Support branches through loading to PC!
//    case ARM::Instr::LDR: return Wrap<HandlerMemoryAccess<false, false>>;
//    case ARM::Instr::LDRB: return Wrap<HandlerMemoryAccess<true, false>>;
//    case ARM::Instr::LDRH: return Wrap<HandlerAddrMode3<ARM::AddrMode3AccessType::LoadUnsignedHalfword>>;
//    case ARM::Instr::LDRSH: return Wrap<HandlerAddrMode3<ARM::AddrMode3AccessType::LoadSignedHalfword>>;
//    case ARM::Instr::LDRSB: return Wrap<HandlerAddrMode3<ARM::AddrMode3AccessType::LoadSignedByte>>;
//    case ARM::Instr::LDRD: return Wrap<HandlerAddrMode3<ARM::AddrMode3AccessType::LoadDoubleword>>;

//    case ARM::Instr::LDM: return Wrap<HandlerLDM_STM<true>>;

    case ARM::Instr::SWI: return Wrap<HandlerSWIForTemporaryInterpreter>;

    default:
        return LookupHandler(instr);
    }
}

static const auto temporary_interpreter_dispatch_table = GenerateDispatchTable(LookupHandlerForTemporaryInterpreter, Wrap<LegacyHandler>);

void TemporaryInterpreterForJIT::Run(ExecutionContext& ctx_, ProcessorController& controller, uint32_t process_id, uint32_t thread_id) {
    auto& ctx = static_cast<InterpreterExecutionContext&>(ctx_);
    ctx.controller = &controller;
    for (;;) {
//        std::cerr << "TemporaryInterpreter running at 0x" << ctx.cpu.reg[15] << std::endl;
        StepWithDispatchTable<&temporary_interpreter_dispatch_table>(ctx);
    }
}

std::unique_ptr<Processor> CreateTemporaryInterpreterForJIT(Setup& setup) {
    return std::make_unique<TemporaryInterpreterForJIT>(setup);
}

// TODO: Better interface
uint32_t ReadPCFrom(ExecutionContext& ctx) {
    return static_cast<InterpreterExecutionContext&>(ctx).cpu.reg[15];
}

// TODO: Better interface
bool CheckIsThumbFrom(ExecutionContext& ctx) {
    return (static_cast<InterpreterExecutionContext&>(ctx).cpu.cpsr.thumb == 1);
}

}  // namespace Interpreter
