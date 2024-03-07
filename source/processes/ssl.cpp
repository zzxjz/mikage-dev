#include "ssl.hpp"

#include <framework/exceptions.hpp>

namespace HLE {

namespace OS {

struct FakeSSL {
    FakeSSL(FakeThread& thread);
};

static auto SSLCCommandHandler(FakeThread& thread, FakeSSL&, std::string_view service_name, const IPC::CommandHeader& header) {
    switch (header.command_id) {
    using Initialize = IPC::IPCCommand<0x1>::add_process_id::response;
    using GenerateRandomBytes = IPC::IPCCommand<0x11>::add_uint32::add_buffer_mapping_write::response;
    case Initialize::id:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case GenerateRandomBytes::id:
        // Stubbed. TODO: Forward to ps::GenerateRandomBytes instead
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    default:
        throw Mikage::Exceptions::NotImplemented("Unknown {} service command with header {:#010x}", service_name, header.raw);
    }

    return ServiceHelper::SendReply;
}

static void SSLCThread(FakeSSL& context, FakeThread& thread) {
    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, "ssl:C", 1));

    auto InvokeCommandHandler = [&](FakeThread& thread, uint32_t /*signalled_handle_index*/) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return SSLCCommandHandler(thread, context, "ssl:C", header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

FakeSSL::FakeSSL(FakeThread& thread) {
    thread.name = "SSLCThread";

    SSLCThread(*this, thread);
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakeSSL>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakeSSL>(os, setup, pid, name);
}

} // namespace OS

} // namespace HLE
