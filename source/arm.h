#pragma once

#include "bit_field.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace ARM {

using BitField::v1::ViewBitField;
using BitField::v1::BitField;

namespace Regs {
enum {
    LR = 14,
    PC = 15,
};
}

enum class ProcessorMode {
    User       = 0b10000,
    FIQ        = 0b10001,
    IRQ        = 0b10010,
    Supervisor = 0b10011,
    Abort      = 0b10111,
    Undefined  = 0b11011,
    System     = 0b11111,
};

// More convenient internal storage
enum class InternalProcessorMode : uint32_t {
    User,
    FIQ,
    IRQ,
    Supervisor,
    Abort,
    Undefined,
    System,
    NumModes,
    Invalid = NumModes // Reserved for actually invalid (or "reserved") processor modes
};
static constexpr size_t NumInternalProcessorModes = static_cast<size_t>(InternalProcessorMode::NumModes);

inline InternalProcessorMode MakeInternal(ProcessorMode mode) {
    switch (mode) {
    case ProcessorMode::User: return InternalProcessorMode::User;
    case ProcessorMode::FIQ: return InternalProcessorMode::FIQ;
    case ProcessorMode::IRQ: return InternalProcessorMode::IRQ;
    case ProcessorMode::Supervisor: return InternalProcessorMode::Supervisor;
    case ProcessorMode::Abort: return InternalProcessorMode::Abort;
    case ProcessorMode::Undefined: return InternalProcessorMode::Undefined;
    case ProcessorMode::System: return InternalProcessorMode::System;
    default: return InternalProcessorMode::Invalid;
    }
}

inline ProcessorMode MakeNative(InternalProcessorMode mode) {
    switch (mode) {
    case InternalProcessorMode::User: return ProcessorMode::User;
    case InternalProcessorMode::FIQ: return ProcessorMode::FIQ;
    case InternalProcessorMode::IRQ: return ProcessorMode::IRQ;
    case InternalProcessorMode::Supervisor: return ProcessorMode::Supervisor;
    case InternalProcessorMode::Abort: return ProcessorMode::Abort;
    case InternalProcessorMode::Undefined: return ProcessorMode::Undefined;
    case InternalProcessorMode::System: return ProcessorMode::System;
    default: throw std::runtime_error("Invalid internal processor mode");
    }
}

enum class AddressingShift : uint32_t {
    LSL     = 0,
    LSR     = 1,
    ASR     = 2,
    ROR_RRX = 3
};

struct DoublePrecisionRegister {
    uint32_t raw_low;
    uint32_t raw_high;
};

struct State {
    std::array<uint32_t,16> reg;

    // VFP (floating point) registers
    // TODO: Current usage of this union is not compatible with strict aliasing rules!
    union {
        std::array<float, 32> fpreg;
        std::array<double, 16> dpreg;
        std::array<DoublePrecisionRegister, 16> dpreg_raw;
    };

    // FP Status and Control Register
    union {
        uint32_t raw;

        BitField<28, 1, uint32_t> unordered; // aka V
        BitField<29, 1, uint32_t> greater_equal_unordered; // aka C
        BitField<30, 1, uint32_t> equal; // aka Z
        BitField<31, 1, uint32_t> less; // aka N
    } fpscr;

    // Current Program Status Register
    union ProgramStatusRegister {
        /**
         * Copies the given PSR into *this, as if a memcpy() had been performed.
         */
        void RawCopyFrom(const ProgramStatusRegister& psr) {
            reinterpret_cast<uint32_t&>(*this) = reinterpret_cast<const uint32_t&>(psr);
        }

        // Processor Mode ("M"): Guaranteed to be one of the values defined in #InternalProcessorMode
        BitField< 0, 5, InternalProcessorMode> mode;    // "M"

        // BitField< 0, 5, ProcessorMode> native_mode; // Used only for accessing within MSR. Do not use outside of that!

