#include <framework/exceptions.hpp>

#include <stack>

#include <common/log.h>

#include <nihstro/shader_bytecode.h>


#include "context.h"
#include "shader.hpp"
#include "shader_private.hpp"

#include <platform/gpu/pica.hpp>

#include <range/v3/algorithm/fill.hpp>

#include <boost/container/static_vector.hpp>

using nihstro::OpCode;
using nihstro::Instruction;
using nihstro::RegisterType;
using nihstro::SourceRegister;
using nihstro::SwizzlePattern;

namespace Pica {

namespace VertexShader {

struct ShaderInterpreter : ShaderEngine {
    ShaderInterpreter() : ShaderEngine(false) {}

    void Reset(const Regs::VSInputRegisterMap&, InputVertex&, uint32_t num_attributes) override;

    void UpdateUniforms(const std::array<Math::Vec4<float24>, 96>&) override {
        // Nothing to do
    }

    OutputVertex Run(Context&, uint32_t entry_point) override;

    std::array<const float24*, 16> input_register_table;

    float24 dummy_register;

    ShaderCodeOffset program_counter;

    Math::Vec4<float24> output_registers[16];

    Math::Vec4<float24> temporary_registers[16];
    bool conditional_code[2];

    // Two Address registers and one loop counter
    // TODO: How many bits do these actually have?
    s32 address_registers[3];

    enum {
        INVALID_ADDRESS = 0xFFFFFFFF
    };

    struct CallStackElement {
        ShaderCodeOffset final_address;  // Address upon which we jump to return_address
        ShaderCodeOffset return_address; // Where to jump when leaving scope
        u8 repeat_counter;  // How often to repeat until this call stack element is removed
        u8 loop_increment;  // Which value to add to the loop counter after an iteration
                            // TODO: Should this be a signed value? Does it even matter?
        ShaderCodeOffset loop_begin_address; // The start address of the current loop (0 if we're not in a loop)
    };

    // TODO: Is there a maximal size for this?
    boost::container::static_vector<CallStackElement, 16> call_stack;

