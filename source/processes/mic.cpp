#include "fake_process.hpp"

namespace HLE {

namespace OS {

struct FakeMIC {
    FakeMIC(FakeThread&);

    // TODO: This should only accept a single connection
//    static constexpr auto session_limit = 1;
    static constexpr auto session_limit = 2;

    spdlog::logger& logger;
};


static auto CommandHandler(FakeThread& thread, FakeMIC& context, const Platform::IPC::CommandHeader& header) {
    switch (header.command_id) {
    case 0x1: // MapSharedMemory
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x1, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x2: // UnmapSharedMemory
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x2, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x3: // StartSampling
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x3, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x5: // StopSampling
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x5, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x6: // IsSampling
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x6, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0); // not sampling
        break;
    }

    case 0x8: // SetGain
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x8, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x9: // GetGain
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x9, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0);
        break;
    }

    case 0xa: // SetPower
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0xa, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0xd: // SetClamp
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0xd, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x10: // Unknown, takes 1 normal parameter
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x8, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    default:
        throw std::runtime_error(fmt::format("MIC: Unknown mic:u command with header {:#x}", header.raw));
    }

    return ServiceHelper::SendReply;
}

FakeMIC::FakeMIC(FakeThread& thread)
    : logger(*thread.GetLogger()) {

    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, "mic:u", session_limit));

    auto InvokeCommandHandler = [this](FakeThread& thread, uint32_t /* index of signalled handle */) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return CommandHandler(thread, *this, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakeMIC>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakeMIC>(os, setup, pid, name);
}

}  // namespace OS

}  // namespace HLE
