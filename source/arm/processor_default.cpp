#include "processor_default.hpp"

#include <range/v3/algorithm/fill.hpp>

namespace Interpreter {

PageTable::PageTable() noexcept {
    ranges::fill(physical_addresses, 0xffffffff);
}

void PageTable::Insert(Memory::PhysicalMemory& mem, uint32_t vstart, uint32_t pstart, uint32_t size) {
    auto first_page_index = (vstart >> 12);
    auto last_page_index = ((vstart + size - 1) >> 12); // Inclusive
    for (auto page_offset = 0; page_offset <= last_page_index - first_page_index; ++page_offset) {
        const auto physical_address = pstart + page_offset * 0x1000;
        physical_addresses[first_page_index + page_offset] = physical_address;
        virtual_addresses_for[physical_address >> 12].push_back((first_page_index + page_offset) << 12);
        if (auto memory = Memory::LookupMemoryBackedPage(mem, physical_address)) {
            host_memory[first_page_index + page_offset] = memory;
        }
    }
}

void PageTable::Remove(uint32_t vstart, uint32_t size) {
    // TODO: Assert vstart + size doesn't overflow the 32-bit range...

    {
        auto begin_it = &physical_addresses[vstart >> 12];
        auto end_it = &physical_addresses[(vstart + size) >> 12];
        std::transform(begin_it, end_it, begin_it,
                       [=](auto paddr) {
                           if (paddr == 0xffffffff) {
                               throw std::runtime_error("Couldn't find virtual memory mapping for removal");
                           }

                           virtual_addresses_for[paddr >> 12].clear();

                           return 0xffffffff;
                       });
    }

    // Remove host memory backed pages, if any
    {
        auto begin_it = &host_memory[vstart >> 12];
        auto end_it = &host_memory[(vstart + size) >> 12];
        std::fill(begin_it, end_it, Memory::HostMemoryBackedPage { nullptr });
    }
}

ProcessorWithDefaultMemory::ProcessorWithDefaultMemory(Interpreter::Setup& setup)
    : Memory::PhysicalMemorySubscriber(setup.mem), setup(setup) {
    // TODO: Initialize page_table based on current Memory state!
}

void ProcessorWithDefaultMemory::WriteVirtualMemory8(uint32_t virt_address, const uint8_t value) {
    WriteVirtualMemory(setup.mem, page_table, virt_address, value);
}

void ProcessorWithDefaultMemory::WriteVirtualMemory16(uint32_t virt_address, const uint16_t value) {
    WriteVirtualMemory(setup.mem, page_table, virt_address, value);
}

void ProcessorWithDefaultMemory::WriteVirtualMemory32(uint32_t virt_address, const uint32_t value) {
    WriteVirtualMemory(setup.mem, page_table, virt_address, value);
}

uint8_t ProcessorWithDefaultMemory::ReadVirtualMemory8(uint32_t virt_address) {
    return ReadVirtualMemory<uint8_t>(setup.mem, page_table, virt_address);
}

uint16_t ProcessorWithDefaultMemory::ReadVirtualMemory16(uint32_t virt_address) {
    return ReadVirtualMemory<uint16_t>(setup.mem, page_table, virt_address);
}

uint32_t ProcessorWithDefaultMemory::ReadVirtualMemory32(uint32_t virt_address) {
    return ReadVirtualMemory<uint32_t>(setup.mem, page_table, virt_address);
}

void ProcessorWithDefaultMemory::OnVirtualMemoryMapped(uint32_t phys_addr, uint32_t size, uint32_t vaddr) {
    page_table.Insert(setup.mem, vaddr, phys_addr, size);
}

void ProcessorWithDefaultMemory::OnVirtualMemoryUnmapped(uint32_t vaddr, uint32_t size) {
    page_table.Remove(vaddr, size);
}

void ProcessorWithDefaultMemory::OnBackedByHostMemory(Memory::HostMemoryBackedPage page, uint32_t physical_address) {
    for (auto& virtual_address : page_table.virtual_addresses_for[physical_address >> 12]) {
        auto& page_table_entry = page_table.host_memory[virtual_address >> 12];
        if (page_table_entry) {
            // TODO: When creating new processes, we need to initialize page_table based on the current memory state
//            throw std::runtime_error(fmt::format("Attempted to override page table cache entry at address {:#x} that already existed", physical_address));
        }
        page_table_entry = page;
    }
}

void ProcessorWithDefaultMemory::OnUnbackedByHostMemory(uint32_t physical_address) {
    for (auto& virtual_address : page_table.virtual_addresses_for[physical_address >> 12]) {
        auto& page_table_entry = page_table.host_memory[virtual_address >> 12];
        if (!page_table_entry) {
            throw std::runtime_error("Attempted to remove page table cache entry that does not exist");
        }
        page_table_entry = { };
    }
}

} // namespace Interpreter
