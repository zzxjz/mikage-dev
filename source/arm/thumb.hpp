#pragma once

#include "../arm.h"

#include <optional>

namespace ARM {

struct DecodedThumbInstr {
    // Equivalent ARM instruction, if any
    std::optional<ARM::ARMInstr> arm_equivalent;
    bool may_read_pc = false;
    bool may_modify_pc = false;

    DecodedThumbInstr& SetMayReadPC() {
        may_read_pc = true;
        return *this;
    }

    DecodedThumbInstr& SetMayModifyPC() {
        may_modify_pc = true;
        return *this;
    }
};

/**
 * Translates the given THUMB instruction to an equivalent ARM encoding.
 * If this is not possible, the caller is responsible for interpreting
 * the instruction manually.
 */
DecodedThumbInstr DecodeThumb(ARM::ThumbInstr);

} // namespace ARM
