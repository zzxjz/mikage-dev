#pragma once

#include "arm.h"
#include "memory.h"

#include <spdlog/sinks/sink.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <fstream>

// TODO: Shouldn't need to include this header...
#include "loader/gamecard.hpp"

namespace HLE {
namespace OS {
class OS;
}
}

namespace Profiler {
class Profiler;
}

namespace Debugger {
class DebugServer;
}

struct KeyDatabase;

class LogManager;

namespace Interpreter {

struct Breakpoint {
    uint32_t address;

    bool operator==(const Breakpoint& bp) const {
        return address == bp.address;
    }
};

struct AttachInfo {
    uint32_t pid;
};

struct WrappedAttachInfo {
    // Waits until some other thread has successfully signalled request_continue via TryAccess
    void AwaitProcessing() {
        std::unique_lock<std::mutex> lock(condvar_mutex);
        request_continue.wait(lock);
    }

    std::mutex condvar_mutex;
    std::unique_ptr<AttachInfo> data;

private:
    std::mutex access_mutex;
    std::condition_variable request_continue;

    friend bool TryAccess(std::weak_ptr<WrappedAttachInfo> attach_info_weak, std::function<bool(const AttachInfo&)> eval);
};

/**
 * Tries to unwrap the AttachInfo data stored in the given weak_ptr and passes
 * it to the given callback on success.
 * @param eval Callback that should return true if the AttachInfo should be
 *             permanently consumed (allowing the main thread to continue)
 * @return true if and only if the AttachInfo was consumed
 * @todo Define this elsewhere
 */
inline bool TryAccess(std::weak_ptr<WrappedAttachInfo> attach_info_weak, std::function<bool(const AttachInfo&)> eval) {
    auto attach_info_shared = attach_info_weak.lock();
    if (!attach_info_shared)
        return false;

    std::lock_guard<std::mutex> guard(attach_info_shared->access_mutex);

    auto* attach_info = attach_info_shared->data.get();
    if (!attach_info)
        return false;

    if (!eval(*attach_info))
        return false;

    // Drop the original data and signal the main thread to continue
    // Note that we lock the mutex here purely to make sure we don't signal the condition variable before it is being waited for
    attach_info_shared->data = nullptr;
    std::lock_guard<std::mutex> lock(attach_info_shared->condvar_mutex);
    attach_info_shared->request_continue.notify_all();
    return true;
}

// Controls thread-safe stepping/running/pausing/quitting of a Processor
class ProcessorController {
    // Handlers called from ProcessEventQueue when a segfault has been reported
    virtual void OnSegfault(uint32_t process_id, uint32_t thread_id) = 0;
    virtual void OnBreakpoint(uint32_t process_id, uint32_t thread_id) = 0;
    virtual void OnProcessLaunched(uint32_t process_id, uint32_t thread_id, uint32_t launched_process_id) = 0;
    virtual void OnReadWatchpoint(uint32_t process_id, uint32_t thread_id, uint32_t data_address) = 0;
    virtual void OnWriteWatchpoint(uint32_t process_id, uint32_t thread_id, uint32_t data_address) = 0;

protected:
public: // TODO: Un-public-ize!
    std::atomic<bool> request_pause{false};
//    std::atomic<bool> paused{true};
    std::atomic<bool> paused{false};
    std::atomic<bool> request_continue{false};

protected:
    /**
     * If valid, this denotes the process-thread-id of the thread that should
     * be paused. If boost::any is used for the thread-id (second element), any
     * thread of the given process is paused.
     */
    std::optional<std::pair<uint32_t, std::optional<uint32_t>>> thread_to_pause;

    friend class Processor;

    struct Event {
        enum class Type {
            Segfault,
            Breakpoint,
            ProcessLaunched,
            ReadWatchpoint,
            WriteWatchpoint,
        } type;

        uint32_t process_id;
        uint32_t thread_id;

        union {
            uint32_t launched_process_id;
            uint32_t watchpoint_data_address;
        };
    };

    std::list<Event> events;

