#pragma once

#include "ipc.hpp"
#include "os.hpp"

#include <variant>

namespace HLE {

namespace OS {

// TODO: Deprecated
class FakePort : public FakeThread {
    const std::string name;

    // TODO: Implement functionality related to limiting the maximal number of sessions!
    uint32_t max_sessions;

    HandleTable::Entry<ServerPort> port;

    virtual HandleTable::Entry<ServerPort> Setup();

    virtual void OnIPCRequest(Handle sender, const IPC::CommandHeader& header) = 0;

protected:
    // Use this for internal logging
    virtual std::string GetInternalName() const;

    // Use this in OnIPCRequest implementations when encountering unknown/unimplemented requests
    void UnknownRequest(const IPC::CommandHeader& header);

    void LogStub(const IPC::CommandHeader& header);

    void LogReturnedHandle(Handle handle);

public:
    FakePort(FakeProcess& parent, const std::string& name, uint32_t max_sessions);

    virtual ~FakePort() = default;

    void Run() override;
};

/// Helper class for creating and using ServerPorts
class ServerPortUtilBase {
    // TODO: Implement functionality related to limiting the maximal number of sessions!
    uint32_t max_sessions;

    std::vector<Handle> handle_table;
    std::vector<std::shared_ptr<Object>> object_table; // Table to keep around references to the objects

    FakeThread& owning_thread;

    std::shared_ptr<Object> GetObjectBase(int32_t index) const;

protected:
    void AppendToHandleTable(Handle handle, std::shared_ptr<Object> object);

    // Use this in ServerPortUtil users when encountering unknown/unimplemented requests
    static void UnknownRequest(const IPC::CommandHeader& header);

    static void LogStub(const IPC::CommandHeader& header);

    static void LogReturnedHandle(Handle handle);

    /// Takes ownership of the given ServerPort handle
    ServerPortUtilBase(FakeThread& owning_thread, HandleTable::Entry<ServerPort> port, uint32_t max_sessions);

    ~ServerPortUtilBase();

public:
    OS::ResultAnd<int32_t> ReplyAndReceive(FakeThread& thread, Handle reply_target);
    OS::Result Reply(FakeThread& thread, Handle reply_target);
    OS::ResultAnd<int32_t> Receive(FakeThread& thread);

    /*
     * Accepts the incoming client connection using OS::SVCAcceptSession and
     * appends the port session to the handle table used for SVCReplyAndReceive
     */
    OS::ResultAnd<int32_t> AcceptSession(FakeThread& thread, int32_t index);

    template<typename Obj>
    std::shared_ptr<Obj> GetObject(int32_t index) const {
        return std::dynamic_pointer_cast<Obj>(GetObjectBase(index));
    }

    Handle GetHandle(int32_t index) const;
};

/// Helper class for creating and using ServerPorts
class ServerPortUtil final : public ServerPortUtilBase {
public:
    static HandleTable::Entry<ServerPort> SetupPort(FakeThread& thread, const std::string& name, uint32_t max_sessions);

    /// Creates an internal ServerPort kernel object
    ServerPortUtil(FakeThread& thread, const std::string& name, uint32_t max_sessions);
};

/// Helper class for creating and hosting services through the "srv:" port
class ServiceUtil final : public ServerPortUtilBase {
public:
    static HandleTable::Entry<ServerPort> SetupService(FakeThread& thread, const std::string& name, uint32_t max_sessions);

    /// Creates an internal ServerPort kernel object
    ServiceUtil(FakeThread& thread, const std::string& name, uint32_t max_sessions);
};

// TODO: Deprecated
class FakeService : public FakePort {
    const std::string name;

    uint32_t max_sessions;

    virtual HandleTable::Entry<ServerPort> Setup() override;

    /**
     * May be implemented by child classes to run initialization code once.
     * This function will be called in a context where it's safe to use system calls.
     */
    virtual void SetupService() {
    }

protected:
    virtual std::string GetInternalName() const override;

public:
    // Create a fake port with empty public name
    FakeService(FakeProcess& parent, const std::string& name, uint32_t max_sessions);

    virtual ~FakeService() = default;
};

struct ServiceHelper {
    virtual void OnNewSession(int32_t port_index, HandleTable::Entry<ServerSession> session) {
        Append(session);
    }

    template<typename T>
    uint32_t Append(HandleTable::Entry<T> entry) {
        handles.emplace_back(std::move(entry.first));
        objects.emplace_back(std::move(entry.second));
        return static_cast<uint32_t>(handles.size() - 1);
    }

    template<typename T>
    uint32_t Append(DebugHandle handle, std::shared_ptr<T> object) {
        handles.emplace_back(std::move(handle));
        objects.emplace_back(std::move(object));
        return static_cast<uint32_t>(handles.size() - 1);
    }

    virtual void Erase(int32_t index) {
        handles.erase(handles.begin() + index);
        objects.erase(objects.begin() + index);
    }

    template<typename T>
    std::shared_ptr<T> GetObject(int32_t index) {
        return std::dynamic_pointer_cast<T>(objects[index]);
    }

private:
    struct DoNothingInternal {};
    struct SendReplyInternal {};
    struct SendReplyToInternal { Handle handle; };

    using OptionalIndex = std::variant<DoNothingInternal, SendReplyInternal, SendReplyToInternal>;

public:

    static constexpr auto DoNothing = OptionalIndex{DoNothingInternal{}};
    static constexpr auto SendReply = OptionalIndex{SendReplyInternal{}};
    static constexpr auto SendReplyTo(Handle handle) {
        return OptionalIndex { SendReplyToInternal { handle } };
    }

    /**
     * Enter a standard service handler loop. The given callback receives the
     * handle index. The callback may modify the handle table. When the
     * callback returns OptionalIndex{SendReply{}}, the next call to
     * ReplyAndReceive will send a reply to the signalled handle. Otherwise,
     * The loop will continue without sending a reply.
     */
    void Run(FakeThread& thread, std::function<OptionalIndex(FakeThread&, uint32_t)> command_callback);

    std::vector<Handle> handles;
    std::vector<std::shared_ptr<Object>> objects;
};

}  // namespace OS

}  // namespace HLE
