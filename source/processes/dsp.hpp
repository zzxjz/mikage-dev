#pragma once

#include "fake_process.hpp"

namespace HLE {

namespace OS {

class FakeDSP final {
    OS& os;
    spdlog::logger& logger;

    /// Signalled when the DSP interrupt fires (which will be reported either through channel_event or semaphore_event)
    Handle interrupt_event_new = HANDLE_INVALID;

    /// Signalled when the DSP is done processing the current command
    /// TODO: There should be several slots of these
    Handle channel_event = HANDLE_INVALID;

    /// Signalled by a client when they have written to DSP shared memory
    Handle semaphore_event;

    uint32_t pipe_address;

    PAddr pipe_info_base = 0; // Address of SubPipeInfo headers

    VAddr static_buffer_addr = 0;

    uint16_t semaphore_mask = 0;

    // TODO: Should check via DSP MMIO instead
    bool semaphore_signaled = false;
    bool data_signaled = false;

    enum class State {
        Running,
        Stopped,
        Sleeping
    } state = State::Stopped;

public:
    FakeDSP(FakeThread& thread);

    void OnIPCRequest(FakeThread& thread, Handle sender, const IPC::CommandHeader& header);

    std::tuple<OS::Result> HandleWriteProcessPipe(FakeThread&, uint32_t pipe_index, uint32_t num_bytes, IPC::StaticBuffer);
    std::tuple<OS::Result,uint32_t,IPC::StaticBuffer> HandleReadPipeIfPossible(FakeThread&, uint32_t pipe_index, uint32_t direction, uint32_t num_bytes);
    std::tuple<OS::Result,uint32_t,IPC::MappedBuffer> HandleLoadComponent(FakeThread& thread, uint32_t size, uint32_t program_mask, uint32_t data_mask, const IPC::MappedBuffer component_buffer);

    std::tuple<OS::Result> HandleFlushDataCache(FakeThread& thread, uint32_t start, uint32_t num_bytes, Handle process);
    std::tuple<OS::Result> HandleInvalidateDataCache(FakeThread& thread, uint32_t start, uint32_t num_bytes, Handle process);

    std::tuple<OS::Result,Handle> HandleGetSemaphoreEventHandle(FakeThread& thread);
};

}  // namespace OS

}  // namespace HLE
