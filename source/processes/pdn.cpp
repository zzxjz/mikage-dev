#include "pdn.hpp"

#include <framework/exceptions.hpp>

#include <teakra/teakra.h>

extern Teakra::Teakra* g_teakra; // TODO: Remove

namespace HLE {

namespace OS {

const uint32_t session_limit = 1;

struct FakePDN final {
    spdlog::logger& logger;

public:
    FakePDN(FakeThread& thread);

    void ServiceThread( FakeThread& thread, const char* service_name,
                        decltype(ServiceHelper::SendReply) (*command_handler)(FakeThread&, FakePDN&, const IPC::CommandHeader&));
};

static decltype(ServiceHelper::SendReply) CommandHandlerD(FakeThread& thread, FakePDN&, const IPC::CommandHeader& header) {
    using ResetDSP = IPC::IPCCommand<0x1>::add_uint32::add_uint32::add_uint32::response;

    switch (header.command_id) {
    case ResetDSP::id:
    {
        uint32_t enable = thread.ReadTLS(0x84);
        uint32_t reset = thread.ReadTLS(0x88);
        if (!enable) {
            // TODO: What to do here?
            // TODO: Do we still need to reset the DSP if the corresponding flag is set?
//            g_dsp_running = false;
        }
        if (reset) {
//            Memory::WriteLegacy<uint16_t>(thread.GetOS().setup.mem, Memory::IO_DSP2::start + 0x8, 1);
//            Memory::WriteLegacy<uint16_t>(thread.GetOS().setup.mem, Memory::IO_DSP2::start + 0x8, 0);

            // TODO: Reset via MMIO control register instead
//            std::vector<std::uint8_t> temp(0x80000);
//            memcpy(temp.data(), g_teakra->GetDspMemory().data(), temp.size());
//            g_teakra->Reset();
//            memcpy(g_teakra->GetDspMemory().data(), temp.data(), temp.size());

//            // TODO: Shouldn't need to sync memory here
//            for (uint32_t word_index = 0; word_index < Memory::DSP::size / sizeof(uint16_t); ++word_index) {
//                g_teakra->ProgramWrite(word_index, Memory::ReadLegacy<uint16_t>(thread.GetOS().setup.mem, Memory::DSP::start + word_index * sizeof(uint16_t)));
//            }

//            if (enable) {
//                g_dsp_running = true; // TODO: Remove
//                g_dsp_just_reset = true;
//            }
        }

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    default:
        throw Mikage::Exceptions::NotImplemented("Unknown pdn:d command request with header {:#010x}", header.raw);
    }

    return ServiceHelper::SendReply;
}

static decltype(ServiceHelper::SendReply) CommandHandlerI(FakeThread&, FakePDN&, const IPC::CommandHeader& header) {
    switch (header.command_id) {
    default:
        throw Mikage::Exceptions::NotImplemented("Unknown pdn:i command request with header {:#010x}", header.raw);
    }

    return ServiceHelper::SendReply;
}

static decltype(ServiceHelper::SendReply) CommandHandlerG(FakeThread& thread, FakePDN& context, const IPC::CommandHeader& header) {
    switch (header.command_id) {
    case IPC::IPCCommand<0x1>::add_uint32::add_uint32::add_uint32::id:
    {
        context.logger.info("{}received command 0x1", ThreadPrinter{thread});
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    default:
        throw Mikage::Exceptions::NotImplemented("Unknown pdn:g command request with header {:#010x}", header.raw);
    }

    return ServiceHelper::SendReply;
}

void FakePDN::ServiceThread(FakeThread& thread, const char* service_name, decltype(ServiceHelper::SendReply) (*command_handler)(FakeThread&, FakePDN&, const IPC::CommandHeader&)) {
    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, service_name, session_limit));

    auto InvokeCommandHandler = [&](FakeThread& thread, uint32_t /*signalled_handle_index*/) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return command_handler(thread, *this, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

FakePDN::FakePDN(FakeThread& thread)
    : logger(*thread.GetLogger()) {

    {
        auto new_thread = std::make_shared<WrappedFakeThread>(  thread.GetParentProcess(),
                                                                [this](FakeThread& thread) { ServiceThread(thread, "pdn:d", CommandHandlerD); });
        new_thread->name = "pdn:dThread";
        thread.GetParentProcess().AttachThread(new_thread);
    }

    {
        auto new_thread = std::make_shared<WrappedFakeThread>(  thread.GetParentProcess(),
                                                                [this](FakeThread& thread) { ServiceThread(thread, "pdn:i", CommandHandlerI); });
        new_thread->name = "pdn:iThread";
        thread.GetParentProcess().AttachThread(new_thread);
    }

    thread.name = "pdn:gThread";
    ServiceThread(thread, "pdn:g", CommandHandlerG);
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakePDN>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakePDN>(os, setup, pid, name);
}

}  // namespace OS

}  // namespace HLE