        BitField< 5, 1, uint32_t> thumb;    // "T"

        BitField< 6, 1, uint32_t> F;        // disable FIQ interrupts
        BitField< 7, 1, uint32_t> I;        // disable IRQ interrupts
        BitField< 8, 1, uint32_t> A;        // disable imprecise data aborts

        BitField<16, 4, uint32_t> ge;

        BitField<16, 2, uint32_t> ge10;
        BitField<18, 2, uint32_t> ge32;

        BitField<16, 1, uint32_t> ge0;
        BitField<17, 1, uint32_t> ge1;
        BitField<18, 1, uint32_t> ge2;
        BitField<19, 1, uint32_t> ge3;

        BitField<27, 1, uint32_t> q; // overflow or saturation in saturated integer arithmetic

        BitField<28, 1, uint32_t> overflow; // "V"
        BitField<29, 1, uint32_t> carry;    // "C"
        BitField<30, 1, uint32_t> zero;     // "Z", "equal"
        BitField<31, 1, uint32_t> neg;      // "N"

        bool InPrivilegedMode() const {
            return (mode != InternalProcessorMode::User);
        }

        /**
         * Fill PSR contents using the given value.
         * This is required to make sure the processor mode field is converted to InternalProcessorMode properly.
         */
        static ProgramStatusRegister FromNativeRaw32(uint32_t val) {
            ProgramStatusRegister psr;
            reinterpret_cast<uint32_t&>(psr) = val;
            psr.mode = MakeInternal(ViewBitField<0, 5, ProcessorMode>(val));
            return psr;
        }

        /**
         * Return the raw uint32_t value expected by the ARM ISA.
         * This is required to make sure the processor mode field is converted to ProcessorMode properly.
         */
        uint32_t ToNativeRaw32() const {
            uint32_t val = reinterpret_cast<const uint32_t&>(*this);
            ViewBitField<0, 5, ProcessorMode>(val).Assign(MakeNative(mode));
            return val;
        }
    };

    ProgramStatusRegister cpsr;
    ProgramStatusRegister spsr[NumInternalProcessorModes]; // The SPSRs for User and System are just dummies!

    bool InPrivilegedMode() const {
        return cpsr.InPrivilegedMode();
    }

    bool HasSPSR() const {
        return (cpsr.mode != InternalProcessorMode::User && cpsr.mode != InternalProcessorMode::System);
    }

    ProgramStatusRegister& GetSPSR(InternalProcessorMode mode) {
        return spsr[static_cast<uint32_t>(mode)];
    }

    std::array<uint32_t,7> banked_regs_user; // r8-r14 (virtual bank used whenever another mode has its registers banked in; shared with System mode)
    std::array<uint32_t,8> banked_regs_fiq;  // r8-r14 + SPSR_fiq
    std::array<uint32_t,3> banked_regs_svc;  // r13 + r14 + SPSR_fiq
    std::array<uint32_t,3> banked_regs_abt;  // r13 + r14 + SPSR_abt
    std::array<uint32_t,3> banked_regs_irq;  // r13 + r14 + SPSR_irq
    std::array<uint32_t,3> banked_regs_und;  // r13 + r14 + SPSR_und

