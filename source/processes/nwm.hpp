#pragma once

#include "ipc.hpp"
#include "os.hpp"

namespace HLE {

namespace OS {

class FakeThread;

struct FakeNWM {
    FakeNWM(FakeThread& thread);

    const uint32_t shared_mem_size = 0x22000;
    uint32_t shared_mem_vaddr;
    HandleTable::Entry<SharedMemoryBlock> shared_memory;
    HandleTable::Entry<Event> shared_memory_event;

    IPC::StaticBuffer soc_static_buffer;
};

}  // namespace OS

}  // namespace HLE
