#include "platform/am.hpp"
#include "platform/pxi.hpp"
#include "platform/sm.hpp"

#include "fake_process.hpp"

extern std::vector<uint64_t>* nand_titles;

namespace HLE {

namespace OS {

struct FakeAM {
    Handle pxiam_session;

    FakeAM(FakeThread&);

    static constexpr auto session_limit = 5;

    spdlog::logger& logger;
};


static OS::ResultAnd<uint32_t>
OnIPCGetDLCContentInfoCount(FakeThread&, FakeAM&, uint32_t /*media_type*/, uint64_t /*title_id*/) {
    // TODO: Actually write out the content info... Just stub-returning the count 1 for now
    return std::make_tuple(RESULT_OK, 1);
}

static OS::ResultAnd<uint32_t, IPC::MappedBuffer>
OnIPCListDLCContentInfos(   FakeThread&, FakeAM&, uint32_t /* number of content infos to read */,
                            uint32_t /*media_type*/, uint64_t /*title_id*/, uint32_t /* offset */,
                            IPC::MappedBuffer buffer) {
    // TODO: Actually write out the content info... Just stub-returning the count 1 for now
    return std::make_tuple(RESULT_OK, 1, buffer);
}

static OS::ResultAnd<IPC::MappedBuffer, IPC::MappedBuffer>
OnIPCGetDLCTitleInfos(  FakeThread&, FakeAM&, uint32_t /*media_type*/,
                        uint32_t /* number of titles */, IPC::MappedBuffer title_ids,
                        IPC::MappedBuffer output_buffer) {
    // TODO: Actually write out content infos
    return std::make_tuple(RESULT_OK, title_ids, output_buffer);
}

static OS::ResultAnd<uint32_t, IPC::MappedBuffer>
OnIPCListDataTitleTicketInfos(  FakeThread& thread, FakeAM&, uint32_t ticket_count,
                                uint64_t title_id, uint32_t /*offset*/,
                                IPC::MappedBuffer output_buffer) {

    if (ticket_count != 1) {
        throw std::runtime_error("Unsupported ticket count requested by ListDataTitleTicketInfos");
    }

    // TODO: Fill these out properly
    thread.WriteMemory32(output_buffer.addr, title_id & 0xffffffff);
    thread.WriteMemory32(output_buffer.addr + 4, title_id >> 32);
    thread.WriteMemory32(output_buffer.addr + 8, 0); // ticket id low
    thread.WriteMemory32(output_buffer.addr + 0xc, 0); // ticket id high
    thread.WriteMemory32(output_buffer.addr + 0x10, 0); // version
    thread.WriteMemory32(output_buffer.addr + 0x14, 0); // size

    return std::make_tuple(RESULT_OK, 1, output_buffer);
}

static OS::ResultAnd<uint32_t>
OnIPCGetNumPrograms(FakeThread& thread, FakeAM& context, uint32_t media_type) {
    auto num_programs = IPC::SendIPCRequest<Platform::PXI::AM::GetTitleCount>(thread, context.pxiam_session, media_type);
    return std::make_tuple(RESULT_OK, num_programs);
}

static OS::ResultAnd<uint32_t, IPC::MappedBuffer>
OnIPCGetProgramList(FakeThread& thread, FakeAM& context, uint32_t title_count, uint32_t media_type, IPC::MappedBuffer buffer) {
    auto num_written = IPC::SendIPCRequest<Platform::PXI::AM::GetTitleList>(thread, context.pxiam_session, title_count, media_type, IPC::StaticBuffer { buffer.addr, buffer.size, 0 });
    return std::make_tuple(RESULT_OK, num_written, buffer);
}

static OS::ResultAnd<uint32_t, IPC::MappedBuffer, IPC::MappedBuffer>
OnIPCGetProgramInfos(FakeThread& thread, FakeAM& context, uint32_t media_type, uint32_t title_count, IPC::MappedBuffer title_ids_buffer, IPC::MappedBuffer out_buffer) {
    IPC::SendIPCRequest<Platform::PXI::AM::GetTitleInfos>(  thread, context.pxiam_session, media_type, title_count,
                                                            IPC::StaticBuffer { title_ids_buffer.addr, title_ids_buffer.size, 0 },
                                                            IPC::StaticBuffer { out_buffer.addr, out_buffer.size, 1 });
    return std::make_tuple(RESULT_OK, title_count, title_ids_buffer, out_buffer);
}

static auto AppCommandHandler(FakeThread& thread, FakeAM& context, const Platform::IPC::CommandHeader& header) {
    using namespace Platform::AM;

    switch (header.command_id) {
    case GetDLCContentInfoCount::id:
        IPC::HandleIPCCommand<GetDLCContentInfoCount>(OnIPCGetDLCContentInfoCount, thread, thread, context);
        break;

    case ListDLCContentInfos::id:
        IPC::HandleIPCCommand<ListDLCContentInfos>(OnIPCListDLCContentInfos, thread, thread, context);
        break;

    case GetDLCTitleInfos::id:
        IPC::HandleIPCCommand<GetDLCTitleInfos>(OnIPCGetDLCTitleInfos, thread, thread, context);
        break;

    case ListDataTitleTicketInfos::id:
        IPC::HandleIPCCommand<ListDataTitleTicketInfos>(OnIPCListDataTitleTicketInfos, thread, thread, context);
        break;

    // TODO: These are actually am:sys commands

    case GetNumPrograms::id:
        IPC::HandleIPCCommand<GetNumPrograms>(OnIPCGetNumPrograms, thread, thread, context);
        break;

    case GetProgramList::id:
        IPC::HandleIPCCommand<GetProgramList>(OnIPCGetProgramList, thread, thread, context);
        break;

    case GetProgramInfos::id:
        IPC::HandleIPCCommand<GetProgramInfos>(OnIPCGetProgramInfos, thread, thread, context);
        break;

    case 0x13: // NeedsCleanup
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, false);
        break;

    case 0x8: // GetNumTickets
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, /*1*/nand_titles->size()); // No tickets
        break;