    /**
     * Move the given PSR to the CPSR register, performing a mode switch and other handling if necessary
     */
    void ReplaceCPSR(ProgramStatusRegister psr) {
        if (cpsr.mode == psr.mode) {
            cpsr.RawCopyFrom(psr);
            return;
        }

        // Bank out current registers
        switch (cpsr.mode) {
        case InternalProcessorMode::User:
        case InternalProcessorMode::System:
            std::copy(&reg[8], &reg[15], banked_regs_user.begin());
            break;

        case InternalProcessorMode::FIQ:
            std::copy(&reg[8], &reg[15], banked_regs_fiq.begin());
            break;

        case InternalProcessorMode::IRQ:
            std::copy(&reg[13], &reg[15], banked_regs_irq.begin());
            break;

        case InternalProcessorMode::Supervisor:
            std::copy(&reg[13], &reg[15], banked_regs_svc.begin());
            break;

        case InternalProcessorMode::Abort:
            std::copy(&reg[13], &reg[15], banked_regs_abt.begin());
            break;

        case InternalProcessorMode::Undefined:
            std::copy(&reg[13], &reg[15], banked_regs_und.begin());
            break;

        // It should not be possible to set cpsr.mode to this value
        case InternalProcessorMode::Invalid:
            assert(false);
            break;
        }

        // Bank in new registers
        switch (cpsr.mode) {
        case InternalProcessorMode::User:
        case InternalProcessorMode::System:
            std::copy(banked_regs_user.begin(), banked_regs_user.end(), &reg[8]);
            break;

        case InternalProcessorMode::FIQ:
            std::copy(banked_regs_fiq.begin(), banked_regs_fiq.end(), &reg[8]);
            break;

        case InternalProcessorMode::IRQ:
            std::copy(banked_regs_irq.begin(), banked_regs_irq.end(), &reg[13]);
            break;

        case InternalProcessorMode::Supervisor:
            std::copy(banked_regs_svc.begin(), banked_regs_svc.end(), &reg[13]);
            break;

        case InternalProcessorMode::Abort:
            std::copy(banked_regs_abt.begin(), banked_regs_abt.end(), &reg[13]);
            break;

        case InternalProcessorMode::Undefined:
            std::copy(banked_regs_und.begin(), banked_regs_und.end(), &reg[13]);
            break;

        // It should not be possible to set cpsr.mode to this value
        case InternalProcessorMode::Invalid:
            assert(false);
            break;
        }
    }

    uint32_t& LR() {
        return reg[14];
    }

    uint32_t& PC() {
        return reg[15];
    }

    uint32_t FetchReg(unsigned index) {
        if (index == 15)
            return PC() + 8;

        return reg[index];
    }

    union {
        // NOTE: The internal mapping from "raw" to any of the getter functions below is arbitrary.
        uint32_t raw[6];

        auto& CPUId() {
            union Type {
                uint32_t raw;

                BitField< 0,  4, uint32_t> CPUID;
                BitField< 4,  4, uint32_t> reserved1; // SBZ
                BitField< 8,  4, uint32_t> ClusterID;
                BitField<12, 20, uint32_t> reserved2; // SBZ
            };
            return reinterpret_cast<Type&>(raw[2]);
        }

        auto& Control() {
            union Type {
                uint32_t raw;

                BitField< 0, 1, uint32_t> M; // MMU enable
                BitField< 1, 1, uint32_t> A; // strict alignment fault checking enable
                BitField< 2, 1, uint32_t> C; // data cache enable
                BitField< 3, 4, uint32_t> reserved1; // Should Be One
                BitField< 7, 1, uint32_t> reserved2; // Should Be Zero
                BitField< 8, 1, uint32_t> S; // System protection (deprecated)
                BitField< 9, 1, uint32_t> R; // ROM protection (deprecated)
                BitField<10, 1, uint32_t> reserved3; // Should Be Zero
                BitField<11, 1, uint32_t> Z; // program flow prediction enable
                BitField<12, 1, uint32_t> I; // L1 instruction cache enable
                BitField<13, 1, uint32_t> V; // location of exception vectors: 0=normal(0x00000000), 1=high(0xFFFF0000)
                BitField<14, 1, uint32_t> reserved4; // Read As One/Should Be One
                BitField<15, 1, uint32_t> L4; // If zero, loads to PC will set the T bit (otherwise, they won't)
                BitField<16, 6, uint32_t> reserved5; // Read As 0b000101
                BitField<22, 1, uint32_t> U;  // unaligned data access operation enable (including support for mixed-endianness data)
                BitField<23, 1, uint32_t> XP; // hardware page translation: subpage AP bits enable
                BitField<24, 1, uint32_t> reserved6; // SBZ/RAZ
                BitField<25, 1, uint32_t> EE; // Value is copied to the E bit of CPSR when taking an exception
                BitField<26, 1, uint32_t> reserved7; // SBZ/RAZ
                BitField<27, 1, uint32_t> NMFI; // If one, FIQs behave as NMFIs (otherwise, they have normal behavior)
                BitField<28, 1, uint32_t> TEX_remap; // TEX remap enable
                BitField<29, 1, uint32_t> force_AP; // Force AP enable
                BitField<30, 2, uint32_t> reserved8; // SBZ/RAZ
            };
            return reinterpret_cast<Type&>(raw[0]);
        }

