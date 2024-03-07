#include "context.h"

#include <nihstro/shader_bytecode.h>

#include <fmt/format.h>

namespace Pica::VertexShader {

using namespace nihstro;

std::string DisassembleShader(uint32_t instruction_raw, const uint32_t* swizzle_data) {
    Instruction instr = { instruction_raw };
    OpCode opcode = instr.opcode.Value();

    std::string disasm = fmt::format("{:<7}", opcode.GetInfo().name);

    // TODO: Not sure if name lookup works properly, yet!

    if (opcode.GetInfo().type == OpCode::Type::Arithmetic) {
        const SwizzlePattern& swizzle = { swizzle_data[instr.common.operand_desc_id] };

        bool src_reversed = 0 != (opcode.GetInfo().subtype & OpCode::Info::SrcInversed);
        auto src1 = instr.common.GetSrc1(src_reversed);
        auto src2 = instr.common.GetSrc2(src_reversed);
        auto dest = instr.common.dest.Value();

        std::string src1_relative_address;
        if (!instr.common.AddressRegisterName().empty())
            src1_relative_address = "[" + instr.common.AddressRegisterName() + "]";

        if (opcode.GetInfo().subtype & OpCode::Info::Dest) {
            disasm += fmt::format("{:>4}.{:<4}  ", dest.GetName(), swizzle.DestMaskToString());
        } else {
            disasm += "    ";
        }

        if (opcode.GetInfo().subtype & OpCode::Info::Src1) {
            disasm += fmt::format("{:>4}.{:<4}  ", ((swizzle.negate_src1 ? "-" : "") + src1.GetName()) + src1_relative_address, swizzle.SelectorToString(0));
        } else {
            disasm += "           ";
        }

        if (opcode.GetInfo().subtype & OpCode::Info::CompareOps) {
            disasm += fmt::format("{} {} ", instr.common.compare_op.ToString(instr.common.compare_op.x), instr.common.compare_op.ToString(instr.common.compare_op.y));
        } else {
        }

        if (opcode.GetInfo().subtype & OpCode::Info::Src2) {
            disasm += fmt::format("{:>4}.{}   ", (swizzle.negate_src2 ? "-" : "") + src2.GetName(), swizzle.SelectorToString(1));
        } else {
            disasm += "            ";
        }

//        disasm += fmt::format("{2} addr:{};      {} <- {}", instr.common.operand_desc_id.Value(), instr.common.address_register_index.Value(),
//                                                    shader_info.LookupDestName(dest, swizzle), (swizzle.negate_src1 ? "-" : "") + shader_info.LookupSourceName(src1, instr.common.address_register_index);
//        if (opcode.GetInfo().subtype & OpCode::Info::Src2)
//            std::cout << ", " << (swizzle.negate_src2 ? "-" : "") + shader_info.LookupSourceName(src2, 0);
    } else if (opcode.GetInfo().type == OpCode::Type::MultiplyAdd) {
        const SwizzlePattern& swizzle = { swizzle_data[instr.mad.operand_desc_id] };

        const bool is_inverted = false; // TODO: Implement

        if (opcode.GetInfo().subtype & OpCode::Info::SrcInversed) {
        // Disabled for debugging Rayman Origins
//            throw std::runtime_error("Not supported: MADI");
        }

        auto src1 = instr.mad.GetSrc1(is_inverted);
        auto src2 = instr.mad.GetSrc2(is_inverted);
        auto src3 = instr.mad.GetSrc3(is_inverted);
        auto dest = instr.mad.dest.Value();

        std::string src2_relative_address;
        std::string src3_relative_address;
        if (!instr.mad.AddressRegisterName().empty())
            (is_inverted ? src3_relative_address : src2_relative_address) = "[" + instr.mad.AddressRegisterName() + "]";

        disasm += fmt::format("{:>4}.{:<4}  ", dest.GetName(), swizzle.DestMaskToString());
        disasm += fmt::format("{:>4}.{:<4}  ", ((swizzle.negate_src1 ? "-" : "") + src1.GetName()), swizzle.SelectorToString(0));
        disasm += fmt::format("{:>4}.{}   ", (swizzle.negate_src2 ? "-" : "") + src2.GetName() + src2_relative_address, swizzle.SelectorToString(1));
        disasm += fmt::format("{:>4}.{}   ", (swizzle.negate_src3 ? "-" : "") + src3.GetName() + src3_relative_address, swizzle.SelectorToString(2));
    } else if (opcode.GetInfo().type == OpCode::Type::Conditional || opcode.GetInfo().type == OpCode::Type::UniformFlowControl) {
        uint32_t target_addr = 4 * instr.flow_control.dest_offset;
        uint32_t return_addr = 4 * instr.flow_control.dest_offset + 4 * instr.flow_control.num_instructions;
        // TODO: For if, this is actually the ELSE range
        disasm += fmt::format("{:#x}-{:#x}", target_addr, return_addr - 1);

        if (opcode.GetInfo().subtype & OpCode::Info::HasCondition) {
            disasm += " if cond (TODO)";
        } if (opcode.GetInfo().subtype & OpCode::Info::HasUniformIndex) {
            disasm += fmt::format(" if uniform b{}", instr.flow_control.bool_uniform_id.Value());
        }
    } else {
        switch (instr.opcode.Value()) {
        case OpCode::Id::NOP:
        case OpCode::Id::END:
            break;

        default:
            disasm += "Unknown instruction";
            break;
        }
    }
    return disasm;
}

} // namespace Pica::VertexShader
