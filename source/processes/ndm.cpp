#include "fake_process.hpp"
#include "ndm.hpp"

#include <framework/exceptions.hpp>

#include <memory>

namespace HLE {

namespace OS {

enum class ExclusiveState {
    None,
    Infrastructure,
    LocalCommunications,
    StreetPass,
    StreetPassData,
};

struct FakeNDM {
    FakeNDM(FakeThread&);

    decltype(HLE::OS::ServiceHelper::SendReply) OnIPCRequest(Handle sender, Thread&, const IPC::CommandHeader&);

    // TODO: Should actually be tied to a ProcessId
    ExclusiveState exclusive_state = ExclusiveState::None;
};

FakeNDM::FakeNDM(FakeThread& thread) {
    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, "ndm:u", /*2*/20));

    auto InvokeCommandHandler = [&](FakeThread& thread, uint32_t signaled_index) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return OnIPCRequest(service.handles[signaled_index], thread, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

decltype(HLE::OS::ServiceHelper::SendReply) FakeNDM::OnIPCRequest(Handle sender, Thread& thread, const IPC::CommandHeader& header) {
    using EnterExclusiveState = IPC::IPCCommand<0x1>::add_uint32::add_process_id::response;
    using LeaveExclusiveState = IPC::IPCCommand<0x2>::add_process_id::response;

    switch (header.command_id) {
    case EnterExclusiveState::id:
        exclusive_state = static_cast<ExclusiveState>(thread.ReadTLS(0x84));
        if (Meta::to_underlying(exclusive_state) > Meta::to_underlying(ExclusiveState::StreetPassData)) {
            throw Mikage::Exceptions::Invalid("Invalid NDM exclusive state");
        }

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case LeaveExclusiveState::id:
        exclusive_state = ExclusiveState::None;

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0x3: // QueryExclusiveMode
        thread.GetLogger()->info("{} received QueryExclusiveMode", ThreadPrinter{thread});

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, Meta::to_underlying(exclusive_state));
        break;

    case 0x6: // SuspendDaemons
    {
        thread.GetLogger()->info("{} received SuspendDaemons", ThreadPrinter{thread});

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x7: // ResumeDaemons
    {
        thread.GetLogger()->info("{} received ResumeDaemons", ThreadPrinter{thread});

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x8: // SuspendScheduler
    {
        thread.GetLogger()->info("{} received SuspendScheduler (parameter={:x})", ThreadPrinter{thread}, thread.ReadTLS(0x84));

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x9: // ResumeScheduler
    {
        thread.GetLogger()->info("{} received ResumeScheduler", ThreadPrinter{thread});

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x14: // OverrideDefaultDaemons
    {
        thread.GetLogger()->info("{} received OverrideDefaultDaemons", ThreadPrinter{thread});

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x15: // ResetDefaultDaemons
    {
        thread.GetLogger()->info("{} received ResetDefaultDaemons", ThreadPrinter{thread});

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    using ClearHalfAwakeMacFilter = IPC::IPCCommand<0x17>::response;
    case ClearHalfAwakeMacFilter::id:
    {
        thread.GetLogger()->info("{} received ClearHalfAwakeMacFilter", ThreadPrinter{thread});

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    default:
        throw std::runtime_error(fmt::format("Unknown ndm:u service command with header {:#010x}", header.raw));
    }

    return HLE::OS::ServiceHelper::SendReply;
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakeNDM>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakeNDM>(os, setup, pid, name);
}

}  // namespace OS

}  // namespace HLE