        auto& AuxiliaryControl() {
            union Type {
                uint32_t raw;

                BitField<0,  1, uint32_t> RS;   // Return stack enable (set on reset)
                BitField<1,  1, uint32_t> DB;   // Dynamic branch prediction enable (set on reset)
                BitField<2,  1, uint32_t> SB;   // Static branch prediction enable (set on reset)
                BitField<3,  1, uint32_t> F;    // Instruction folding enable
                BitField<4,  1, uint32_t> EXCL; // 0 = L1 and L2 caches are inclusive, 1 = caches are exclusive
                BitField<5,  1, uint32_t> SMP;  // 0 = AMP (no coherency), 1 = SMP (coherency)
                BitField<6,  1, uint32_t> L1_parity_checking; // L1 parity checking enable

                BitField<7, 25, uint32_t> reserved; // Should be preserved
            };
            return reinterpret_cast<Type&>(raw[1]);
        }

        // privileged modes, only
        auto& TranslationTableBase0() {
            union Type {
                uint32_t raw;

                BitField< 3, 2, uint32_t> RGN; // Out cachable attributes for page table walking

                BitField< 1, 1, uint32_t> S; // Page table walk to shared (S=1) or nonshared (S=0) memory

                // Bits 13-ttbc.N:5 UNP/SBZ.

                /*template<typename TTBC>
                uint32_t GetTTB0(const TTBC& ttbc) const {
                    // return 18-N uppermost bits
                    return BitFieldView<14, 18, uint32_t>(raw) >> ttbc.N;
                }*/
            };
            return reinterpret_cast<Type&>(raw[3]);
        }

        // TODO: TranslationTableBase1

        // privileged modes, only
        auto& TranslationTableBaseControl() {
            union Type {
                uint32_t raw;

                // NOTE: Reading this register (or specifically, "N") returns the size of the page table boundary for TTBR0. SBZ_UNP should be zero then.
                //       This is probably equivalent to just returning the value of N, though...

                BitField< 0,  3, uint32_t> N; // 0=Use Translation Table Base Register 0; otherwise=Consider TTBR1
                BitField< 3, 29, uint32_t> SBZ_UNP; // Should Be Zero or Unpredictable
            };
            return reinterpret_cast<Type&>(raw[4]);
        }

        // Accessible from all modes
        auto& ThreadLocalStorage() {
            union Type {
                // TODO: Double-check that this does not contain any other fields
                uint32_t virtual_addr;
            };
            return reinterpret_cast<Type&>(raw[5]);
        }
    } cp15;

    uint64_t cycle_count;
};
static_assert(std::is_pod<State>::value, "State is not a POD type");

enum class OperandShifterMode : uint32_t {
    LSL = 0,     // Logical Shift Left
    LSR = 1,     // Logical Shift Right
    ASR = 2,     // Arithmetic Shift Right
    ROR_RRX = 3, // ROtate Right or Rotate Right with Extend
};

union ARMInstr {
    uint32_t raw;

    BitField<28, 4, uint32_t> cond;

    BitField<24, 4, uint32_t> opcode_prim;
    BitField< 4, 20, uint32_t> identifier_4_23;
    BitField<20,  8, uint32_t> identifier_20_27;

