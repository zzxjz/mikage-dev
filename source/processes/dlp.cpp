#include "processes/dlp.hpp"

#include "fake_process.hpp"

namespace HLE {

namespace OS {

struct FakeDLP {
    FakeDLP(FakeThread&);

    static constexpr auto session_limit = 1;

    spdlog::logger& logger;
};


static auto DlpSrvrCommandHandler(FakeThread& thread, FakeDLP& context, const Platform::IPC::CommandHeader& header) {
    switch (header.command_id) {
    case 0xe: // IsChild
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0); // Not a child
        break;

    default:
        throw std::runtime_error(fmt::format("DownloadPlay: Unknown dlp:SRVR command with header {:#x}", header.raw));
    }

    return ServiceHelper::SendReply;
}

FakeDLP::FakeDLP(FakeThread& thread)
    : logger(*thread.GetLogger()) {

    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, "dlp:SRVR", session_limit));

    auto InvokeCommandHandler = [this](FakeThread& thread, uint32_t /* index of signalled handle */) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return DlpSrvrCommandHandler(thread, *this, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakeDLP>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakeDLP>(os, setup, pid, name);
}

}  // namespace OS

}  // namespace HLE
