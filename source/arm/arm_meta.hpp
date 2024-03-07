#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace ARM {

enum class Instr {
    AND,
    EOR,
    SUB,
    RSB,
    ADD,
    ADC,
    SBC,
    RSC,
    TST,
    TEQ,
    CMP,
    CMN,
    ORR,
    MOV,
    BIC,
    MVN,

    MUL,

    SSUB8,
    QSUB8,
    UADD8,
    USUB8,
    UQADD8,
    UQSUB8,
    UHADD8,
    SSAT,
    USAT,

    SXTAH, // SXTAH and SXTH
    UXTB16,
    UXTAH, // UXTAH and UXTH

    B,
    BL,
    BX,
    BLX,
    BLXImm,

    LDR,
    LDRB,
    LDRH,
    LDRSH,
    LDRSB,
    LDRD,
    STR,
    STRB,
    STRH,
    STRD,

    LDM,
    STM,

    // Move (general-purpose) Register to PSR (Program Status Register)
    // Note that this encoding can be a nop by setting the field mask to all-zeroes
    MSR,

    MRRC, // Move to ARM registers from Coprocessor
    MCRR, // Move to Coprocessor from ARM registers

    // VFP instructions
    VLDR, // Load single register from memory
    VSTR, // Store single register to memory

    VLDM, // Load multiple registers from memory
    VSTM, // Store multiple registers to memory

    VFP_S, // VFP (single precision)
    VFP_D, // VFP (double precision)

    MRRC_VFP, // Specialization for VFP coprocessor
    MCRR_VFP, // Specialization for VFP coprocessor

    SWI, // Software Interrupt (i.e. system call)

    Unknown
};

namespace detail {

constexpr std::array<char, 32> to_array(const char (&arr)[33]) {
    std::array<char, 32> ret { };
    for (std::size_t i = 0; i < ret.size(); ++i) {
        ret[i] = arr[i];
    }
    return ret;
}

template<typename Rng, typename Value>
constexpr auto find(Rng&& rng, Value&& value) {
    using std::begin;
    using std::end;
    auto it = begin(rng);
    auto endit = end(rng);
    for (; it != endit; ++it) {
        if (*it == value) {
            break;
        }
    }
    return it;
}

template<typename Rng, typename Value>
constexpr bool contains(Rng&& rng, Value&& value) {
    using std::end;
    return (find(std::forward<Rng>(rng), std::forward<Value>(value)) != end(rng));
}

} // namespace detail

struct InstrMetaInfo {
    constexpr InstrMetaInfo(const char (&arr)[33]) : bit_encoding(detail::to_array(arr)) {
    }

    /**
     * 32-character string specifying the semantics of each bit:
     * m, n, d, s: Rm, Rn, Rd, Rs
     * c: Conditional execution code
     * 'o', '/': Should be zero/one
     * A, B, C, D, E: Placeholders for fields specific to addressing modes 1, 2, 3, 4, and 5, respectively
     * P: Placeholder for coprocessor instructions
     * S (update conditional execution flags NCVZ)
     */
    const std::array<char, 32> bit_encoding;
};

