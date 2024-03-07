#include "http.hpp"
#include "fake_process.hpp"

namespace HLE {

namespace OS {

static auto CommandHandler(FakeHTTP& context, FakeThread& thread, Platform::IPC::CommandHeader header) try {
    switch (header.command_id) {
    // Initialize
    case 0x1:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    using CreateRootCertChain = IPC::IPCCommand<0x2d>::response::add_uint32;
    case CreateRootCertChain::id:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0xafafafa0);
        break;

    using RootCertChainAddDefaultCert = IPC::IPCCommand<0x30>::add_uint32::add_uint32::response::add_uint32;
    case RootCertChainAddDefaultCert::id:
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0xafafafa1);
        break;
    }

    using OpenDefaultClientCertContext = IPC::IPCCommand<0x33>::add_uint32::response::add_uint32;
    case OpenDefaultClientCertContext::id:
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0xafafafa2);
        break;
    }

    default:
        throw IPC::IPCError{header.raw, 0xdeadbef0};
    }

    return ServiceHelper::SendReply;
} catch (const IPC::IPCError& err) {
    throw std::runtime_error(fmt::format("Unknown HTTP service command with header {:#010x}", err.header));
}

static void IPCThread(FakeHTTP& context, FakeThread& thread) {
    // TODO: How many parallel sessions should we support?
    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, "http:C", 4));

    auto InvokeCommandHandler = [&context](FakeThread& thread, uint32_t index) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return CommandHandler(context, thread, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

FakeHTTP::FakeHTTP(FakeThread& thread) {
    thread.name = "HTTPThread";

    IPCThread(*this, thread);
}

}  // namespace OS

}  // namespace HLE