    BitField< 0, 24, int32_t> branch_target;

    BitField< 0, 8, uint32_t> immed_8;

    BitField<22, 1, uint32_t> R;

    BitField<25, 1, uint32_t> msr_I;
    BitField<16, 4, uint32_t> msr_field_mask;

    uint32_t ExpandMSRFieldMask() const {
        return (msr_field_mask & 0x8) * (0xFF << 21)
             | (msr_field_mask & 0x4) * (0xFF << 14)
             | (msr_field_mask & 0x2) * (0xFF << 7)
             | (msr_field_mask & 0x1) * 0xFF;
    }

    // Addressing Mode 1
    BitField< 5, 2, OperandShifterMode> addr1_shift;
    BitField< 7, 5, uint32_t> addr1_shift_imm;
    BitField< 8, 4, uint32_t> rotate_imm;
    BitField<20, 1, uint32_t> addr1_S; // update CPSR flags

    BitField< 0, 4, uint32_t> addr3_immed_lo;
    BitField< 8, 4, uint32_t> addr3_immed_hi;

    // The HLS bits are weird: Originally they were used to configure
    // signed/unsigned (S), load/store (L), and halfword/byte (H).
    // However, certain configurations are invalid or have a different meaning:
    // S=0, H=0 is a different instruction
    // S=1, L=0 is used to define double-word access
    BitField< 5, 1, uint32_t> addr3_H;
    BitField< 6, 1, uint32_t> addr3_S;
    BitField<20, 1, uint32_t> addr3_L;

    // Addressing Mode 4 - Load/Store Multiple
    BitField< 0, 16, uint32_t> addr4_registers;
    BitField<20,  1, uint32_t> addr4_L; // 0=Store, 1=Load
    BitField<21,  1, uint32_t> addr4_W; // Update base register after the transfer
    BitField<22,  1, uint32_t> addr4_S; // ??
    BitField<23,  1, uint32_t> addr4_U; // 0=Decrement address, 1=Increment address
    BitField<24,  1, uint32_t> addr4_P; // 0=Change address after transfer, 1=Change address before transfer

    // NOTE: Despite their name, these are generic to all adressing modes, it seems.
    // TODO: These are actually addressing mode 2/5 fields
    BitField<21, 1, uint32_t> ldr_W;
    BitField<23, 1, uint32_t> ldr_U;
    BitField<24, 1, uint32_t> ldr_P;
    BitField<25, 1, uint32_t> ldr_I;

    // For VFP instructions, this bit is used to select a register index (D=0: bottom 16 registers, D=1: top 16 registers)
    BitField< 0, 8, uint32_t> addr5_offset; // actual offset is obtained by multiplying with 4

    BitField<20, 1, uint32_t> addr5_L; // 0=Store, 1=Load
    BitField<22, 1, uint32_t> addr5_D;

    BitField< 5, 2, OperandShifterMode> ldr_shift;
    BitField< 7, 5, uint32_t> ldr_shift_imm;
    BitField< 0, 12, uint32_t> ldr_offset;

    BitField< 0, 4, uint32_t> idx_rm;
    BitField< 8, 4, uint32_t> idx_rs;
    BitField<12, 4, uint32_t> idx_rd;
    BitField<16, 4, uint32_t> idx_rn;

    BitField<16, 5, uint32_t> sat_imm;

    BitField< 6, 1, uint32_t> cps_F;
    BitField< 7, 1, uint32_t> cps_I;
    BitField< 8, 1, uint32_t> cps_A;
    BitField<19, 1, uint32_t> cps_imod_enable; // modify interrupt flags
    BitField<18, 1, uint32_t> cps_imod_value;  // new value for interrupt flags (used iff cps_imod_enable is true)
    BitField<17, 1, uint32_t> cps_mmod; // modify processor mode
    BitField< 0, 5, ProcessorMode> cps_mode;