    struct {
        ShaderCodeOffset max_offset; // maximum program counter ever reached
        u32 max_opdesc_id; // maximum swizzle pattern index ever used
    } debug;
};

static void LegacyInterpretShaderOpcode(
        Context& context, ShaderInterpreter& engine, nihstro::Instruction instr) {
    // Placeholder for invalid inputs
    static float24 dummy_vec4_float24[4];

    auto& swizzle_data = context.swizzle_data;
    auto& shader_uniforms = context.shader_uniforms;

    const SwizzlePattern swizzle { swizzle_data[instr.common.operand_desc_id] };

    auto LookupSourceRegister = [&](const SourceRegister& source_reg) -> const float24* {
        switch (source_reg.GetRegisterType()) {
        case RegisterType::Input:
            return engine.input_register_table[source_reg.GetIndex()];

        case RegisterType::Temporary:
            return &engine.temporary_registers[source_reg.GetIndex()].x;

        case RegisterType::FloatUniform:
            return &shader_uniforms.f[source_reg.GetIndex()].x;

        default:
            return dummy_vec4_float24;
        }
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

        const int address_offset = (instr.common.address_register_index == 0)
                                   ? 0 : engine.address_registers[instr.common.address_register_index - 1];

        const float24* src1_ = LookupSourceRegister(instr.common.GetSrc1(is_inverted) + (is_inverted ? 0 : address_offset));
        const float24* src2_ = LookupSourceRegister(instr.common.GetSrc2(is_inverted) + (is_inverted ? address_offset : 0));

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

        float24* dest = (instr.common.dest.Value() < 0x10) ? &engine.output_registers[instr.common.dest.Value().GetIndex()][0]
                    : (instr.common.dest.Value() < 0x20) ? &engine.temporary_registers[instr.common.dest.Value().GetIndex()][0]
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
                engine.address_registers[i] = static_cast<s32>(src1[i].ToFloat32());
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
        if (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MAD ||
            instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI) {
            const SwizzlePattern& swizzle = *(SwizzlePattern*)&swizzle_data[instr.mad.operand_desc_id];

            const bool is_inverted = (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI);

            const int address_offset = (instr.mad.address_register_index == 0)
                                       ? 0 : engine.address_registers[instr.mad.address_register_index - 1];

            const float24* src1_ = LookupSourceRegister(instr.mad.GetSrc1(is_inverted));
            const float24* src2_ = LookupSourceRegister(instr.mad.GetSrc2(is_inverted) + (is_inverted ? 0 : address_offset));
            const float24* src3_ = LookupSourceRegister(instr.mad.GetSrc3(is_inverted) + (is_inverted ? address_offset : 0));

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

            float24* dest = (instr.mad.dest.Value() < 0x10) ? &engine.output_registers[instr.mad.dest.Value().GetIndex()][0]
                        : (instr.mad.dest.Value() < 0x20) ? &engine.temporary_registers[instr.mad.dest.Value().GetIndex()][0]
                        : dummy_vec4_float24;

            for (int i = 0; i < 4; ++i) {
                if (!swizzle.DestComponentEnabled(i))
                    continue;

                dest[i] = src1[i] * src2[i] + src3[i];
            }
        } else {
            throw std::runtime_error(fmt::format("Unhandled multiply-add instruction: {:#x}", instr.hex));
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


static void ProcessShaderCode(Context& context, ShaderInterpreter& engine) {
    auto& shader_memory = context.shader_memory;
    auto& shader_uniforms = context.shader_uniforms;

    while (true) {
        if (!engine.call_stack.empty()) {
            auto& top = engine.call_stack.back();
            if (engine.program_counter == top.final_address) {
                engine.address_registers[2] += top.loop_increment;

                if (top.repeat_counter-- == 0) {
                    engine.program_counter = top.return_address;
                    engine.call_stack.pop_back();
                } else {
                    engine.program_counter = top.loop_begin_address;
                }

                // TODO: Is "trying again" accurate to hardware?
                continue;
            }
        }

        bool exit_loop = false;
        const Instruction instr = engine.program_counter.ReadFrom(shader_memory);

        // TODO: Evaluate performance impact of moving this up a scope
        auto call = [](ShaderInterpreter& engine, ShaderCodeOffset offset, u32 num_instructions,
                        ShaderCodeOffset return_offset, u8 repeat_count, u8 loop_increment) {
            engine.program_counter = offset.GetPrevious(); // Using previous offset to make sure when incrementing the PC we end up at the correct offset
            if (engine.call_stack.size() >= engine.call_stack.capacity()) {
                throw std::runtime_error("Shader call stack exhausted");
            }
            engine.call_stack.push_back({ offset.IncrementedBy(num_instructions), return_offset, repeat_count, loop_increment, offset });
        };
        ShaderCodeOffset current_instr_offset = engine.program_counter;

        engine.debug.max_offset = std::max(engine.debug.max_offset, current_instr_offset.GetNext());

        switch (instr.opcode.Value().GetInfo().type) {
        case OpCode::Type::Arithmetic:
        case OpCode::Type::MultiplyAdd:
            LegacyInterpretShaderOpcode(context, engine, instr);
            break;

        default:
        {
            // TODO: Evaluate performance impact of moving this up a scope
            static auto evaluate_condition = [](const ShaderInterpreter& engine, bool refx, bool refy, Instruction::FlowControlType flow_control) {
                // TODO: Evaluate performance impact of splitting this into two variables (might be easier on the optimizer)
                bool results[2] = { refx == engine.conditional_code[0],
                                    refy == engine.conditional_code[1] };

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
                if (evaluate_condition(engine, instr.flow_control.refx, instr.flow_control.refy, instr.flow_control)) {
                    engine.program_counter = ShaderCodeOffset { instr.flow_control.dest_offset }.GetPrevious();
                }
                break;

            case OpCode::Id::JMPU:
                // NOTE: The lowest bit of the num_instructions field indicates an inverted condition for this instruction.
                // TODO: Add a better-named field alias for this case
                if (shader_uniforms.b[instr.flow_control.bool_uniform_id] == !(instr.flow_control.num_instructions & 1)) {
                    engine.program_counter = ShaderCodeOffset { instr.flow_control.dest_offset }.GetPrevious();
                }
                break;

            case OpCode::Id::CALL:
                call(engine,
                     ShaderCodeOffset { instr.flow_control.dest_offset },
                     instr.flow_control.num_instructions,
                     current_instr_offset.GetNext(), 0, 0);
                break;

            case OpCode::Id::CALLU:
                if (shader_uniforms.b[instr.flow_control.bool_uniform_id]) {
                    call(engine,
                        ShaderCodeOffset { instr.flow_control.dest_offset },
                        instr.flow_control.num_instructions,
                        current_instr_offset.GetNext(), 0, 0);
                }
                break;

            case OpCode::Id::CALLC:
                if (evaluate_condition(engine, instr.flow_control.refx, instr.flow_control.refy, instr.flow_control)) {
                    call(engine,
                        ShaderCodeOffset { instr.flow_control.dest_offset },
                        instr.flow_control.num_instructions,
                        current_instr_offset.GetNext(), 0, 0);
                }
                break;

            case OpCode::Id::IFU:
                if (shader_uniforms.b[instr.flow_control.bool_uniform_id]) {
                    call(engine,
                         current_instr_offset.GetNext(),
                         ShaderCodeOffset { instr.flow_control.dest_offset } - current_instr_offset.GetNext(),
                         ShaderCodeOffset { instr.flow_control.dest_offset }.IncrementedBy(instr.flow_control.num_instructions), 0, 0);
                } else {
                    call(engine,
                         ShaderCodeOffset { instr.flow_control.dest_offset },
                         instr.flow_control.num_instructions,
                         ShaderCodeOffset { instr.flow_control.dest_offset }.IncrementedBy(instr.flow_control.num_instructions), 0, 0);
                }

                break;

            case OpCode::Id::IFC:
            {
                // TODO: Do we need to consider swizzlers here?

                if (evaluate_condition(engine, instr.flow_control.refx, instr.flow_control.refy, instr.flow_control)) {
                    call(engine,
                         current_instr_offset.GetNext(),
                         ShaderCodeOffset { instr.flow_control.dest_offset } - (current_instr_offset.GetNext()),
                         ShaderCodeOffset { instr.flow_control.dest_offset }.IncrementedBy(instr.flow_control.num_instructions), 0, 0);
                } else {
                    call(engine,
                         ShaderCodeOffset { instr.flow_control.dest_offset },
                         instr.flow_control.num_instructions,
                         ShaderCodeOffset { instr.flow_control.dest_offset }.IncrementedBy(instr.flow_control.num_instructions), 0, 0);
                }

                break;
            }

            case OpCode::Id::LOOP:
            {
                engine.address_registers[2] = shader_uniforms.i[instr.flow_control.int_uniform_id].y;

                call(engine,
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

        engine.program_counter = engine.program_counter.GetNext();

        if (exit_loop)
            break;
    }
}

void ShaderInterpreter::Reset(const Regs::VSInputRegisterMap& register_map, InputVertex& input, uint32_t num_attributes) {
    dummy_register = float24::FromFloat32(0);
    ranges::fill(input_register_table, &dummy_register);
    if(num_attributes > 0) input_register_table[register_map.attribute0_register] = &input.attr[0].x;
    if(num_attributes > 1) input_register_table[register_map.attribute1_register] = &input.attr[1].x;
    if(num_attributes > 2) input_register_table[register_map.attribute2_register] = &input.attr[2].x;
    if(num_attributes > 3) input_register_table[register_map.attribute3_register] = &input.attr[3].x;
    if(num_attributes > 4) input_register_table[register_map.attribute4_register] = &input.attr[4].x;
    if(num_attributes > 5) input_register_table[register_map.attribute5_register] = &input.attr[5].x;
    if(num_attributes > 6) input_register_table[register_map.attribute6_register] = &input.attr[6].x;
    if(num_attributes > 7) input_register_table[register_map.attribute7_register] = &input.attr[7].x;
    if(num_attributes > 8) input_register_table[register_map.attribute8_register] = &input.attr[8].x;
    if(num_attributes > 9) input_register_table[register_map.attribute9_register] = &input.attr[9].x;
    if(num_attributes > 10) input_register_table[register_map.attribute10_register] = &input.attr[10].x;
    if(num_attributes > 11) input_register_table[register_map.attribute11_register] = &input.attr[11].x;
    if(num_attributes > 12) input_register_table[register_map.attribute12_register] = &input.attr[12].x;
    if(num_attributes > 13) input_register_table[register_map.attribute13_register] = &input.attr[13].x;
    if(num_attributes > 14) input_register_table[register_map.attribute14_register] = &input.attr[14].x;
    if(num_attributes > 15) input_register_table[register_map.attribute15_register] = &input.attr[15].x;
}

OutputVertex ShaderInterpreter::Run(Context& context, uint32_t entry_point) {
    program_counter = { entry_point };
    debug.max_offset = { 0 };
    debug.max_opdesc_id = 0;

    ranges::fill(conditional_code, false);
    ranges::fill(address_registers, 0);

    call_stack.clear();

    ProcessShaderCode(context, *this);

    // Setup output data
    OutputVertex ret;
    // Map output registers to (tightly packed) output attributes and
    // assign semantics
    // TODO(neobrain): Under some circumstances, up to 16 registers may be used. We need to
    // figure out what those circumstances are and enable the remaining registers then.
    for (unsigned output_reg = 0, output_attrib = 0; output_attrib < context.registers.shader_num_output_attributes; ++output_reg ) {
        if (!(context.registers.vs_output_register_mask.mask() & (1 << output_reg))) {
            continue;
        }

        const auto& output_register_map = context.registers.shader_output_semantics[output_attrib++];

        u32 semantics[4] = {
            output_register_map.map_x, output_register_map.map_y,
            output_register_map.map_z, output_register_map.map_w
        };

        for (int comp = 0; comp < 4; ++comp) {
            float24* out = ((float24*)&ret) + semantics[comp];
            if (semantics[comp] != Regs::VSOutputAttributes::INVALID) {
                *out = output_registers[output_reg][comp];
            } else {
                // Zero output so that attributes which aren't output won't have denormals in them,
                // which would slow us down later.
                memset(out, 0, sizeof(*out));
            }
        }
    }

    LOG_TRACE(Render_Software, "Output vertex: pos (%.2f, %.2f, %.2f, %.2f), col(%.2f, %.2f, %.2f, %.2f), tc0(%.2f, %.2f)",
        ret.pos.x.ToFloat32(), ret.pos.y.ToFloat32(), ret.pos.z.ToFloat32(), ret.pos.w.ToFloat32(),
        ret.color.x.ToFloat32(), ret.color.y.ToFloat32(), ret.color.z.ToFloat32(), ret.color.w.ToFloat32(),
        ret.tc0.u().ToFloat32(), ret.tc0.v().ToFloat32());

    return ret;
}

std::unique_ptr<ShaderEngine> CreateInterpreter() {
    return std::unique_ptr<ShaderEngine> { new ShaderInterpreter() };
}

} // namespace

} // namespace
