#pragma once

#include <debug_server.hpp>

#include <mutex>
#include <unordered_map>

namespace HLE::OS {
class OS;
class Process;
class Thread;
using ProcessId = uint32_t;
using ThreadId = uint32_t;
}

namespace Debugger {

struct OSService : Service {
    struct ProcessContext {
        HLE::OS::Process* process;
        std::unordered_map<HLE::OS::ThreadId, HLE::OS::Thread*> threads;
    };

    std::unordered_map<HLE::OS::ProcessId, ProcessContext> processes;
    std::mutex access_mutex;

    void RegisterProcess(HLE::OS::ProcessId, HLE::OS::Process&);
    void UnregisterProcess(HLE::OS::ProcessId);

    void RegisterThread(HLE::OS::ProcessId, HLE::OS::ThreadId, HLE::OS::Thread&);
    void UnregisterThread(HLE::OS::ProcessId, HLE::OS::ThreadId);

    void Shutdown();

    void RegisterRoutes(Pistache::Rest::Router&) override;
};

} // namespace Debugger