    // NOTE: Without this, HOME Menu will silently drop any titles reported on system versions 9.0.0 and above.
    case 0x9: // GetTicketList
    {
        auto num_tickets = std::min<uint32_t>(thread.ReadTLS(0x84), nand_titles->size());
        auto skip = thread.ReadTLS(0x88);
        if (skip != 0) {
            fprintf(stderr, "ERROR: skip count not zero\n");
            throw std::runtime_error("Error");
        }
        auto size = thread.ReadTLS(0x8c);
        auto addr = thread.ReadTLS(0x90);
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, /*2, 2*/ 4, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, num_tickets); // No tickets
        thread.WriteTLS(0x8c, size);
        thread.WriteTLS(0x90, addr);
//        thread.WriteMemory32(addr, /*0xe22f26aa*/ 00022000); // mset
//        thread.WriteMemory32(addr + 4, /*0x0004feda*/ 00040010);
for (int i = 0; i < num_tickets; ++i) {
        thread.WriteMemory32(addr + i * 8, /*0xe22f26aa*/ (*nand_titles)[i] & 0xffffffff); // mset
        thread.WriteMemory32(addr + 4 + i * 8, /*0x0004feda*/ (*nand_titles)[i] >> 32);
}
        break;
    }

    using QueryAvailableTitleDatabase = IPC::IPCCommand<0x19>::add_uint32::response::add_uint32;
    case QueryAvailableTitleDatabase::id:
        // TODO: Read input media type
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0); // not available
        break;


    using GetTwlArchiveResourceInfo = IPC::IPCCommand<0x20>::response::add_uint64::add_uint64::add_uint64::add_uint64;
    case GetTwlArchiveResourceInfo::id:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 9, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        // No free space whatsoever
        thread.WriteTLS(0x88, rand());
        thread.WriteTLS(0x8c, rand());
        thread.WriteTLS(0x90, rand());
        thread.WriteTLS(0x94, rand());
        thread.WriteTLS(0x98, rand());
        thread.WriteTLS(0x9c, rand());
        thread.WriteTLS(0xa0, rand());
        thread.WriteTLS(0xa4, rand());
        break;

    using DeleteAllImportContextsFiltered = IPC::IPCCommand<0x22>::add_uint32::add_uint32::response;
    case DeleteAllImportContextsFiltered::id:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0x408: // GetProgramInfoFromCIA
        // Queries information from a CIA file opened via fs
        // Home Menu uses this to query information about update partition CIAs when launching from a game card
        // TODO: Implement. For now, we just return some valid title id (the one mset has) to make subsequent IPC requests not error out
        // TODO: We can implement this now!
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 8, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0x00022000);
        thread.WriteTLS(0x8c, 0x40010);
        thread.WriteTLS(0x90, 0);
        thread.WriteTLS(0x94, 0);
        thread.WriteTLS(0x98, 0);
        thread.WriteTLS(0x9c, 0);
        thread.WriteTLS(0xa0, 0);
        break;

    using GetSystemUpdaterMutex = IPC::IPCCommand<0x412>::response::add_handle<IPC::HandleType::Mutex>;
    case GetSystemUpdaterMutex::id:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 3, 0).raw); // TODO: Return proper mutex
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0);
        thread.WriteTLS(0x8c, std::get<1>(thread.CallSVC(&OS::SVCCreateMutex, false)).first.value);
        break;

    default:
        throw std::runtime_error(fmt::format("AM: Unknown am:app/am:sys command with header {:#x}", header.raw));
    }

    return ServiceHelper::SendReply;
}

FakeAM::FakeAM(FakeThread& thread)
    : logger(*thread.GetLogger()) {

    // Get PxiFS0 service handle via srv:
    {
        auto [result,srv_session] = thread.CallSVC(&OS::OS::SVCConnectToPort, "srv:");
        if (result != RESULT_OK)
            thread.CallSVC(&OS::OS::SVCBreak, OS::OS::BreakReason::Panic);

        namespace SMSRV = Platform::SM::SRV;
        IPC::SendIPCRequest<SMSRV::RegisterClient>(thread, srv_session.first, IPC::EmptyValue{});

        pxiam_session = IPC::SendIPCRequest<SMSRV::GetServiceHandle>(   thread, srv_session.first,
                                                                        Platform::SM::PortName("pxi:am9"), 0);

        thread.CallSVC(&OS::OS::SVCCloseHandle, srv_session.first);
    }

    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, "am:net", session_limit));
    service.Append(ServiceUtil::SetupService(thread, "am:u", session_limit));
    service.Append(ServiceUtil::SetupService(thread, "am:app", session_limit));
    service.Append(ServiceUtil::SetupService(thread, "am:sys", session_limit));

    auto InvokeCommandHandler = [this](FakeThread& thread, uint32_t /* index of signalled handle */) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return AppCommandHandler(thread, *this, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakeAM>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakeAM>(os, setup, pid, name);
}

}  // namespace OS

}  // namespace HLE
