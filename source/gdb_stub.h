#pragma once

#include "interpreter.h"

#include <spdlog/common.h>
#include <spdlog/logger.h>

#include <memory>
#include <set>
#include <string>
#include <unordered_map>

class LogManager;

namespace HLE {
namespace OS {
class OS;
class Thread;
class Process;

using ProcessId = uint32_t;
using ThreadId = uint32_t;

}
}

namespace Interpreter {

class GDBStubTCPServer;
class GDBStub final : public ProcessorController {
    friend class GDBStubTCPServer;

    // We refer to the "OS" more abstractly as env(ironment) here.
    // In the future, we may support LLE kernel emulation, and as such
    // may only have one thread remaining to debug. To simplify design of
    // this stub interface, we might still want to keep around a virtual
    // OS layer around at that point, though.
    HLE::OS::OS& env;

    std::unique_ptr<GDBStubTCPServer> server;

    std::unordered_map<char, std::pair<HLE::OS::ProcessId,HLE::OS::ThreadId>> thread_for_operation;

    std::shared_ptr<spdlog::logger> logger;

    std::vector<HLE::OS::ProcessId> attached_processes;

    // Information about a process that is waiting for a debugger to attach
    std::weak_ptr<WrappedAttachInfo> attach_info;

    /**
     * Handle an incoming GDB command
     *
     * Returns an optional response to send to GDB.
     */
    std::optional<std::string> HandlePacket(const std::string& command);

    void Continue(bool single_step = false);

    /**
     * Get CPU Id of the controlled CPU
     */
    unsigned GetCPUId();

    void OnSegfault(uint32_t process_id, uint32_t thread_id) override;
    void OnBreakpoint(uint32_t process_id, uint32_t thread_id) override;
    void OnReadWatchpoint(uint32_t process_id, uint32_t thread_id, uint32_t data_address) override;
    void OnWriteWatchpoint(uint32_t process_id, uint32_t thread_id, uint32_t data_address) override;
    void OnProcessLaunched(uint32_t process_id, uint32_t thread_id, uint32_t launched_process_id) override;

    // Environment inspection utility functions

    // Get currently active thread
    HLE::OS::Thread& GetCurrentThread() const;

    // Get currently active process
    HLE::OS::Process& GetCurrentProcess() const;

    // Get a list of threads running in the environment
    std::vector<std::shared_ptr<HLE::OS::Thread>> GetThreadList() const;

    std::shared_ptr<HLE::OS::Thread> GetThreadForOperation(char operation);

public:
    GDBStub(HLE::OS::OS& os, LogManager& log_manager, unsigned port);
    ~GDBStub();

    void OfferAttach(std::weak_ptr<WrappedAttachInfo> attach_info) override;

    void ProcessQueue();
};

} // namespace
