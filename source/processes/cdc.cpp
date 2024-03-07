#include "cdc.hpp"
#include "os.hpp"

#include "platform/cdc.hpp"

namespace HLE {

namespace OS {

struct FakeCDC final {
    spdlog::logger& logger;

public:
    FakeCDC(FakeThread& thread);

    void ServiceThread( FakeThread& thread, const char* service_name,
                        decltype(ServiceHelper::SendReply) (*command_handler)(FakeThread&, FakeCDC&, const IPC::CommandHeader&));
};

static OS::ResultAnd<uint32_t, uint32_t> HIDGetTouchData(FakeThread& thread, FakeCDC& context) {
    context.logger.info("{}received GetTouchData", ThreadPrinter{thread});

    // TODO: This should return the actual touch data instead of dummy zeroes
    return std::make_tuple(RESULT_OK, 0, 0);
}

static OS::ResultAnd<> HIDInitialize(FakeThread& thread, FakeCDC& context) {
    context.logger.info("{}received Initialize", ThreadPrinter{thread});

    return std::make_tuple(RESULT_OK);
}

static decltype(ServiceHelper::SendReply) HIDCommandHandler(FakeThread& thread, FakeCDC& context, const IPC::CommandHeader& header) {
    using namespace Platform::CDC::HID;

    switch (header.command_id) {
    case GetTouchData::id:
        IPC::HandleIPCCommand<GetTouchData>(HIDGetTouchData, thread, thread, context);
        break;

    case Initialize::id:
        IPC::HandleIPCCommand<Initialize>(HIDInitialize, thread, thread, context);
        break;

    default:
        throw std::runtime_error(fmt::format("Unknown cdc:HID service command with header {:#010x}", header.raw));
    }

    return ServiceHelper::SendReply;
}

static decltype(ServiceHelper::SendReply) DSPCommandHandler(FakeThread& thread, FakeCDC& context, const IPC::CommandHeader& header) {
    // 3dbrew erratum: This command takes no input parameters
    using Command0x6 = IPC::IPCCommand<0x6>::response::add_uint32;

    using Command0x7 = IPC::IPCCommand<0x7>::add_uint32::response;

    switch (header.command_id) {
    case Command0x6::id:
        context.logger.info("{}received cdc:DSP command 0x6", ThreadPrinter{thread});
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0);
        break;

    case Command0x7::id:
        context.logger.info("{}received cdc:DSP command 0x7 with arg {:#x}", ThreadPrinter{thread}, thread.ReadTLS(0x84));
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    using Command0x8 = IPC::IPCCommand<0x8>::add_uint32::response;
    case Command0x8::id:
        context.logger.info("{}received cdc:DSP command 0x8 with arg {:#x}", ThreadPrinter{thread}, thread.ReadTLS(0x84));
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    default:
        throw std::runtime_error(fmt::format("Unknown cdc:DSP service command with header {:#010x}", header.raw));
    }

    return ServiceHelper::SendReply;
}

void FakeCDC::ServiceThread(FakeThread& thread, const char* service_name,
                            decltype(ServiceHelper::SendReply) (*command_handler)(FakeThread&, FakeCDC&, const IPC::CommandHeader&)) {
    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, service_name, 1));

    auto InvokeCommandHandler = [&](FakeThread& thread, uint32_t /*signalled_handle_index*/) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return command_handler(thread, *this, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

FakeCDC::FakeCDC(FakeThread& thread)
    : logger(*thread.GetLogger()) {
    {
        auto new_thread = std::make_shared<WrappedFakeThread>(  thread.GetParentProcess(),
                                                                [this](FakeThread& thread) { ServiceThread(thread, "cdc:DSP", DSPCommandHandler); });
        new_thread->name = "cdc:DSPThread";
        thread.GetParentProcess().AttachThread(new_thread);
    }

    thread.name = "cdc:HIDThread";
    ServiceThread(thread, "cdc:HID", HIDCommandHandler);
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakeCDC>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakeCDC>(os, setup, pid, name);
}

}  // namespace OS

}  // namespace HLE