    BitField< 8, 4, uint32_t> coproc_id;
    BitField<21, 3, uint32_t> coproc_opcode1;
    BitField< 5, 3, uint32_t> coproc_opcode2;

    BitField<10, 2, uint32_t> uxtb_rotate;

    BitField< 5, 1, uint32_t> vfp_data_M;
    BitField< 7, 1, uint32_t> vfp_data_N;

    BitField< 6, 1, uint32_t> vfp_data_opcode_s;
    BitField<20, 1, uint32_t> vfp_data_opcode_r;
    BitField<21, 1, uint32_t> vfp_data_opcode_q;
    BitField<23, 1, uint32_t> vfp_data_opcode_p;
};

enum class AddrMode1Encoding {
    Imm,
    ShiftByImm,
    ShiftByReg,
    Unknown
};

inline AddrMode1Encoding GetAddrMode1Encoding(const ARMInstr& instr) {
    auto raw = instr.raw;

    // Bits 26 and 27 must be zero
    bool is_valid_addr_mode1_instruction = (0 == ViewBitField<26, 2, uint32_t>(raw));
    if (!is_valid_addr_mode1_instruction)
        return AddrMode1Encoding::Unknown;

    auto is_immediate = ViewBitField<25, 1, uint32_t>(raw);
    if (is_immediate)
        return AddrMode1Encoding::Imm;

    // Check whether this is a register shift or an immediate shift
    auto is_register_shift = ViewBitField<4, 1, uint32_t>(raw);
    if (!is_register_shift)
        return AddrMode1Encoding::ShiftByImm;

    // register shift encodings must have bit 7 set to zero to be valid.
    auto is_valid_register_shift = (0 == ViewBitField<7, 1, uint32_t>(raw));
    if (is_valid_register_shift)
        return AddrMode1Encoding::ShiftByReg;

    return AddrMode1Encoding::Unknown;
}

enum class AddrMode3AccessType {
    // These three load a small value into a larger register, hence sign-extension may be wished
    LoadSignedByte,
    LoadSignedHalfword,
    LoadUnsignedHalfword,

    // These load values into equally-sized registers, hence sign-extension is not applicable
    LoadDoubleword,
    StoreByte,
    StoreHalfword,
    StoreDoubleword,

    // Not an addressing mode 3 instruction
    Invalid
};

union ThumbInstr {
    uint16_t raw;

    BitField<13,  3, uint16_t> opcode_upper3;
    BitField<12,  4, uint16_t> opcode_upper4;
    BitField<11,  5, uint16_t> opcode_upper5;
    BitField<10,  6, uint16_t> opcode_upper6;
    BitField< 9,  7, uint16_t> opcode_upper7;
    BitField< 8,  8, uint16_t> opcode_upper8;
    BitField< 7,  9, uint16_t> opcode_upper9;
    BitField< 6, 10, uint16_t> opcode_upper10;

    BitField<0, 3, uint16_t> idx_rd_low;
    BitField<3, 3, uint16_t> idx_rm;
    BitField<6, 3, uint16_t> idx_rn;
    BitField<8, 3, uint16_t> idx_rd_high;

    BitField<6, 1, uint16_t> idx_rm_upperbit;
    BitField<7, 1, uint16_t> idx_rd_upperbit;

    BitField<6, 5, uint16_t> immed_mid_5;
    BitField<6, 3, uint16_t> immed_mid_5_lower3;
    BitField<9, 2, uint16_t> immed_mid_5_upper2;

    BitField<0, 7, uint16_t> immed_low_7;

    BitField<0, 8, uint16_t> immed_low_8;
    BitField<0, 8, int16_t> signed_immed_low_8;

    BitField<0, 11, int16_t> signed_immed_11;
    BitField<0, 11, uint16_t> unsigned_immed_11;

    // Conditional Branch
    BitField<8, 4, uint16_t> cond;

    // Push/Pop
    BitField<0, 8, uint16_t> register_list;
};

}  // namespace
