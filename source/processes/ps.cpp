#include "ps.hpp"

#include <framework/exceptions.hpp>

namespace HLE {

namespace OS {

struct FakePS {
    FakePS(FakeThread&);

    static constexpr auto session_limit = 9;

    spdlog::logger& logger;
};

static OS::ResultAnd<> HandleEncryptSignDecryptVerifyAesCcm(
        FakeThread& thread, FakePS&, uint32_t input_size, uint32_t, uint32_t output_size,
         uint32_t total_out_size, uint32_t out_mac_size, uint32_t, uint32_t,
         uint32_t, uint32_t, uint32_t, IPC::MappedBuffer input_data,
         IPC::MappedBuffer output_data) {
    if (input_data.size < input_size || output_data.size < total_out_size) {
        throw Mikage::Exceptions::Invalid("Buffers smaller than indicated size");
    }

    if (total_out_size != output_size + out_mac_size) {
        throw Mikage::Exceptions::NotImplemented("EncryptSignDecryptVerifyAesCcm: Unexpected total output size");
    }

    if (input_size != output_size) {
        // Not sure what should happen in this case
        throw Mikage::Exceptions::NotImplemented("EncryptSignDecryptVerifyAesCcm: Input size doesn't match output size");
    }

    // TODO: Implement the actual cryptography. For now, we just copy the unmodified input buffer
    for (uint32_t offset = 0; offset < input_size; ++offset) {
        thread.WriteMemory(output_data.addr + offset, thread.ReadMemory(input_data.addr + offset));
    }

    return std::make_tuple(RESULT_OK);
}

static auto CommandHandler(FakeThread& thread, FakePS& context, const Platform::IPC::CommandHeader& header) {
    using EncryptSignDecryptVerifyAesCcm = IPC::IPCCommand<0x5>::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_buffer_mapping_read::add_buffer_mapping_write::response;

    switch (header.command_id) {
    case 0x2: // VerifyRsa256Hash
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case EncryptSignDecryptVerifyAesCcm::id:
    {
        IPC::HandleIPCCommand<EncryptSignDecryptVerifyAesCcm>(HandleEncryptSignDecryptVerifyAesCcm, thread, thread, context);
        break;
    }

    case 0xc: // SeedRNG
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0xd: // GenerateRandomBytes
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    default:
        fmt::print("PS: Unknown ps:ps command with header {:#x}", header.raw);
        throw std::runtime_error(fmt::format("PS: Unknown ps:ps command with header {:#x}", header.raw));
    }

    return ServiceHelper::SendReply;
}

FakePS::FakePS(FakeThread& thread)
    : logger(*thread.GetLogger()) {

    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, "ps:ps", session_limit));

    {
        auto static_buffer_size = 0x208;
        auto [result, static_buffer_addr] = thread.CallSVC(&OS::OS::SVCControlMemory, 0, 0, static_buffer_size, 3 /* COMMIT */, 3 /* RW */);
        thread.WriteTLS(0x180, IPC::TranslationDescriptor::MakeStaticBuffer(0, static_buffer_size).raw);
        thread.WriteTLS(0x184, static_buffer_addr);
    }

    auto InvokeCommandHandler = [this](FakeThread& thread, uint32_t /* index of signalled handle */) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return CommandHandler(thread, *this, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakePS>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakePS>(os, setup, pid, name);
}

}  // namespace OS

}  // namespace HLE
