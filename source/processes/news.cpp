#include "news.hpp"
#include "fake_process.hpp"

namespace HLE {

namespace OS {

static auto NEWSUCommandHandler(FakeNEWS&, FakeThread&, Platform::IPC::CommandHeader header) try {
    switch (header.command_id) {
    default:
        throw IPC::IPCError{header.raw, 0xdeadbeef};
    }

    return ServiceHelper::SendReply;
} catch (const IPC::IPCError& err) {
    throw std::runtime_error(fmt::format("Unknown news:u service command with header {:#010x}", err.header));
}

static auto NEWSSCommandHandler(FakeNEWS& context, FakeThread& thread, Platform::IPC::CommandHeader header) try {
    using GetTotalNotifications = IPC::IPCCommand<0x5>::response::add_uint32;
    using GetNewsDBHeader = IPC::IPCCommand<0xa>::add_uint32::add_buffer_mapping_write::response/*::add_buffer_mapping_write*/;

    switch (header.command_id) {
    case 1:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case GetTotalNotifications::id:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0);
        break;

    case 0x13: // WriteNewsDBSavedata
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0x14: // Unknown, returns at least one uint32_t value in addition to the result
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0);
        break;

    case GetNewsDBHeader::id:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, /*2*/0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0); // number of bytes written to output buffer
        break;

    default:
        throw IPC::IPCError{header.raw, 0xdeadbeef};
    }

    return ServiceHelper::SendReply;
} catch (const IPC::IPCError& err) {
    throw std::runtime_error(fmt::format("Unknown news:s service command with header {:#010x}", err.header));
}

template<typename Handler>
static void IPCThread(FakeNEWS& context, FakeThread& thread, const char* service_name, uint32_t session_limit, Handler&& handler) {
    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, service_name, session_limit));

    auto InvokeCommandHandler = [&context,handler=std::move(handler)](FakeThread& thread, uint32_t) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return handler(context, thread, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

FakeNEWS::FakeNEWS(FakeThread& thread) {
    thread.name = "news:sThread";

    {
        auto newsu_thread = std::make_shared<WrappedFakeThread>(thread.GetParentProcess(), [this](FakeThread& thread) {
            return IPCThread(*this, thread, "news:u", 1, NEWSUCommandHandler);
        });
        newsu_thread->name = "news:uThread";
        thread.GetParentProcess().AttachThread(newsu_thread);
    }

    IPCThread(*this, thread, "news:s", 2, NEWSSCommandHandler);
}

}  // namespace OS

}  // namespace HLE
