#include "fake_process.hpp"
#include "platform/sm.hpp"

namespace HLE {

namespace OS {

ServerPortUtilBase::ServerPortUtilBase(FakeThread& owning_thread, HandleTable::Entry<ServerPort> port, uint32_t max_sessions) : max_sessions(max_sessions), owning_thread(owning_thread) {
    AppendToHandleTable(port.first, port.second);
}

ServerPortUtilBase::~ServerPortUtilBase() {
// TODO: This breaks things when exceptions are involed. Say, for instance, a FakeThread is throwing an exception. What happens is:
// * Exception is thrown, stack unwinding starts
// * The ServerPortUtilBase destructor is called
// * SVCCloseHandle is called, hence invoking the scheduler
// * ... but now propagation of the exception is delayed until we re-enter the FakeThread's coroutine
// Since closing these handles isn't a big deal (the only time we call this destructor is when the FakeProcess exits anyway!), for now we just don't close them here. The emulated OS will take care of that instead.
//    for (auto handle : handle_table)
//        owning_thread.CallSVC(&OS::SVCCloseHandle, handle);
}

std::shared_ptr<Object> ServerPortUtilBase::GetObjectBase(int32_t index) const {
    return object_table[index];
}

Handle ServerPortUtilBase::GetHandle(int32_t index) const {
    return handle_table[index];
}

void ServerPortUtilBase::AppendToHandleTable(Handle handle, std::shared_ptr<Object> object) {
    handle_table.push_back(handle);
    object_table.push_back(object);
}

void ServerPortUtilBase::UnknownRequest(const IPC::CommandHeader& header) {
/*    // Return error
    WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
    WriteTLS(0x84, -1);
    GetLogger()->warn("{} {} received unknown command id {:#x} (full command: {:#010x})",
                      ThreadPrinter{*this}, GetInternalName(), header.command_id.Value(), header.raw);

    // We could just continue, but apparently most programs do not actually check for errors appropriately
    // TODO: Once we reach sufficient level of implementation completeness, consider removing this panic.
    thread.CallSVC(&OS::SVCBreak, OS::BreakReason::Panic);
*/
}

void ServerPortUtilBase::LogStub(const IPC::CommandHeader& header) {
//    GetLogger()->info("{}{} sending stub reply for command id {:#x}", ThreadPrinter{*this}, GetInternalName(), header.command_id.Value());
}

void ServerPortUtilBase::LogReturnedHandle(Handle handle) {
//    GetLogger()->info("{}{} returning handle {}", ThreadPrinter{*this}, GetInternalName(), HandlePrinter{*this,handle});
}

OS::ResultAnd<int32_t> ServerPortUtilBase::ReplyAndReceive(FakeThread& thread, Handle reply_target) {
    auto ret = thread.CallSVC(&OS::SVCReplyAndReceive, handle_table.data(), handle_table.size(), reply_target);
    if (std::get<0>(ret) != 0) {
        throw std::runtime_error("Stop using me, I'm broken");
    }
    return ret;
}

OS::ResultAnd<int32_t> ServerPortUtilBase::AcceptSession(FakeThread& thread, int32_t index) {
    OS::Result result;
    HandleTable::Entry<ServerSession> session;
    std::tie(result,session) = thread.CallSVC(&OS::SVCAcceptSession, *GetObject<ServerPort>(index));
    if (result != RESULT_OK)
        thread.CallSVC(&OS::SVCBreak, OS::BreakReason::Panic);

    // If maximum number of sessions is exhausted, close handle again.
    if (handle_table.size() < OS::MAX_SESSIONS+1) {
        AppendToHandleTable(session.first, session.second);
    } else {
        thread.GetLogger()->warn("{} Maximal number of sessions exhausted; closing session handle {}",
                                 ThreadPrinter{thread}, HandlePrinter{thread,session.first});
        thread.CallSVC(&OS::SVCCloseHandle, session.first);
        result = -1; // TODO: Set proper result code!
    }

    return std::make_pair(result, static_cast<int32_t>(handle_table.size() - 1));
}

ServerPortUtil::ServerPortUtil(FakeThread& thread, const std::string& name, uint32_t max_sessions)
    : ServerPortUtilBase(thread, SetupPort(thread, name, max_sessions), max_sessions) {
}

HandleTable::Entry<ServerPort> ServerPortUtil::SetupPort(FakeThread& thread, const std::string& name, uint32_t max_sessions) {
    HandleTable::Entry<ServerPort> server_port;
    HandleTable::Entry<ClientPort> client_port;
    OS::Result result;
    std::tie(result,server_port,client_port) = thread.CallSVC(&OS::SVCCreatePort, name, max_sessions);
    if (result != RESULT_OK) {
        throw std::runtime_error(fmt::format("Failed to create port \"{}\"", name));
    }

    // Close the client port, since we don't need it
    std::tie(result) = thread.CallSVC(&OS::SVCCloseHandle, client_port.first);
    if (result != RESULT_OK) {
        throw std::runtime_error("Failed to close handle");
    }

    return server_port;
}

ServiceUtil::ServiceUtil(FakeThread& thread, const std::string& name, uint32_t max_sessions)
    : ServerPortUtilBase(thread, SetupService(thread, name, max_sessions), max_sessions) {
}

HandleTable::Entry<ServerPort> ServiceUtil::SetupService(FakeThread& thread, const std::string& name, uint32_t max_sessions) {
    auto logger = thread.GetLogger();

    // Connect to "srv:"
    OS::Result result;
    Handle srv_handle;
    std::shared_ptr<ClientSession> srv_session;
    std::forward_as_tuple(result,std::tie(srv_handle,srv_session)) = thread.CallSVC(&OS::SVCConnectToPort, "srv:");
    if (result != RESULT_OK) {
        throw std::runtime_error("Failed to connect to srv: port");
    }

    // Register service
    Handle service_handle = IPC::SendIPCRequest<Platform::SM::SRV::RegisterService>(thread, srv_handle, Platform::SM::PortName(name.c_str()), max_sessions);
    std::tie(result) = thread.CallSVC(&OS::SVCCloseHandle, srv_handle);
    if (result != RESULT_OK) {
        throw std::runtime_error("Failed to close handle");
    }
    auto service_port = thread.GetProcessHandleTable().FindObject<ServerPort>(service_handle);

    return { service_handle, service_port };
}

void ServiceHelper::Run(FakeThread& thread, std::function<OptionalIndex(FakeThread&, uint32_t)> command_callback) {
    Handle last_signalled = HANDLE_INVALID;

    auto&& os = thread.GetOS();

    for (;;) {
        OS::Result result;
        int32_t index;
        std::tie(result,index) = thread.CallSVC(&OS::SVCReplyAndReceive, handles.data(), handles.size(), last_signalled);
        last_signalled = HANDLE_INVALID;
        if (result == 0xc920181a) {
            if (index == -1) {
                // Reply target was closed... TODO: Implement
                os.SVCBreak(thread, OS::BreakReason::Panic);
            } else {
                // Session was closed client-side; close it server-side, too
                thread.GetLogger()->info("{}ClientSession closed, closing server-side session", ThreadPrinter{thread});

                // Erase table entry before calling CloseHandle so that the latter will see the object refcount=1
                auto handle = handles[index];
                Erase(index);
                os.SVCCloseHandle(thread, handle);
            }
        } else if (result != RESULT_OK) {
            os.SVCBreak(thread, OS::BreakReason::Panic);
        } else {
            if (auto server_port = std::dynamic_pointer_cast<ServerPort>(objects[index])) {
                // ServerPort: Incoming client connection

                HandleTable::Entry<ServerSession> session;
                std::tie(result,session) = thread.CallSVC(&OS::SVCAcceptSession, *GetObject<ServerPort>(index));
                if (result != RESULT_OK) {
                    os.SVCBreak(thread, OS::BreakReason::Panic);
                }

                OnNewSession(index, session);
            } else {
                // server_session: Incoming IPC command from the indexed client
//                 thread.GetLogger()->info("{}received IPC request", ThreadPrinter{thread});
                auto signalled_handle = handles[static_cast<uint32_t>(index)];
                auto reply_target_opt = command_callback(thread, static_cast<uint32_t>(index));
                std::visit(boost::hana::overload([&](DoNothingInternal) { },
                                                 [&](SendReplyInternal) { last_signalled = signalled_handle; },
                                                 [&](SendReplyToInternal tag) { last_signalled = tag.handle; }),
                           reply_target_opt);
            }
        }
    }
}

}  // namespace OS

}  // namespace HLE
