#pragma once

#include "fake_process.hpp"
#include "ipc.hpp"

#include <memory>

namespace HLE {

namespace OS {

struct Handle;

class FakePTM final {
    OS& os;
    spdlog::logger& logger;

public:
    FakePTM(FakeThread& thread);

    void IPCThread(FakeThread& thread, const char* service_name);
    void IPCThread_gets(FakeThread& thread, const char* service_name);

    void CommandHandler(FakeThread& thread, const IPC::CommandHeader& header);
    void CommandHandler_gets(FakeThread& thread, const IPC::CommandHeader& header);
};

}  // namespace OS

}  // namespace HLE

#pragma once