    void ProcessEventQueue() {
        while (!events.empty()) {
            auto& event = events.front();
            switch (event.type) {
            case Event::Type::Segfault:
                OnSegfault(event.process_id, event.thread_id);
                break;

            case Event::Type::Breakpoint:
                OnBreakpoint(event.process_id, event.thread_id);
                break;

            case Event::Type::ProcessLaunched:
                OnProcessLaunched(event.process_id, event.thread_id, event.launched_process_id);
                break;

            case Event::Type::ReadWatchpoint:
                OnReadWatchpoint(event.process_id, event.thread_id, event.watchpoint_data_address);
                break;

            case Event::Type::WriteWatchpoint:
                OnWriteWatchpoint(event.process_id, event.thread_id, event.watchpoint_data_address);
                break;

            default:
                // Do nothing
                ;
            }

            events.pop_front();
        }
    }

public:
    virtual ~ProcessorController() = default;

    // TODO: This is probably a mess and full of race conditions... :(

    // TODO: Need functions to signal quit requests

    /**
     * Request the emulation core to be paused and busy-waits until the request has been fulfilled.
     * @note The amount of CPU instructions processed between the request of the pause and the actual pausing is undefined.
     */
    void RequestPause() {
        // TODO: Get rid of this! Currently, we need it in case automated stepping encounters a breakpoint..
        if (paused)
            return;
        assert(!paused);

        thread_to_pause = std::nullopt;
        request_pause = true;

        while (!paused) {
        }

        request_pause = false;
        assert(paused);
    }

    // Request emulation to be continued without any planned pause in the future. Does not return before emulation has continued.
    void RequestContinue() {
        assert(paused);

        request_continue = true;
        while (paused) {};
        request_continue = false;

        assert(!paused);
    }

    // Request emulation to be continued and pause after the next instruction. Does not return before emulation has continued and stopped again.
    void RequestStep(std::optional<std::pair<uint32_t, std::optional<uint32_t>>> process_thread_id = std::nullopt) {
        assert(paused);

        thread_to_pause = process_thread_id;
        request_pause = true;
        request_continue = true;
        while (paused) {};
        request_continue = false;
        while (!paused) {};
        request_pause = false;

        assert(paused);
    }

    bool ShouldPause(uint32_t process_id, uint32_t thread_id) const {
        if (!request_pause)
            return false;

        if (thread_to_pause == std::nullopt)
            return true;

        if (thread_to_pause->first != process_id)
            return false;

        return thread_to_pause->second.value_or(thread_id) == thread_id;
    }

    // Use this in the Processor when an internal (unrecoverable) error happens
    void NotifySegfault(uint32_t process_id, uint32_t thread_id) {
        events.push_back({Event::Type::Segfault, process_id, thread_id});
    }

    void NotifyBreakpoint(uint32_t process_id, uint32_t thread_id) {
        events.push_back({Event::Type::Breakpoint, process_id, thread_id});
    }

    void NotifyReadWatchpoint(uint32_t process_id, uint32_t thread_id, uint32_t address) {
        events.push_back({Event::Type::ReadWatchpoint, process_id, thread_id, address});
    }

    void NotifyWriteWatchpoint(uint32_t process_id, uint32_t thread_id, uint32_t address) {
        events.push_back({Event::Type::WriteWatchpoint, process_id, thread_id, address});
    }

    void NotifyProcessLaunched(uint32_t process_id, uint32_t thread_id, uint32_t launched_process_id) {
        events.push_back({Event::Type::ProcessLaunched, process_id, thread_id, launched_process_id});
    }

    virtual void OfferAttach(std::weak_ptr<WrappedAttachInfo> attach_info) = 0;
};

/// Trivial ProcessorController with debugging features disabled
class DummyController final : public ProcessorController {
    void OnSegfault(uint32_t process_id, uint32_t thread_id) override {
    }

    void OnBreakpoint(uint32_t process_id, uint32_t thread_id) override {
    }

    void OnReadWatchpoint(uint32_t process_id, uint32_t thread_id, uint32_t data_address) override {
    }

    void OnWriteWatchpoint(uint32_t process_id, uint32_t thread_id, uint32_t data_address) override {
    }

    void OnProcessLaunched(uint32_t process_id, uint32_t thread_id, uint32_t launched_process_id) override {
    }

    void OfferAttach(std::weak_ptr<WrappedAttachInfo> attach_info) override {
    }
};

}  // namespace Interpreter

namespace Interpreter {

struct Callsite {
    uint32_t source;
    uint32_t target;

    ARM::State state;
};

struct Setup;

struct CPUContext;

// #define CONTROL_FLOW_LOGGING
struct ControlFlowLogger {
    uint32_t indent = 0;

    void Branch(CPUContext& ctx, const char* kind, uint32_t addr);

