#include "processes/dlp.hpp"

#include "fake_process.hpp"

namespace HLE {

namespace OS {

struct FakeCECD {
    FakeCECD(FakeThread&);

    // Applies to cecd:u and cecd:s. cecd:ndm only accepts a single connection
    static constexpr auto session_limit = 4;

    spdlog::logger& logger;

    HandleTable::Entry<Event> info_event;
    HandleTable::Entry<Event> state_change_event;
};


static auto UserCommandHandler(FakeThread& thread, FakeCECD& context, const Platform::IPC::CommandHeader& header) {
    switch (header.command_id) {
    case 0x1: // OpenRawFile
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x1, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        // A non-zero value needs to be returned here, since Home Menu fails to
        // allocate some internal buffers otherwise
        thread.WriteTLS(0x88, 1);
        break;

    case 0x2: // ReadRawFile
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x1, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 1);
        break;

    case 0x5: // WriteRawFile
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x5, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0x8: // Delete
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x8, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0x9:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x9, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0xc: // RunCommandAlt
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0xc, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0xe: // GetCecStateAbbreviated
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0xe, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 1); // idle state
        break;

    case 0xf: // GetCecInfoEventHandle
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0xf, 1, 2).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, IPC::TranslationDescriptor::MakeHandles(1).raw);
        thread.WriteTLS(0x8c, context.info_event.first.value);
        break;
    }

    case 0x10: // GetChangeStateEventHandle
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x10, 1, 2).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, IPC::TranslationDescriptor::MakeHandles(1).raw);
        thread.WriteTLS(0x8c, context.info_event.first.value);
        break;
    }

    case 0x11: // OpenAndWrite
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x11, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x12: // OpenAndRead
    {
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x12, 2 + 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 1); // Number of bytes read (Home Menu fails to allocate a buffer used later on if we return 0 here)
        thread.WriteTLS(0x8c, 0);
        thread.WriteTLS(0x90, 0);
        break;
    }

    default:
        throw std::runtime_error(fmt::format("CECD: Unknown cecd:u command with header {:#x}", header.raw));
    }

    return ServiceHelper::SendReply;
}

FakeCECD::FakeCECD(FakeThread& thread)
    : logger(*thread.GetLogger()) {

    // TODO: Should these be OneShot or Sticky events?
    OS::Result result;
    std::tie(result, info_event) = thread.CallSVC(&OS::SVCCreateEvent, ResetType::OneShot);
    if (result != RESULT_OK) {
        throw std::runtime_error("Failed to create CECD info event");
    }
    std::tie(result, state_change_event) = thread.CallSVC(&OS::SVCCreateEvent, ResetType::OneShot);
    if (result != RESULT_OK) {
        throw std::runtime_error("Failed to create CECD state change event");
    }

    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, "cecd:u", session_limit));
    service.Append(ServiceUtil::SetupService(thread, "cecd:s", session_limit));

    auto InvokeCommandHandler = [this](FakeThread& thread, uint32_t /* index of signalled handle */) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return UserCommandHandler(thread, *this, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakeCECD>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakeCECD>(os, setup, pid, name);
}

}  // namespace OS

}  // namespace HLE