inline constexpr std::pair<Instr, InstrMetaInfo> meta_table[] = {
    // Operations at the bottom have higher priority for dispatch

    //               UPPER          ....          LOWER
    { Instr::AND,    "cccc00A0000SnnnnddddAAAAAAAAAAAA" },
    { Instr::EOR,    "cccc00A0001SnnnnddddAAAAAAAAAAAA" },
    { Instr::SUB,    "cccc00A0010SnnnnddddAAAAAAAAAAAA" },
    { Instr::RSB,    "cccc00A0011SnnnnddddAAAAAAAAAAAA" },
    { Instr::ADD,    "cccc00A0100SnnnnddddAAAAAAAAAAAA" },
    { Instr::ADC,    "cccc00A0101SnnnnddddAAAAAAAAAAAA" },
    { Instr::SBC,    "cccc00A0110SnnnnddddAAAAAAAAAAAA" },
    { Instr::RSC,    "cccc00A0111SnnnnddddAAAAAAAAAAAA" },
    { Instr::TST,    "cccc00A10001nnnnooooAAAAAAAAAAAA" },
    { Instr::TEQ,    "cccc00A10011nnnnooooAAAAAAAAAAAA" },
    { Instr::CMP,    "cccc00A10101nnnnooooAAAAAAAAAAAA" },
    { Instr::CMN,    "cccc00A10111nnnnooooAAAAAAAAAAAA" },
    { Instr::ORR,    "cccc00A1100SnnnnddddAAAAAAAAAAAA" },
    { Instr::MOV,    "cccc00A1101SooooddddAAAAAAAAAAAA" },
    { Instr::BIC,    "cccc00A1110SnnnnddddAAAAAAAAAAAA" },
    { Instr::MVN,    "cccc00A1111SooooddddAAAAAAAAAAAA" },

    // NOTE: Rn indeed is used as the destination register here
    { Instr::MUL,    "cccc0000000Sddddoooossss1001mmmm" },

    { Instr::SSUB8,  "cccc01100001nnnndddd////1111mmmm" },
    { Instr::QSUB8,  "cccc01100010nnnndddd////1111mmmm" },
    { Instr::UADD8,  "cccc01100101nnnndddd////1001mmmm" },
    { Instr::USUB8,  "cccc01100101nnnndddd////1111mmmm" },
    { Instr::UQADD8, "cccc01100110nnnndddd////1001mmmm" },
    { Instr::UQSUB8, "cccc01100110nnnndddd////1111mmmm" },
    { Instr::UHADD8, "cccc01100111nnnndddd////1001mmmm" },
    { Instr::SSAT,   "cccc0110101-----dddd------01mmmm" },
    { Instr::USAT,   "cccc0110111-----dddd------01mmmm" },

    // NOTE: Also covers SXTH
    { Instr::SXTAH,  "cccc01101011nnnndddd--oo0111mmmm" },
    { Instr::UXTB16, "cccc011011001111dddd--oo0111mmmm" },
    // NOTE: Also covers UXTH
    { Instr::UXTAH,  "cccc01101111nnnndddd--oo0111mmmm" },

    { Instr::B,      "cccc1010------------------------" },
    { Instr::BL,     "cccc1011------------------------" },
    { Instr::BX,     "cccc00010010////////////0001mmmm" },
    { Instr::BLX,    "cccc00010010////////////0011mmmm" }, // Register variant
    { Instr::BLXImm, "1111101-------------------------" }, // Immediate variant

    { Instr::LDR,    "cccc01BBB0B1nnnnddddBBBBBBBBBBBB" },
    { Instr::STR,    "cccc01BBB0B0nnnnddddBBBBBBBBBBBB" },
    { Instr::LDRB,   "cccc01BBB1B1nnnnddddBBBBBBBBBBBB" },
    { Instr::STRB,   "cccc01BBB1B0nnnnddddBBBBBBBBBBBB" },

    //                           L             SH
    { Instr::STRH,   "cccc000CCCC0nnnnddddCCCC1011CCCC" },
    { Instr::LDRD,   "cccc000CCCC0nnnnddddCCCC1101CCCC" },
    { Instr::STRD,   "cccc000CCCC0nnnnddddCCCC1111CCCC" },
    { Instr::LDRH,   "cccc000CCCC1nnnnddddCCCC1011CCCC" },
    { Instr::LDRSB,  "cccc000CCCC1nnnnddddCCCC1101CCCC" },
    { Instr::LDRSH,  "cccc000CCCC1nnnnddddCCCC1111CCCC" },

    { Instr::LDM,    "cccc100DDDD1nnnnDDDDDDDDDDDDDDDD" },
    { Instr::STM,    "cccc100DDDD0nnnnDDDDDDDDDDDDDDDD" },

    { Instr::MSR,    "cccc00110-10----////------------"}, // Immediate variant
    { Instr::MSR,    "cccc00010-10----////oooo0000mmmm"}, // Register variant

    { Instr::MRRC,   "cccc11000101nnnnddddPPPPPPPPmmmm" },
    { Instr::MCRR,   "cccc11000100nnnnddddPPPPPPPPmmmm" },

    { Instr::SWI,    "cccc1111------------------------" },

    // Coprocessor instructions (bit 8 indicates single/double precision)
    { Instr::VLDM,     "cccc110PPPP1nnnndddd101PEEEEEEEE" },
    { Instr::VSTM,     "cccc110PPPP0nnnndddd101PEEEEEEEE" },

    { Instr::MRRC_VFP, "cccc11000101nnnndddd101P00P1mmmm" },
    { Instr::MCRR_VFP, "cccc11000100nnnndddd101P00P1mmmm" },

    { Instr::VLDR,     "cccc1101EE01nnnndddd101PPPPPPPPP" },
    { Instr::VSTR,     "cccc1101EE00nnnndddd101PPPPPPPPP" },

    { Instr::VFP_S,    "cccc1110PdPPnnnndddd1010nPm0mmmm" },
    // TODO: Most variants of this only allow upper n/d/m to be 0!
    { Instr::VFP_D,    "cccc1110PdPPnnnndddd1011nPm0mmmm" },
};

