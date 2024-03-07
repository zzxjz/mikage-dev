#include "context.h"
#include "shader.hpp"
#include "shader_analysis.hpp"

#include <framework/exceptions.hpp>
#include <framework/meta_tools.hpp>

#include <nihstro/shader_bytecode.h>

#include <range/v3/algorithm/all_of.hpp>
#include <range/v3/algorithm/find_if.hpp>
#include <range/v3/algorithm/upper_bound.hpp>
#include <range/v3/view/cache1.hpp>
#include <range/v3/view/counted.hpp>
#include <range/v3/view/intersperse.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/join.hpp>
#include <range/v3/view/map.hpp>
#include <range/v3/view/remove_if.hpp>
#include <range/v3/view/transform.hpp>
#include <range/v3/range/conversion.hpp>

#include <bitset>

using nihstro::OpCode;
using nihstro::Instruction;
using nihstro::RegisterType;
using nihstro::SourceRegister;
using nihstro::SwizzlePattern;

namespace Pica::VertexShader {

template<typename Rng>
static void AnalyzeShaderInternal(const Rng& shader_memory, std::bitset<16> bool_uniforms, AnalyzedProgram& analyzed_program, std::set<ShaderCodeOffset> dirty_blocks) {
    using SuccessionVia = AnalyzedProgram::SuccessionVia;

    std::map<ShaderCodeOffset, AnalyzedProgram::BlockInfo>& blocks = analyzed_program.blocks;

    while (!dirty_blocks.empty()) {
        const ShaderCodeOffset block_start = *dirty_blocks.begin();
        dirty_blocks.erase(dirty_blocks.begin());
        ShaderCodeOffset offset = block_start;
        auto& block_info = blocks.at(offset);
        auto& current_block_length = block_info.length;
        current_block_length = 0;

        auto add_call = [&](ShaderCodeOffset function_entry, ShaderCodeOffset function_exit) {
            auto block_offsets = blocks | ranges::views::keys;

            // Check if the called instruction range starts within an already existing block
            auto it = ranges::upper_bound(block_offsets, function_entry, [](auto a, auto b) { return !(a < b || a == b); });
            if (it != ranges::end(block_offsets) && !(*it < function_entry)) {
                // TODO: Remove. This appears to just be a sanity check to ensure we got the upper_bound right...
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
//                fmt::print("Registering block for return site {:#x}\n", 4 * return_site.value);
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
//                fmt::print("Registering block for return target {:#x}\n", 4 * return_target.value);
            }
            if (is_conditional) {
                target_block_it->second.predecessors.insert(return_site);
            }
        };

//        fmt::print("Processing block at {:#x}\n", 4 * block_start.value);

        bool end_block = false;
        while (!end_block) {
            const Instruction instr = offset.ReadFrom(shader_memory);

            if (offset != block_start && blocks.count(offset)) {
                // Fall through into an existing block.
                // This happens e.g. for the end of branch bodies, which is
                // registered as a single-instruction block to register the
                // post-branch address
                end_block = true;
                add_successor(offset, SuccessionVia::Fallthrough);
//                fmt::print("Encountered block at {:#x} => closing current block\n", 4 * offset.value);
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
                    // NOTE: Apart from b15, these conditions can be evaluated statically!
                    // NOTE: The lowest bit of the num_instructions field indicates an inverted condition for this instruction.
                    // TODO: Add a better-named field alias for this case
                    // TODO: Handle uniform b15 (geometry shader)
                    if (bool_uniforms[instr.flow_control.bool_uniform_id] == !(instr.flow_control.num_instructions & 1)) {
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
                    // NOTE: For CALLU, the condition can be evaluated statically
                    // TODO: Even for IFU, the condition cannot be evaluated statically for uniform b15 (geometry shader)
                    if (instr.opcode.Value() != OpCode::Id::CALLU || bool_uniforms[instr.flow_control.bool_uniform_id]) {
                        auto return_site = target.IncrementedBy(instr.flow_control.num_instructions);
                        add_return_site(return_site.GetPrevious(), offset.GetNext(), false);
                        add_call(target, return_site.GetPrevious());
//                        fmt::print("CALL at {:#x} => adding block bounds at {:#x}-{:#x}\n", 4 * offset.value, 4 * target.value, 4 * return_site.value);
                    } else if (instr.opcode.Value() == OpCode::Id::CALLU && !bool_uniforms[instr.flow_control.bool_uniform_id]) {
//                        fmt::print("Skipping CALLU instruction at {:#x} due to statically false branch\n", 4 * offset.value);
                    }


                    add_successor(offset.GetNext(), SuccessionVia::Fallthrough);

                    end_block = true;
                    break;
                }

                case OpCode::Id::IFU:
                case OpCode::Id::IFC:
                {
                    ++current_block_length;
                    end_block = true;

                    auto else_offset = ShaderCodeOffset { instr.flow_control.dest_offset };
                    auto branch_end = else_offset.IncrementedBy(instr.flow_control.num_instructions);

                    // NOTE: For IFU, the condition can be evaluated statically
                    // TODO: Even for IFU, the condition cannot be evaluated statically for uniform b15 (geometry shader)
                    // "If" case
                    if (instr.opcode.Value() != OpCode::Id::IFU || bool_uniforms[instr.flow_control.bool_uniform_id]) {
                        add_return_site(else_offset.GetPrevious(), branch_end, true);
                        add_successor(offset.GetNext(), SuccessionVia::BranchToIf);
                    }

                    // "else" case (if non-empty)
                    if (instr.opcode.Value() != OpCode::Id::IFU || !bool_uniforms[instr.flow_control.bool_uniform_id]) {
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
                    throw std::runtime_error("TODO: Implement BREAKC");

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
//        fmt::print("Block at {:#x}-{:#x} finalized\n", 4 * block_start.value, 4 * block_start.value + 4 * block_info.length - 1);
    }
}

// TODO: Optimization passes:
// * Inline single-instruction "Goto" blocks with one possible successor into call instructions that refer to that block's offset
// * Merge single predecessor blocks
// * Turn "CheckCallStack" into "PopCallStack" if there's only a single path from CALL to the current instruction
// * Turn CheckCallStack at end of if-block into static jump to post-branch

// Optimize CFG: Merge adjacent blocks with linear connection together
static void MergeLinearBlockChains(AnalyzedProgram& program, const uint32_t* shader_memory) {
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
    for (auto block_iter = program.blocks.begin(), next_block_iter = std::next(block_iter); block_iter != program.blocks.end(); block_iter = next_block_iter) {
        if (next_block_iter != program.blocks.end()) {
            ++next_block_iter;
        }

        auto& block = *block_iter; // Node B
        if (block.second.predecessors.size() == 1) {
            const auto pred_block_offset = *block.second.predecessors.begin();
            auto& pred_block = program.blocks.at(pred_block_offset); // Node A (will merge B into this to get A')
            if (pred_block.successors.size() != 1 || pred_block_offset.IncrementedBy(pred_block.length) != block.first) {
                continue;
            }

            // For CALL instructions, we need a mapping between PICA200
            // offsets and micro op offsets. For simplicity, we ensure this
            // by avoiding merging blocks for now.
            auto last_pred_instr = nihstro::Instruction { pred_block_offset.IncrementedBy(pred_block.length - 1).ReadFrom(shader_memory) };
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
                auto& successor = program.blocks.at(successor_offset.first);
                successor.predecessors.erase(block.first);
                auto [it, inserted] = successor.predecessors.insert(pred_block_offset);
                ValidateContract(inserted);
            }

            // Repoint successor references in the predecessor (A) from B to B's successors
            pred_block.successors.erase(block.first);
            pred_block.successors.merge(block.second.successors);
            pred_block.length += block.second.length;

            next_block_iter = program.blocks.erase(block_iter);
        }
    }
}

static void DetectFunctions(AnalyzedProgram& program) {
    auto& functions = program.functions;

    // TODO: Ensure there are no functions with duplicate entry point
    if (functions.size() > 1) {
        // pick any. TODO: Workaround for Rayman Origins
        functions.resize(1);
    }

    // First, find all blocks to functions and append their callees to the function list
    for (auto& [function_entry, call_info] : program.call_targets) {
        auto function_it = ranges::find_if(functions, [entry=function_entry](auto& f) { return f.entry == entry; });
        if (function_it == functions.end()) {
//            fmt::print("Adding function {:#x}-{:#x}\n", 4 * function_entry.value, 4 * call_info.return_site.value);
            functions.push_back({ function_entry, call_info.return_site, {} });
        } else if (function_it->exit != call_info.return_site) {
            throw std::runtime_error("A function at this offset already exists with different parameters");
        }
    }

    // Second, do a depth-first search to verify all paths from the function entry converge at its exit
    auto check_convergence = [&](ShaderCodeOffset start, ShaderCodeOffset end, std::set<ShaderCodeOffset>& function_blocks, const auto& recurse) -> bool {
        // Traverse linear chain of blocks
        auto is_same_block = [&program](ShaderCodeOffset block_start, ShaderCodeOffset b) {
            auto block_end = block_start.IncrementedBy(program.blocks.at(block_start).length);
            return !(b < block_start) && (b < block_end);
        };
        for (; !is_same_block(start, end);) {
            auto& block = program.blocks.at(start);
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
        auto& block = program.blocks.at(start);

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

//    fmt::print("FUNCTIONS:\n");
    for (AnalyzedProgram::Function& func : functions) {
        if (!check_convergence(func.entry, func.exit, func.blocks, check_convergence)) {
//            throw std::runtime_error("Function execution paths do not converge");
        }

//        fmt::print("function {:#x}-{:#x}\n", 4 * func.entry.value, 4 * func.exit.value);
    }
}

static AnalyzedProgram AnalyzeShaderInternal(   const uint32_t* shader_instructions, uint32_t num_instructions,
                                                uint16_t bool_uniforms_raw, ShaderCodeOffset entry_point) {
    AnalyzedProgram analyzed_program;

    analyzed_program.blocks[entry_point] = { };
    std::set<ShaderCodeOffset> dirty_blocks { entry_point };
    analyzed_program.entry_point = entry_point;

    std::bitset<16> bool_uniforms(bool_uniforms_raw);
    AnalyzeShaderInternal(  ranges::views::counted(shader_instructions, num_instructions),
                            bool_uniforms, analyzed_program, dirty_blocks);

    // Sanity check: Ensure blocks are non-overlapping
    for (auto block_iter : ranges::views::iota(analyzed_program.blocks.begin(), analyzed_program.blocks.end())) {
        if (block_iter == std::prev(analyzed_program.blocks.end())) {
            break;
        }

        auto& block = *block_iter;
        auto& next_start = std::next(block_iter)->first;
        if (next_start < block.first.IncrementedBy(block.second.length)) {
            throw std::runtime_error(fmt::format("Blocks are overlapping: {:#x}-{:#x}, {:#x}", block_iter->first.value * 4, block.first.IncrementedBy(block.second.length).value * 4, next_start.value * 4));
        }
    }

    MergeLinearBlockChains(analyzed_program, shader_instructions);

    auto ComputePostDominator = [&analyzed_program](ShaderCodeOffset block_offset, const auto& recurse) -> ShaderCodeOffset {
        // Since each CFG node has at most two predecessors/successors, the
        // post-dominator can be uniquely derived from any single path.
        // We just pick the first one, and validate later to ensure it's
        // consistent with the other paths.
        auto& block = analyzed_program.blocks.at(block_offset);
        auto successor_offset = block.successors.begin()->first;

        if (block.successors.size() == 1) {
            // Not a conditional, hence nothing to do
            return successor_offset;
        }

        // If there is more than one successor, it must be a conditional
        // (and hence there must be exactly 2 successors)
        ValidateContract(block.successors.size() == 2);

        // Depth-first search to find prospective post-dominator
        bool followed_postdom = false;
        while (true) {
            auto& successor_info = analyzed_program.blocks.at(successor_offset);

            // Having two predecessors indicates this is a convergence point
            // of two branch paths: This could either be the one we just
            // skipped over (since it was a nested branch), or it's the one
            // from the original block
            if (!followed_postdom && successor_info.predecessors.size() == 2) {
                return successor_offset;
            }
            followed_postdom = false;

            if (successor_info.successors.size() == 2) {
                // Nested conditional. Compute post-dominators recursively and then skip past this branch
                successor_offset = recurse(successor_offset, recurse);
                followed_postdom = true;
            } else {
                ValidateContract(successor_info.successors.size() == 1);
                successor_offset = successor_info.successors.begin()->first;
            }
        }
    };

    for (auto& block : analyzed_program.blocks) {
        // Skip return sites
        auto is_call_stack_pop = [](AnalyzedProgram::SuccessionVia reason) { return reason == AnalyzedProgram::SuccessionVia::Return; };
        if (ranges::all_of(block.second.successors | ranges::views::values, is_call_stack_pop)) {
            continue;
        }

        ValidateContract(block.second.successors.size() == 1 || block.second.successors.size() == 2);
        if (block.second.successors.size() == 1) {
            block.second.post_dominator = block.second.successors.begin()->first;
        } else if (block.second.successors.size() == 2) {
            block.second.post_dominator = ComputePostDominator(block.first, ComputePostDominator);
            fmt::print(stderr, "Found post-dominator {:#x} for {:#x}\n", 4 * block.second.post_dominator.value, 4 * block.first.value);
        }
    }

    auto ValidatePostDominator = [&analyzed_program](ShaderCodeOffset from) {
        const auto& block = analyzed_program.blocks.at(from);
        auto to = block.post_dominator;

        // This holds the intersection of all paths from the block start to
        // its alleged post-dominator (excluding those two bounding nodes).
        // If this intersection is non-empty, there is a node that is reached
        // by all execution paths (i.e. our post-dominator is not correct).
        std::set<ShaderCodeOffset> immdom_candidates;
        std::set<ShaderCodeOffset> immdom_candidates2;

        bool first_path = true;
        for (auto& succ : block.successors) {
            auto node_offset = succ.first;

            while (node_offset != to) {
                if (first_path || immdom_candidates.find(node_offset) != immdom_candidates.end()) {
                    immdom_candidates2.insert(node_offset);
                }

                // Skip to the next post-dominator (which is validated separately)
                node_offset = analyzed_program.blocks.at(node_offset).post_dominator;
                if (node_offset == ShaderCodeOffset { 0xffffffff }) {
                    throw std::runtime_error(fmt::format(   "Validation of post-dominator for block at {:#x} failed: Candidate not reachable from the node it was supposed to post-dominate",
                                                            4 * from.value));
                }
            }
            first_path = false;
            std::swap(immdom_candidates, immdom_candidates2);
        }

        if (!immdom_candidates.empty()) {
            throw std::runtime_error("Validation of post-dominator relations failed: Another post-dominator exists that pre-dominates the candidate\n");
        }
    };

    for (auto& block : analyzed_program.blocks) {
        ValidatePostDominator(block.first);
    }

    DetectFunctions(analyzed_program);

    return analyzed_program;
}

AnalyzedProgramPimpl::AnalyzedProgramPimpl() = default;
AnalyzedProgramPimpl::AnalyzedProgramPimpl(AnalyzedProgramPimpl&& other) = default;
AnalyzedProgramPimpl& AnalyzedProgramPimpl::operator=(AnalyzedProgramPimpl&& other) = default;
AnalyzedProgramPimpl::~AnalyzedProgramPimpl() = default;


AnalyzedProgramPimpl::AnalyzedProgramPimpl(AnalyzedProgram&& program_)
    : program(std::make_unique<AnalyzedProgram>(std::move(program_))) {
}

AnalyzedProgramPimpl AnalyzeShader( const uint32_t* shader_instructions, uint32_t num_instructions,
                                    uint16_t bool_uniforms, uint32_t entry_point) {
    return AnalyzedProgramPimpl {
        AnalyzeShaderInternal(  shader_instructions,
                                num_instructions,
                                bool_uniforms,
                                ShaderCodeOffset { entry_point })
    };
}

std::string VisualizeShaderControlFlowGraph(
        const AnalyzedProgram& program,
        const uint32_t* shader_instructions, uint32_t num_instructions,
        const uint32_t* swizzle_data, uint32_t /*num_swizzle_slots*/) {
    std::string dot = "digraph {\n";

    std::set<ShaderCodeOffset> function_blocks;

// TODO: Print entry point node
    auto instructions = ranges::views::counted(shader_instructions, num_instructions);

    auto print_block = [&](ShaderCodeOffset pica_start, uint32_t length, bool print_block_offset) {
        auto block_offsets = ranges::views::iota(pica_start, pica_start.IncrementedBy(length));
        auto disassemble_instr = [&](ShaderCodeOffset offset) {
            return fmt::format("{:#05x}: {}", 4 * offset.value, DisassembleShader(offset.ReadFrom(instructions).hex, swizzle_data));
        };
        auto block_disassembly = block_offsets | ranges::views::transform(disassemble_instr) | ranges::views::intersperse("\\\\l") | ranges::views::cache1 | ranges::views::join | ranges::to<std::string>;

        return fmt::format( R"(  block{:#x}[fontname="Arial" shape=box label="{}\\l"])",
                            4 * pica_start.value,
                            block_disassembly) + '\n';
    };

    for (auto& function : program.functions) {
        dot += fmt::format("\n subgraph cluster_{:#x} {{\n", 4 * function.entry.value);
        dot += fmt::format("label = \"Function at {:#x}\"; \n\n", 4 * function.entry.value);
        for (auto& block_start : function.blocks) {
            dot += print_block( block_start,
                                program.blocks.at(block_start).length,
                                block_start == function.entry);
            function_blocks.insert(block_start);
        }
        dot += "}\n\n";
    }

    for (auto& block : program.blocks) {
        if (!function_blocks.count(block.first)) {
            dot += print_block(block.first, block.second.length, false);
        }
    }

    dot += '\n';

    for (auto& block : program.blocks) {
        for (auto& successor : block.second.successors) {
            dot += fmt::format( "  block{:#x} -> block{:#x}",
                                4 * block.first.value,
                                4 * successor.first.value);

            using SuccessionVia = AnalyzedProgram::SuccessionVia;
            switch (successor.second) {
                case SuccessionVia::Return:
                    dot += " [style=dotted]";
                    break;

                case SuccessionVia::BranchToIf:
                    dot += " [color=green]";
                    break;

                case SuccessionVia::BranchToElse:
                    dot += " [color=red]";
                    break;

                default:
                    ;
            }
            dot += '\n';
        }
    }

    dot += "}\n";

    return dot;
}

} // namespace Pica::VertexShader
