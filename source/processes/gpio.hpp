#pragma once

#include "fake_process.hpp"

#include <memory>

namespace HLE {

namespace OS {

struct Handle;

class FakeGPIO final {
    OS& os;
    spdlog::logger& logger;

public:
    FakeGPIO(FakeThread& thread);

    void GPIOThread(FakeThread& thread, const char* service_name, uint32_t interrupt_mask);

    void CommandHandler(FakeThread& thread, const IPC::CommandHeader& header, uint32_t interrupt_mask);

    OS::ResultAnd<> HandleBindInterrupt(FakeThread& thread, uint32_t service_interrupt_mask, uint32_t interrupt_mask,
                                        uint32_t priority, Handle event);
};

}  // namespace OS

}  // namespace HLE