template<typename F>
constexpr std::array<std::invoke_result_t<F, Instr>, 8192> GenerateDispatchTable(F&& f, std::invoke_result_t<F, Instr> default_value = { }) {
    std::array<std::invoke_result_t<F, Instr>, 8192> table { };
    for (auto& entry : table) {
        entry = default_value;
    }

    for (auto& instr : meta_table) {
        const bool is_addressing_mode_1 = detail::contains(instr.second.bit_encoding, 'A');
        const bool is_addressing_mode_2 = detail::contains(instr.second.bit_encoding, 'B');
        const bool is_addressing_mode_5 = detail::contains(instr.second.bit_encoding, 'E');
        const bool is_coprocessor_op = is_addressing_mode_5 || detail::contains(instr.second.bit_encoding, 'P');

        uint32_t output_key = 0;
        uint32_t entropy = 0; // Number of non-fixed bits (i.e. logarithmic entropy)
        uint32_t dynamic_bits[12] { };

        for (uint32_t key_bit = 0; key_bit < 12; ++key_bit) {
            // Encoding bits 27-20 and 7-4 (in that order)
            uint32_t index_bits[] = { 27u, 26u, 25u, 24u, 11u, 10u, 9u, 8u, 7u, 6u, 5u, 4u };
            if (is_coprocessor_op) {
                // For coprocessor (addressing mode 5) instructions, use bits 11-8 instead of 7-4
                index_bits[0] = 23;
                index_bits[1] = 22;
                index_bits[2] = 21;
                index_bits[3] = 20;
            }
            switch (instr.second.bit_encoding[index_bits[key_bit]]) {
            // Fixed bit: Always 0
            case '0':
            case 'o':
                break;

            // Fixed bit: Always 1
            case '1':
            case '/':
                output_key |= (uint32_t { 1 } << key_bit);
                break;

            default:
                // Dynamic bit:
                // Either 0 or 1; possible encodings are enumerated below.
                // Instruction-specific encoding constraints are enforced later
                dynamic_bits[entropy] = key_bit;
                ++entropy;
                break;
            }
        }

        auto entry = f(instr.first);
        // With N dynamic bits, there are up to N different encodings that match the instruction pattern. Let's iterate over all of them:
        for (uint32_t encoding_index = 0; encoding_index < (1u << entropy); ++encoding_index) {
            // Translate the index of the encoding to the key in the dispatch table
            uint32_t dynamic_key = output_key;
            for (uint32_t dynamic_bit_index = 0; dynamic_bit_index < entropy; ++dynamic_bit_index) {
                if (encoding_index & (1 << dynamic_bit_index)) {
                    dynamic_key |= (1 << dynamic_bits[dynamic_bit_index]);
                }
            }

            // Enforce instruction-specific constraints:
            if (is_addressing_mode_1) {
                // If bit 4 and 7 in the instruction are set and bit 25 is unset, this is not an addressing mode 1 instruction
                if (dynamic_key == (dynamic_key | 0b1001) && ((dynamic_key >> 9) & 1) == 0) {
                    // Discard this encoding
                    continue;
                }
            }

            if (is_addressing_mode_2) {
                // If bit 4 and 25 in the instruction are set, this is not an addressing mode 2 instruction
                if (dynamic_key == (dynamic_key | 0b1) && ((dynamic_key >> 9) & 1) == 1) {
                    // Discard this encoding
                    continue;
                }
            }

            // TODO: For addressing mode 3, (!instr.ldr_P && instr.ldr_W) should filter out the key!

            if (is_addressing_mode_5 && (instr.first == Instr::VLDM || instr.first == Instr::VSTM || instr.first == Instr::VLDR || instr.first == Instr::VSTR)) {
                // If bit 21 (W) is set, bit 23 (U) and bit 24 (P) must be different
                // (otherwise this is instruction is UNDEFINED)
                const bool w = ((dynamic_key >> 5) & 1);
                const bool u = ((dynamic_key >> 7) & 1);
                const bool p = ((dynamic_key >> 8) & 1);
                if (w && (u == p)) {
                    // Discard this encoding
                    continue;
                }
            }

            if (instr.second.bit_encoding[0] == '1' &&
                instr.second.bit_encoding[1] == '1' &&
                instr.second.bit_encoding[2] == '1' &&
                instr.second.bit_encoding[3] == '1') {
                // Distinguish special unconditional operations in the table key
                dynamic_key |= (1 << 12);
            }

            table[dynamic_key] = entry;
        }
    }

    return table;
}

constexpr uint32_t BuildDispatchTableKey(uint32_t arm_instr) {
    // For coprocessor instructions, use bits 8-11 instead of 4-7
    const bool is_coprocessor = (((arm_instr >> 25) & 0x7) == 0b110 || ((arm_instr >> 24) & 0xf) == 0b1110);
    uint32_t lower_key = (is_coprocessor ? ((arm_instr & 0xf00) >> 8) : ((arm_instr & 0xf0) >> 4));
    return ((uint32_t { (arm_instr >> 28) == 0xf } << 12) | ((arm_instr & 0xff00000) >> 16) | lower_key);
}

} // namespace ARM
