#pragma once

#include "shader_private.hpp"

#include <range/v3/view/counted.hpp>

#include <set>
#include <optional>
#include <variant>
#include <vector>

namespace Pica::VertexShader {

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
    // TODO: Add length!
};

} // namespace Pica::VertexShader
