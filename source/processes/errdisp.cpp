#include "errdisp.hpp"
#include "os.hpp"

#include <framework/exceptions.hpp>

namespace HLE {

namespace OS {

struct FakeErrorDisp {
    void OnIPCRequest(FakeThread&, const IPC::CommandHeader&);

public:
    FakeErrorDisp(FakeThread& thread);

    virtual ~FakeErrorDisp() = default;
};

FakeErrorDisp::FakeErrorDisp(FakeThread& thread) {
    ServiceHelper service;
    auto [result, server_port, client_port] = thread.CallSVC(&OS::SVCCreatePort, "err:f", OS::MAX_SESSIONS);
    if (result != RESULT_OK) {
        throw Mikage::Exceptions::Invalid("Failed to create err:f port");
    }

    service.Append(server_port);
    thread.name = "ErrDispThread";
    auto InvokeCommandHandler = [&](FakeThread& thread, uint32_t /*index*/) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        OnIPCRequest(thread, header);
        return ServiceHelper::SendReply;
    };
    service.Run(thread, std::move(InvokeCommandHandler));
}

void FakeErrorDisp::OnIPCRequest(FakeThread& thread, const IPC::CommandHeader& header) {
    switch (header.command_id) {
    case 1:  // Throw
    {
        thread.GetLogger()->info("{} received Throw", ThreadPrinter{thread});

        // Validate command
        if (header.raw != 0x00010800)
            thread.GetOS().SVCBreak(thread, OS::BreakReason::Panic);

        auto type = thread.ReadTLS(0x84) & 0xff;
        auto error_code = thread.ReadTLS(0x88);
        throw std::runtime_error(fmt::format(   "ErrDisp exception: type={:#x}, error={:#010x}, pc={:#010x}, process_id={:#x}, title_id={:#010x}{:08x}, app_title_id={:#010x}{:08x}",
                                                type, error_code, thread.ReadTLS(0x8c), thread.ReadTLS(0x90),
                                                thread.ReadTLS(0x94), thread.ReadTLS(0x98), thread.ReadTLS(0x9c), thread.ReadTLS(0xa0)));
    }

    default:
        throw Mikage::Exceptions::NotImplemented("Unknown err:f IPC request {:#x}", header.raw);
    }
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakeErrorDisp>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakeErrorDisp>(os, setup, pid, name);
}

}  // namespace OS

}  // namespace HLE
