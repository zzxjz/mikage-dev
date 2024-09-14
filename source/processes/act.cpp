#include "act.hpp"

#include <framework/exceptions.hpp>

namespace HLE {

namespace OS {

struct FakeACT {
    FakeACT(FakeThread& thread);
};

static auto ACTCommandHandler(FakeThread& thread, FakeACT&, std::string_view service_name, const IPC::CommandHeader& header) {
    using Initialize = IPC::IPCCommand<0x1>::add_uint32::add_uint32::add_process_id::add_handle<IPC::HandleType::SharedMemoryBlock>::response;

    using GetAccountDataBlock = IPC::IPCCommand<0x6>::add_uint32::add_uint32::add_uint32::add_buffer_mapping_write::response;

//    // TODO: Unknown what reply this sends
//    // NOTE: Contrary to what the name (taken from 3dbrew) suggests, the client sends the event
//    using GetNZoneBeaconNotFoundEvent = IPC::IPCCommand<0x2f>::add_process_id::add_handle<IPC::HandleType::Event>::response;

//    using SetClientVersion = IPC::IPCCommand<0x40>::add_uint32::add_process_id::response;

    switch (header.command_id) {
    case Initialize::id:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case GetAccountDataBlock::id:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0xd:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 10, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0);
        thread.WriteTLS(0x8c, 0);
        thread.WriteTLS(0x90, 0);
        thread.WriteTLS(0x94, 0);
        thread.WriteTLS(0x98, 0);
        thread.WriteTLS(0x9c, 0);
        thread.WriteTLS(0xa0, 0);
        thread.WriteTLS(0xa4, 0);
        thread.WriteTLS(0xa8, 0);
        break;

    default:
        throw Mikage::Exceptions::NotImplemented("Unknown {} service command with header {:#010x}", service_name, header.raw);
    }

    return ServiceHelper::SendReply;
}

static void ACTUThread(FakeACT& context, FakeThread& thread) {
    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, "act:u", 2));
    service.Append(ServiceUtil::SetupService(thread, "act:a", 2));

    auto InvokeCommandHandler = [&](FakeThread& thread, uint32_t /*signalled_handle_index*/) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return ACTCommandHandler(thread, context, "act:a/u", header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

FakeACT::FakeACT(FakeThread& thread) {
    thread.name = "ACUThread";

    ACTUThread(*this, thread);
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakeACT>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakeACT>(os, setup, pid, name);
}

} // namespace OS

} // namespace HLE
