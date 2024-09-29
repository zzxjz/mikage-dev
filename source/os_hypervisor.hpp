#pragma once

#include <functional>
#include <memory>

struct AudioFrontend;

namespace Settings {
struct Settings;
}

namespace HLE {

namespace OS {

class Thread;
class Process;
class CodeSet;
struct Handle;

using ProcessId = uint32_t;

class Hypervisor;

namespace HPV {

class Session;

// TODO: Small-function optimization!
using IPCCallback = std::function<void(Hypervisor&, Thread&)>;

struct State;

} // namespace HPV

class Hypervisor {
    std::unique_ptr<HPV::State> state;

public:
    Hypervisor(Settings::Settings&, AudioFrontend&);
    ~Hypervisor();

    // "thread" refers to the target (i.e. server) thread
    void OnIPCRequestFromTo(Thread& from_thread, Thread &to_thread, Handle session_handle);

    // "thread" refers to the source (i.e. server) thread
    void OnIPCReplyFromTo(Thread& from_thread, Thread &to_thread, Handle session_handle);

    void OnPortCreated(ProcessId process, std::string_view port_name, Handle port_handle);

    // Registers the session handle as associated to the given named port.
    // @note Sessions are associated to *unnamed* ports in ServiceManager::GetServiceHandle via OnSessionNamed
    void OnConnectToPort(ProcessId process, std::string_view port_name, Handle session_handle);

    // Registers the session handle as associated to the given port
    void OnNewSession(ProcessId process, Handle port_handle, Handle session_handle);

    // Registers a session handle that is not associated with any port
    void OnSessionCreated(ProcessId process, Handle session_handle);

    // Will name a port indicated by the handle. Naming will propagate to all other handles to the same session
    void SetPortName(ProcessId process, Handle port_handle, std::string_view port_name);

    // Takes ownership of the given Session
    void SetSessionObject(ProcessId process, Handle session_handle, HPV::Session*);

    void OnHandleDuplicated(ProcessId source_process, Handle source_handle, ProcessId dest_process, Handle dest_handle);

    void OnHandleClosed(ProcessId, Handle);

    // TODO: Must be wired up internally for shadow services to subscribe to events
    void OnEventSignaled(Thread&, ProcessId, Handle event_handle);

    /**
     * Associates the object referenced by the given handle in the given thread
     * with a human-readable name
     */
    void SetObjectTag(Thread&, Handle, std::string name);
};

} // namespace OS

} // namespace HLE
