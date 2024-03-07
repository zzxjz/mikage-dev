#pragma once

#include "processor_default.hpp"

#include <boost/coroutine2/coroutine.hpp>

namespace Interpreter {

// TODO: Instead of inhering CPUContext, integrate CPUContext into this class!
struct InterpreterExecutionContext : ExecutionContextWithDefaultMemory, CPUContext {
    InterpreterExecutionContext(Processor& parent_, Setup& setup)
        : ExecutionContextWithDefaultMemory(parent_, setup.mem), CPUContext(setup.os.get(), &setup) {

        // TODO: Setup virtual memory mappings
    }

    ARM::State ToGenericContext() override {
        return cpu;
    }

    void FromGenericContext(const ARM::State& state) override {
        // TODO: Don't use memcpy for copying?
        std::memcpy(&cpu, &state, sizeof(state));
    }

    std::optional<uint32_t> TranslateVirtualAddress(uint32_t address) {
        return ExecutionContextWithDefaultMemory::TranslateVirtualAddress(address);
    }

    void SetDebuggingEnabled(bool enabled = true) override {
        debugger_attached = enabled;
    }

    bool IsDebuggingEnabled() const override {
        return debugger_attached;
    }

    // Optional reference to coroutine
    // TODO: Fix for HostThreadBasedThreadControl
    boost::coroutines2::coroutine<uint32_t>::push_type* coro = nullptr;

    // Tagged address for LDREX/STREX.
    // We perform an implicit CLREX on context switches, so this mainly ensures
    // that the STREX following a LDREX uses a matching address.
    std::optional<uint32_t> monitor_address;
};

} // namespace Interpreter
