#include "csnd.hpp"

#include "platform/csnd.hpp"

namespace HLE {

namespace OS {

struct FakeCSND {
    uint32_t shared_mem_vaddr;
    HandleTable::Entry<Mutex> shared_mem_mutex;

    // TODO: What size is this supposed to be?
    static constexpr uint32_t shared_mem_size = 0x3000;

    FakeCSND(FakeThread& thread);

    static HandleTable::Entry<SharedMemoryBlock>
    AllocateSharedMemory(FakeThread& thread, uint32_t size);
};

static OS::ResultAnd<std::array<Handle, 2>>
CSNDInitialize(FakeThread& thread, FakeCSND& context, uint32_t shared_mem_size, uint32_t, uint32_t, uint32_t, uint32_t) {
//    if (shared_mem_size != context.shared_mem_size) {

//    }
    shared_mem_size = context.shared_mem_size; // TODO: Erm.. but well, we *do* need to round the inputs size up!

    auto [result,shared_mem_block] = thread.CallSVC(&OS::SVCCreateMemoryBlock, context.shared_mem_vaddr, shared_mem_size, 0x3/*RW*/, 0x3/*RW*/);
    if (result != RESULT_OK) {
        throw std::runtime_error("SVCCreateMemoryBlock failed");
    }

    return std::make_tuple(RESULT_OK, std::array<Handle, 2> { context.shared_mem_mutex.first, shared_mem_block.first });
}

static OS::ResultAnd<>
CSNDFlushDataCache(FakeThread& thread, FakeCSND&, uint32_t addr, uint32_t num_bytes, Handle process) {
    thread.CallSVC(&OS::SVCFlushProcessDataCache, process, addr, num_bytes);

    thread.CallSVC(&OS::SVCCloseHandle, process);

    return { RESULT_OK };
}

static auto CommandHandler(FakeThread& thread, FakeCSND& context, const IPC::CommandHeader& header) {
    namespace CSND = Platform::CSND;

    switch (header.command_id) {
    case CSND::Initialize::id:
        IPC::HandleIPCCommand<CSND::Initialize>(CSNDInitialize, thread, thread, context);
        break;

    case 2: // Shutdown
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 3: // ExecuteCommands
    {
        // Set finished


        auto command_offset = thread.ReadTLS(0x84);
        // NOTE: Shared memory mapped at addr=0x10004000 in appleted
        auto val = thread.ReadMemory32(context.shared_mem_vaddr + command_offset + 4);
        thread.WriteMemory32(context.shared_mem_vaddr + command_offset + 4, val | 1);

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 5:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0xffffff00);
        break;

    case 6:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case CSND::FlushDataCache::id:
        IPC::HandleIPCCommand<CSND::FlushDataCache>(CSNDFlushDataCache, thread, thread, context);
        break;

    default:
        throw std::runtime_error(fmt::format("Unknown csnd:SND service command with header {:#010x}", header.raw));
    }

    return ServiceHelper::SendReply;
}


FakeCSND::FakeCSND(FakeThread& thread) {
    OS::Result result;

    std::tie(result, shared_mem_vaddr) = thread.CallSVC(&OS::SVCControlMemory, 0, 0, shared_mem_size, 3/*ALLOC*/, 0x3/*RW*/);
    if (result != RESULT_OK) {
        throw std::runtime_error("FakeCSND failed to allocate shared memory");
    }

    std::tie(result, shared_mem_mutex) = thread.CallSVC(&OS::SVCCreateMutex, false);
    shared_mem_mutex.second->name = "CSNDMutex";
    if (result != RESULT_OK) {
        throw std::runtime_error("Failed to create CSND mutex");
    }

    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, "csnd:SND", 1));

    auto InvokeCommandHandler = [&](FakeThread& thread, uint32_t /*signalled_handle_index*/) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return CommandHandler(thread, *this, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakeCSND>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakeCSND>(os, setup, pid, name);
}

} // namespace OS

} // namespace HLE