    void Return(CPUContext& ctx, const char* kind);

    void SVC(CPUContext& ctx, uint32_t id);

    void Log(CPUContext& ctx, const std::string& str);

private:
    std::string GetIndent();
    void MakeSureFileIsOpen(CPUContext& ctx);

#ifdef CONTROL_FLOW_LOGGING
    std::ofstream os;
#endif
};

// TODO: Move to private implementation
struct CPUContext {
    CPUContext(HLE::OS::OS* os = nullptr, Setup* setup = nullptr);
    ~CPUContext();

    ARM::State cpu{};

    std::list<Breakpoint> breakpoints;
    std::list<Breakpoint> read_watchpoints;
    std::list<Breakpoint> write_watchpoints;

    std::vector<Callsite> backtrace;

    // This must be true for any of the debugging functionality to be enabled
    bool debugger_attached = false;

    bool trap_on_resume = false;

    ControlFlowLogger cfl{};

    void RecordCall(uint32_t source, uint32_t target, ARM::State state);

    HLE::OS::OS* os{};

    Setup* setup{};

    ProcessorController* controller{};
};

// TODO: This should be somewhere stored in its own header! (need to provide external default destructor in Processor to enable that
// TODO: Rename to Emulator (and strip the CPUContext; they belong in the Processors' ExecutionContext instead, and those are created by OS)
struct Setup {
    Setup(LogManager&, const KeyDatabase&,
          std::unique_ptr<Loader::GameCard>, Profiler::Profiler&,
          Debugger::DebugServer&);
   ~Setup();

    CPUContext cpus[2];

    Memory::PhysicalMemory mem;

    std::unique_ptr<HLE::OS::OS> os;

    const KeyDatabase& keydb;

    std::unique_ptr<Loader::GameCard> gamecard = nullptr;

    Profiler::Profiler& profiler;

    Debugger::DebugServer& debug_server;
};

// TODO: Don't leak this implementation detail
void Step(struct ExecutionContext&); // NOTE: This is only for the Interpreter

class Processor;

struct ExecutionContext {
    Processor& parent;

    ExecutionContext(Processor& parent);

    ExecutionContext(const ExecutionContext&) = delete;
    ExecutionContext& operator=(const ExecutionContext&) = delete;
    ExecutionContext(const ExecutionContext&&) = delete;
    ExecutionContext& operator=(ExecutionContext&&) = delete;

    // Automatically unregisters this context from its parent Processor
    virtual ~ExecutionContext();

    // TODO: Provide generic accessors instead
    virtual ARM::State ToGenericContext() = 0;
    virtual void FromGenericContext(const ARM::State&) = 0;

    /**
     * Enables ProcessorController to take control over the thread corresponding to this context.
     */
    virtual void SetDebuggingEnabled(bool enabled = true) { (void)enabled; }

    virtual bool IsDebuggingEnabled() const { return false; }
};

class Processor {
    friend struct ExecutionContext;

    virtual ExecutionContext* CreateExecutionContextImpl() = 0;

    void UnregisterContext(ExecutionContext&);

protected:
    std::vector<ExecutionContext*> contexts;

public:
    virtual ~Processor() = default;

    virtual void WriteVirtualMemory8(uint32_t virt_address, const uint8_t value) = 0;
    virtual void WriteVirtualMemory16(uint32_t virt_address, const uint16_t value) = 0;
    virtual void WriteVirtualMemory32(uint32_t virt_address, const uint32_t value) = 0;

    virtual uint8_t ReadVirtualMemory8(uint32_t virt_address) = 0;
    virtual uint16_t ReadVirtualMemory16(uint32_t virt_address) = 0;
    virtual uint32_t ReadVirtualMemory32(uint32_t virt_address) = 0;

    // Runs until shut down by controller
    virtual void Run(ExecutionContext&, ProcessorController& controller, uint32_t process_id, uint32_t thread_id) = 0;

    virtual void OnVirtualMemoryMapped(uint32_t phys_addr, uint32_t size, uint32_t vaddr) = 0;

    virtual void OnVirtualMemoryUnmapped(uint32_t vaddr, uint32_t size) = 0;


    std::unique_ptr<ExecutionContext> CreateExecutionContext() {
        return std::unique_ptr<ExecutionContext> { CreateExecutionContextImpl() };
    }
};

std::unique_ptr<Processor> CreateInterpreter(Setup& setup);

}  // namespace Interpreter
