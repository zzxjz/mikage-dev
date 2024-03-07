#pragma once

#include <interpreter.h>

#include <boost/container/small_vector.hpp>

namespace Interpreter {

class ExecutionContextWithDefaultMemory;

/**
 * Interface to access memory using virtual addresses.
 *
 * Exposes HostMemoryBackedPages that can directly be written to, and a
 * direct mapping from virtual addresses to physical ones to be used for slow
 * fallback paths in case the memory is not host memory backed.
 */
struct PageTable {
    PageTable() noexcept;

    void Insert(Memory::PhysicalMemory& mem, uint32_t vstart, uint32_t pstart, uint32_t size);

    void Remove(uint32_t vstart, uint32_t size);

    Memory::HostMemoryBackedPage LookupHostMemory(uint32_t vaddr) const {
        return host_memory[vaddr >> 12];
    }

    static constexpr uint32_t num_pages = (1 << 20);

    // List mapping virtual memory page indexes to their corresponding physical memory page
    // (one entry per page in the entire 32-bit address space)
    std::array<Memory::HostMemoryBackedPage, num_pages> host_memory;

    // List mapping virtual memory page indexes to their corresponding physical memory address
    std::array</*PAddr*/ uint32_t, num_pages> physical_addresses; // 0xffffffff signalizes an unmapped address

    /**
     * List mapping physical memory page indexes to a list of virtual memory addresses that are mapped to them
     * Typically, for each 3DS process there are at most 2 (usually just 1) virtual pages that map to the same physical page
     */
    std::array<boost::container::small_vector</*VAddr*/ uint32_t, 2>, num_pages> virtual_addresses_for;
};

class ProcessorWithDefaultMemory : public Processor, Memory::PhysicalMemorySubscriber {
protected:
    Setup& setup;
    PageTable page_table;

    ProcessorWithDefaultMemory(Interpreter::Setup& setup);

    friend class ExecutionContextWithDefaultMemory;

    static std::optional<uint32_t> TranslateVirtualAddress(const PageTable& page_table, uint32_t vaddr) {
        auto paddr_start = page_table.physical_addresses[vaddr >> 12];

        if (paddr_start == 0xffffffff) {
            throw std::runtime_error("Virtual address " + fmt::format("{:#x}", vaddr) + " is not mapped");
        }

        return paddr_start + (vaddr & 0xfff);
    }

    template<typename T>
    static void WriteVirtualMemory(Memory::PhysicalMemory& mem, const PageTable& page_table, uint32_t address, T value) {
        auto page = page_table.LookupHostMemory(address);
        if (page) {
            Memory::Write(page, address & 0xfff, value);
            return;
        }
        // Else fall back to slow handler-based write

        auto opt_paddr = TranslateVirtualAddress(page_table, address);
        if (!opt_paddr) {
            throw std::runtime_error(fmt::format("Invalid virtual address {:#x}", address));
        }

        Memory::WriteLegacy(mem, *opt_paddr, value);
    }

    template<typename T>
    static T ReadVirtualMemory(Memory::PhysicalMemory& mem, const PageTable& page_table, uint32_t address) {
        auto page = page_table.LookupHostMemory(address);
        if (page) {
            return Memory::Read<T>(page, address & 0xfff);
        }
        // Else fall back to slow handler-based read

        auto opt_paddr = TranslateVirtualAddress(page_table, address);
        if (!opt_paddr) {
            throw std::runtime_error(fmt::format("Invalid virtual address {:#x}", address));
        }

        return Memory::ReadLegacy<T>(mem, *opt_paddr);
    }

    // Narrow interface to the more refined return type ExecutionContextWithDefaultMemory
    ExecutionContext* CreateExecutionContextImpl() final;
    virtual ExecutionContextWithDefaultMemory* CreateExecutionContextImpl2() = 0;

    void OnBackedByHostMemory(Memory::HostMemoryBackedPage, uint32_t address) override;
    void OnUnbackedByHostMemory(uint32_t address) override;

public:
    Setup& GetSetup() {
        return setup;
    }

    void WriteVirtualMemory8(uint32_t virt_address, const uint8_t value) override;
    void WriteVirtualMemory16(uint32_t virt_address, const uint16_t value) override;
    void WriteVirtualMemory32(uint32_t virt_address, const uint32_t value) override;

    uint8_t ReadVirtualMemory8(uint32_t virt_address) override;
    uint16_t ReadVirtualMemory16(uint32_t virt_address) override;
    uint32_t ReadVirtualMemory32(uint32_t virt_address) override;

    void OnVirtualMemoryMapped(uint32_t phys_addr, uint32_t size, uint32_t vaddr) override;

    void OnVirtualMemoryUnmapped(uint32_t vaddr, uint32_t size) override;
};

class ExecutionContextWithDefaultMemory : public ExecutionContext {
    Memory::PhysicalMemory& mem;

    const PageTable& page_table;

    friend class ProcessorWithDefaultMemory;

protected:
    ExecutionContextWithDefaultMemory(Processor& parent_, Memory::PhysicalMemory& mem_)
        : ExecutionContext(parent_), mem(mem_), page_table(static_cast<ProcessorWithDefaultMemory&>(parent).page_table) {
    }

    std::optional<uint32_t> TranslateVirtualAddress(uint32_t vaddr) {
        return ProcessorWithDefaultMemory::TranslateVirtualAddress(page_table, vaddr);
    }

public:
    // Re-define memory accessors here to get better codegen:
    // The JIT engines must be able to emit calls to memory read handlers
    // without them being wrapped in unnecessary layers of function calls.

    template<typename T>
    void WriteVirtualMemory(uint32_t address, T value) {
        ProcessorWithDefaultMemory::WriteVirtualMemory<T>(mem, page_table, address, value);
    }

    template<typename T>
    T ReadVirtualMemory(uint32_t address) {
        return ProcessorWithDefaultMemory::ReadVirtualMemory<T>(mem, page_table, address);
    }
};

inline ExecutionContext* ProcessorWithDefaultMemory::CreateExecutionContextImpl() {
    return CreateExecutionContextImpl2();
}

} // namespace Interpreter
