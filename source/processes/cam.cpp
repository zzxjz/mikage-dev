#include "cam.hpp"
#include "fake_process.hpp"

#include "os.hpp"

#include <framework/image_format.hpp>

namespace HLE {

namespace OS {

// TODO: Move to hw/y2r
struct CAMOutputFormat {
    uint32_t raw;

    static constexpr std::array<GenericImageFormat, 4> format_map = {{
        GenericImageFormat::RGBA8,
        GenericImageFormat::RGB8,
        GenericImageFormat::RGBA5551,
        GenericImageFormat::RGB565,
    }};
};

struct FakeCAM {
    // Staging buffer for output data
    // TODO: On actual hardware, the DMA engines are likely used to stream data directly to/from the Y2R register block
    VAddr output_vaddr = 0;
    VAddr input_y_vaddr = 0;
    uint32_t input_size = 0;

    GenericImageFormat output_format = GenericImageFormat::Unknown;

    HandleTable::Entry<Event> transfer_end_event;

    FakeCAM(FakeThread& thread);
};

static OS::ResultAnd<> Y2RSetReceiving(FakeCAM& context, FakeThread& thread, VAddr dest_vaddr, uint32_t dest_size, uint32_t transfer_unit, uint32_t transfer_stride, Handle dest_process_handle) {
    auto dest_process = thread.GetProcessHandleTable().FindObject<Process>(dest_process_handle);
    thread.GetLogger()->info("SetReceiving: Stub conversion to {:#x}-{:#x} in {}", dest_vaddr, dest_vaddr + dest_size, ProcessPrinter { *dest_process });

    // TODO: Below logic should be done in StartConversion instead

    // TODO: Set up Y2R hardware to perform the actual conversion.

    DMAConfig config {
        .channel = 0xff,
        .value_size = 0, // uint8_t
        .flags = {}, // ??
        .dest = DMAConfig::SubConfig::Default(),
        .source = DMAConfig::SubConfig::Default(),
    };
    // Copy raw data to staging buffer so we get *some* feedback
    auto [result, dma_handle] = thread.CallSVC(&OS::SVCStartInterprocessDMA, *dest_process, thread.GetParentProcess(), context.input_y_vaddr, context.output_vaddr, std::min(context.input_size, dest_size), config);

    std::tie(result, dma_handle) = thread.CallSVC(&OS::SVCStartInterprocessDMA, thread.GetParentProcess(), *dest_process, context.output_vaddr, dest_vaddr, dest_size, config);

    // TODO: Link dma_handle to transfer_end_event

    thread.CallSVC(&OS::SVCCloseHandle, dest_process_handle);
    return { result };
}


static auto Y2RCommandHandler(FakeCAM& context, FakeThread& thread, Platform::IPC::CommandHeader header) try {
    using SetReceiving = IPC::IPCCommand<0x18>::add_uint32::add_uint32::add_uint32::add_uint32::add_handle<IPC::HandleType::Process>::response;

    switch (header.command_id) {
    // SetInputFormat
    case 0x1:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x1, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetOutputFormat
    case 0x3:
        context.output_format = ToGenericFormat(CAMOutputFormat { thread.ReadTLS(0x84) });

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x3, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0x4: // GetOutputFormat
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x3, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, FromGenericFormat<CAMOutputFormat>(context.output_format).raw);
        break;

    // SetRotation
    case 0x5:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x5, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetBlockAlignment
    case 0x7:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x7, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetSpatialDithering
    case 0x9:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x9, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetTemporalDithering
    case 0xb:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0xb, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetTransferEndInterrupt
    case 0xd:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0xd, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // GetTransferEndEvent
    case 0xf:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0xf, 1, 2).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, IPC::TranslationDescriptor::MakeHandles(1, false).raw);
        thread.WriteTLS(0x8c, context.transfer_end_event.first.value);
        break;

    // SetSendingY
    case 0x10:
        context.input_y_vaddr = thread.ReadTLS(0x84);
        context.input_size = thread.ReadTLS(0x88);
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x10, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetSendingU
    case 0x11:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x11, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetSendingV
    case 0x12:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x12, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetSendingYuv
    case 0x13:
        context.input_y_vaddr = thread.ReadTLS(0x84);
        context.input_size = thread.ReadTLS(0x88);
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x13, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case SetReceiving::id:
        IPC::HandleIPCCommand<SetReceiving>(Y2RSetReceiving, thread, context, thread);
        break;

    // SetInputLineWidth
    case 0x1a:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x1a, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetInputLines
    case 0x1c:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x1c, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetCoefficientParams
    case 0x1e:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x1e, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetStandardCoefficient
    case 0x20:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x20, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetAlpha
    case 0x22:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x22, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetDitheringWeightParams
    case 0x24:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x24, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // StartConversion
    case 0x26:
        // Immediately signal completion
        thread.CallSVC(&OS::SVCSignalEvent, context.transfer_end_event.first);

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x26, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetPackageParameter
    case 0x29:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x29, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // PingProcess
    case 0x2a:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x2a, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0);
        break;

    // DriverInitialize
    case 0x2b:
        thread.CallSVC(&OS::SVCClearEvent, context.transfer_end_event.first);

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x2b, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // DriverFinalize
    case 0x2c:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x2c, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // StopConversion
    case 0x27:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x27, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // IsBusyConversion
    case 0x28:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0x28, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0); // Not busy
        break;

    default:
        throw IPC::IPCError{header.raw, 0xdeadbeef};
    }

    return ServiceHelper::SendReply;
} catch (const IPC::IPCError& err) {
    throw std::runtime_error(fmt::format("Unknown y2r:u service command with header {:#010x}", err.header));
}

template<typename Handler>
static void IPCThread(FakeCAM& context, FakeThread& thread, const char* service_name, Handler&& handler) {
    // TODO: Only a single connection should be accepted on any service
    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, service_name, 3));

    auto InvokeCommandHandler = [&context,handler=std::move(handler)](FakeThread& thread, uint32_t) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return handler(context, thread, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

FakeCAM::FakeCAM(FakeThread& thread) {
    thread.name = "y2r:uThread";

    Result result;
    std::tie(result, output_vaddr) = thread.CallSVC(&OS::SVCControlMemory, 0, 0, 0x47000, 3/*ALLOC*/, 0x3/*RW*/);
    if (result != RESULT_OK) {
        throw std::runtime_error("Failed to allocate y2r:u output buffer");
    }

    std::tie(result, transfer_end_event) = thread.CallSVC(&OS::SVCCreateEvent, ResetType::OneShot);
    if (result != RESULT_OK)
        thread.CallSVC(&OS::SVCBreak, OS::BreakReason::Panic);
    transfer_end_event.second->name = "Y2RTransferEndEvent";

    IPCThread(*this, thread, "y2r:u", Y2RCommandHandler);
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakeCAM>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakeCAM>(os, setup, pid, name);
}

}  // namespace OS

}  // namespace HLE
