/**
 * @file Memory interface used to represent and access emulated memory and
 *       MMIO devices.
 */

#pragma once

#include <spdlog/fmt/fmt.h>

#include <boost/endian/conversion.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

class LogManager;

class PicaContext;

class InputSource;

namespace EmuDisplay {
struct EmuDisplay;
}

/**
 * Memory namespace
 */
namespace Memory {

// TODO: Change this to a strongly typed enum to aid the compiler optimizer in pointer analysis
using EmulatedMemory = uint8_t;

using PAddr = uint32_t;

struct HostMemoryBackedPage {
    // nullptr if no memory backed page exists
    EmulatedMemory* data = nullptr;

    explicit operator bool() const {
        return (data != nullptr);
    }
};

// Set of contiguous pages in memory backed by host RAM
struct HostMemoryBackedPages {
    EmulatedMemory* data = nullptr;

    uint32_t num_bytes = 0;

    explicit operator bool() const {
        return (data != nullptr);
    }
};

/**
 * Type representing a the physical address range of a memory bus.
 */
template<uint32_t PAddrStart, uint32_t Size>
struct Bus {
    static constexpr uint32_t start = PAddrStart;
    static constexpr uint32_t size  = Size;
    static constexpr uint32_t end   = start + size;

    uint8_t Read8(uint32_t address);
    uint16_t Read16(uint32_t address);
    uint32_t Read32(uint32_t address);

    void Write8(uint32_t address, uint8_t value);
    void Write16(uint32_t address, uint16_t value);
    void Write32(uint32_t address, uint32_t value);

    static_assert(start < static_cast<uint32_t>(start + size), "End address wrapped due to overflow... wrong template parameters?");
};

enum class HookKind {
    Read,
    Write,
    ReadWrite
};

constexpr bool HasReadHook(HookKind kind) noexcept {
    return (kind == HookKind::Read || kind == HookKind::ReadWrite);
}

constexpr bool HasWriteHook(HookKind kind) noexcept {
    return (kind == HookKind::Write || kind == HookKind::ReadWrite);
}

constexpr HookKind AddReadHook(HookKind kind) noexcept {
    return (kind == HookKind::Write) ? HookKind::ReadWrite : HookKind::Read;
}

constexpr HookKind AddWriteHook(HookKind kind) noexcept {
    return (kind == HookKind::Read) ? HookKind::ReadWrite : HookKind::Write;
}

template<HookKind>
struct Hook;

using PAddr = uint32_t;

struct MemoryRange {
    PAddr start;
    uint32_t num_bytes;
};

struct HookBase {
    // Actual memory range this hook is active for
    MemoryRange range;

    // Other hook contained in this same page
    // Guaranteed to be non-overlapping and to cover only memory *after* this hook
    std::unique_ptr<HookBase> next {};
};

template<HookKind>
struct HookHandler;

template<> struct HookHandler<HookKind::Read> {
    virtual void OnRead(PAddr addr, uint32_t num_bytes) = 0;
};

template<> struct HookHandler<HookKind::Write> {
    virtual void OnWrite(PAddr addr, uint32_t num_bytes, uint32_t value) = 0;
};

using ReadHandler = HookHandler<HookKind::Read>;
using WriteHandler = HookHandler<HookKind::Write>;

// Arguments: Address and value size
template<>
struct Hook<HookKind::Read> : HookBase {
    Hook(MemoryRange range, ReadHandler& handler_) : HookBase { range }, handler(handler_) {}

    ReadHandler& handler;
};

// Arguments: Address, value, and value size
template<>
struct Hook<HookKind::Write> : HookBase {
    Hook(MemoryRange range, WriteHandler& handler_) : HookBase { range }, handler(handler_) {}

    WriteHandler& handler;
};

using ReadHook = Hook<HookKind::Read>;
using WriteHook = Hook<HookKind::Write>;

// TODO: Find optimal alignment (check VEC_SIZE in glibc? Might just be 64 for AVX2). Having any alignment at all helps the compiler generate better memset()s for initialization
template<uint32_t PAddrStart, uint32_t Size>
struct alignas(512) MemoryBus : Bus<PAddrStart, Size> {
    // NOTE: If we over-allocate this array (by adding 3 more entries than strictly necessary), we can avoid needing range checks! (or at least we can reshuffle them past the actual writes to benefit from speculative execution)
    uint8_t data[Size];

    std::array<std::unique_ptr<HookBase>, (Size >> 12)> write_hooks;
    std::array<std::unique_ptr<HookBase>, (Size >> 12)> read_hooks;

    // TODO: When no hooks are set, callers should always use direct memory
    //       access instead of calling the Read/Write* functions. Currently,
    //       ReadLegacy/WriteLegacy is used in many places still, but once
    //       that is deprecated we should assert that a hook is set when
    //       using Write*.

    uint8_t Read8(uint32_t address) {
        assert(address < this->end);
        if (auto* hook = LookupReadHookFor(address)) {
            hook->handler.OnRead(address, sizeof(uint8_t));
        } else {
            // TODO: See above
        }
        return data[address - PAddrStart];
    }
    uint16_t Read16(uint32_t address) {
        if (auto* hook = LookupReadHookFor(address)) {
            hook->handler.OnRead(address, sizeof(uint16_t));
        } else {
            // TODO: See above
        }

        // TODO: Endianness
        uint16_t ret;
        memcpy(&ret, &data[address - PAddrStart], sizeof(ret));
        return ret;
    }
    uint32_t Read32(uint32_t address) {
        if (auto* hook = LookupReadHookFor(address)) {
            hook->handler.OnRead(address, sizeof(uint32_t));
        } else {
            // TODO: See above
        }

        // TODO: Endianness
        uint32_t ret;
        memcpy(&ret, &data[address - PAddrStart], sizeof(ret));
        return ret;
    }

    void Write8(uint32_t address, uint8_t value) {
        assert(address < this->end);
        if (auto* hook = LookupWriteHookFor(address)) {
            hook->handler.OnWrite(address, sizeof(value), value);
        } else {
            // TODO: See above
        }
        data[address - PAddrStart] = value;
    }
    void Write16(uint32_t address, uint16_t value) {
        if (auto* hook = LookupWriteHookFor(address)) {
            hook->handler.OnWrite(address, sizeof(value), value);
        } else {
            // TODO: See above
        }
        // TODO: Endianness
        memcpy(&data[address - PAddrStart], &value, sizeof(value));
    }
    void Write32(uint32_t address, uint32_t value) {
        if (auto* hook = LookupWriteHookFor(address)) {
            hook->handler.OnWrite(address, sizeof(value), value);
        } else {
            // TODO: See above
        }
        // TODO: Endianness
        memcpy(&data[address - PAddrStart], &value, sizeof(value));
    }

private:
    friend HostMemoryBackedPages LookupContiguousMemoryBackedPage(struct PhysicalMemory&, PAddr, uint32_t);

    template<HookKind AccessMode>
    friend HostMemoryBackedPages LookupContiguousMemoryBackedPage(struct PhysicalMemory&, PAddr, uint32_t);

    WriteHook* LookupWriteHookFor(uint32_t address) noexcept {
        return static_cast<WriteHook*>(write_hooks[(address - PAddrStart) >> 12].get());
    }

    ReadHook* LookupReadHookFor(uint32_t address) noexcept {
        return static_cast<ReadHook*>(read_hooks[(address - PAddrStart) >> 12].get());
    }
};

struct MemoryAccessHandler {
    uint8_t Read8(uint32_t offset);
    uint16_t Read16(uint32_t offset);
    uint32_t Read32(uint32_t offset);

    void Write8(uint32_t offset, uint8_t value);
    void Write16(uint32_t offset, uint16_t value);
    void Write32(uint32_t offset, uint32_t value);
};

/**
 * Proxy bus descriptor structure that forwards all reads and writes to a
 * handler given by T. This allows us to define MMIO handlers in external
 * translation units and forward-declaring them in this file instead of
 * defining them all in this header.
 * @tparam T handler structure to forward Read/Write function calls to. Must be a base of MemoryAccessHandler
 */
template<typename T, uint32_t PAddrStart, uint32_t Size>
struct ProxyBus : MemoryBus<PAddrStart, Size> {
    std::unique_ptr<T> handler;

    // Constructors and destructors are defined outside the header since T is
    // not a complete type in most translation units (which is necessary to
    // implement this destructor).
    ProxyBus(std::unique_ptr<T> handler);
    ProxyBus(ProxyBus&& bus);
    ~ProxyBus();

    uint8_t Read8(uint32_t address);
    uint16_t Read16(uint32_t address);
    uint32_t Read32(uint32_t address);

    void Write8(uint32_t address, uint8_t value);
    void Write16(uint32_t address, uint16_t value);
    void Write32(uint32_t address, uint32_t value);
};

struct MPCorePrivate;
struct HID;
struct LCD;
struct AXIHandler;
struct GPU;
struct HASH;
struct GPIO;
struct SPI;
struct CONFIG11;
struct DSPMMIO;

struct DSPMemory;

using VRAM  = MemoryBus<0x18000000, 0x00600000>;
//using DSP   = MemoryBus<0x1ff00000, 0x00080000>;
using DSP   = ProxyBus<DSPMemory, 0x1ff00000, 0x00080000>;
using AXI   = MemoryBus<0x1ff80000, 0x00080000>;
//using FCRAM = MemoryBus<0x20000000, 0x08000000>;
// Upper 0x08000000 bytes only available on New3DS!
// using FCRAM = MemoryBus<0x20000000, 0x10000000>;
using FCRAM = MemoryBus<0x20000000, 0x0800'0000>;

using IO_HASH = ProxyBus<HASH,         0x10101000, 0x1000>;
using IO_DSP1 = ProxyBus<DSPMMIO,      0x10103000, 0x1000>; // TODO: Actually CSND registers

using IO_CONFIG11 = ProxyBus<CONFIG11, 0x10140000, 0x2000>;
using IO_SPIBUS2 = ProxyBus<SPI,       0x10142000, 0x1000>; // SPI devices 3/4/5
using IO_SPIBUS3 = ProxyBus<SPI,       0x10143000, 0x1000>; // SPI device 6
using IO_HID = ProxyBus<HID,           0x10146000, 0x1000>;
using IO_GPIO = ProxyBus<GPIO,         0x10147000, 0x1000>;

using IO_SPIBUS1 = ProxyBus<SPI,       0x10160000, 0x1000>; // SPI devices 0/1/2 (3/4/5 at 0x10142000, 6 at 0x10143000)

using IO_LCD = ProxyBus<LCD,           0x10202000, 0x1000>;
using IO_DSP2 = ProxyBus<DSPMMIO,      0x10203000, 0x1000>; // TODO: Is this indeed just a mirror of 0x10103000? TODO: Actually the real DSP registers
using IO_AXI = ProxyBus<AXIHandler,    0x1020F000, 0x1000>;

using IO_HASH2 = ProxyBus<HASH,        0x10301000, 0x1000>; // NOTE: This is *NOT* just a mirror of 0x10101000 ... (3dbrew erratum)

using IO_GPU = ProxyBus<GPU,           0x10400000, 0x2000>;

using MPCorePrivateBus = ProxyBus<MPCorePrivate, 0x17e00000, 0x00002000>;


struct PhysicalMemorySubscriber;

struct PhysicalMemory {
    using Busses = std::tuple<FCRAM, VRAM, DSP, AXI, IO_HID, IO_LCD, IO_AXI,
                               IO_GPU, IO_HASH,
                               IO_CONFIG11, IO_DSP1,
                               IO_SPIBUS2, IO_SPIBUS3,
                               IO_HASH2, IO_GPIO, IO_SPIBUS1,
                               IO_DSP2, MPCorePrivateBus>;

    Busses memory;

    std::vector<PhysicalMemorySubscriber*> subscribers;

    /**
     * @note This constructor leaves the memory system in a partially
     *       unintialized state. To finalize initialization, some of the busses
     *       need to be connected to their respective end points.
     */
    PhysicalMemory(LogManager& log_manager);

    void InjectDependency(PicaContext& pica);
    void InjectDependency(InputSource&);
};

struct PhysicalMemorySubscriber {
    PhysicalMemorySubscriber(PhysicalMemory& mem_) : mem(mem_) {
        mem.subscribers.push_back(this);
    }

    PhysicalMemory& mem;

    virtual ~PhysicalMemorySubscriber() {
        for (auto it = mem.subscribers.begin(); it != mem.subscribers.end(); ++it) {
            if (*it == this) {
                mem.subscribers.erase(it);
                break;
            }
        }
    }

    // Called when a MemoryBus in PhysicalMemory transitions from
    // "not backed by host memory" to "backed by host memory" state
    virtual void OnBackedByHostMemory(HostMemoryBackedPage page, uint32_t address) {
        // Do nothing by default
    }

    // Called when a MemoryBus in PhysicalMemory transitions from
    // "backed by host memory" to "not backed by host memory" state
    virtual void OnUnbackedByHostMemory(uint32_t address) {
        // Do nothing by default
    }
};

struct IsInside {
    uint32_t address;

    template<typename Bus>
    bool operator()(const Bus& bus) const {
        return address >= bus.start && address < bus.end;
    }
};

namespace detail {

/// Utility functor to call the appropriate Read8/Read16/Read32 variant based on the given data type
template<typename DataType>
struct BusReader {
    uint32_t address;

    template<typename Bus>
    DataType operator()(Bus& bus) const {
        static_assert(std::is_same_v<DataType, uint8_t> || std::is_same_v<DataType, uint16_t> || std::is_same_v<DataType, uint32_t>, "Invalid DataType");
        if constexpr (std::is_same<DataType, uint8_t>::value) {
            return bus.Read8(address);
        } else if constexpr (std::is_same<DataType, uint16_t>::value) {
            return bus.Read16(address);
        } else if constexpr (std::is_same<DataType, uint32_t>::value) {
            return bus.Read32(address);
        }
    }
};

/// Utility functor to call the appropriate Read8/Read16/Read32 variant based on the given data type
template<typename DataType>
struct BusWriter {
    uint32_t address;

    template<typename Bus>
    void operator()(Bus& bus, DataType value) const {
        static_assert(std::is_same_v<DataType, uint8_t> || std::is_same_v<DataType, uint16_t> || std::is_same_v<DataType, uint32_t>, "Invalid DataType");
        if (std::is_same<DataType, uint8_t>::value) {
            return bus.Write8(address, value);
        } else if (std::is_same<DataType, uint16_t>::value) {
            return bus.Write16(address, value);
        } else if (std::is_same<DataType, uint32_t>::value) {
            return bus.Write32(address, value);
        }
    }
};

template<typename DataType, typename BusTuple, size_t... Idxs>
DataType ReadHelper(PhysicalMemory& mem, uint32_t address, std::index_sequence<Idxs...>) {
    DataType data;
    auto attempt_read = [&](auto&& bus) {
        if (IsInside{address}(bus)) {
            data = BusReader<DataType>{address}(bus);
            return true;
        } else {
            return false;
        }
    };

    // Using short-circuit evaluation here to keep testing until we find the right bus (but no further than that)
    bool match_found = (attempt_read(std::get<Idxs>(mem.memory)) || ...);
    if (!match_found)
        throw std::runtime_error(fmt::format("Read from unknown physical address {:#010x}", address));

    return data;
}

template<typename DataType, typename BusTuple, size_t... Idxs>
void WriteHelper(PhysicalMemory& mem, uint32_t address, DataType value, std::index_sequence<Idxs...>) {
    auto attempt_write = [&](auto&& bus) {
        if (IsInside{address}(bus)) {
            BusWriter<DataType>{address}(bus, value);
            return true;
        } else {
            return false;
        }
    };

    // Using short-circuit evaluation here to keep testing until we find the right bus (but no further than that)
    bool match_found = (attempt_write(std::get<Idxs>(mem.memory)) || ...);
    if (!match_found)
        throw std::runtime_error(fmt::format("Write to unknown physical address {:#010x}", address));
}

template<typename Bus, typename Callback>
bool ForEachMemoryBusHelper(Bus& bus, Callback& callback) {
    if constexpr (std::is_base_of_v<MemoryBus<Bus::start, Bus::size>, Bus>) {
        return callback(bus);
    }

    return false;
}

// Iterates over all MemoryBusses, calling callback on each of them until it returns true
template<typename Callback, typename... Busses>
bool ForEachMemoryBus(std::tuple<Busses...>& busses, Callback& callback) {
    return (ForEachMemoryBusHelper(std::get<Busses>(busses), callback) || ...);
}

template<typename Bus>
HostMemoryBackedPage GetMemoryBackedPageFor(Bus& bus, PAddr address) {
    return { bus.data + (address - bus.start) };
}

} // namespace detail

/**
 * Read from PhysicalMemory under the constraint that only the address ranges on
 * the BusTuple are considered.
 * @throws std::runtime_error when the given address is outside any of the
 *         given Bus address ranges
 */
template<typename DataType, typename BusTuple = PhysicalMemory::Busses>
DataType ReadLegacy(PhysicalMemory& mem, uint32_t address) {
    // TODO: Statically assert that BusTuple is a subtuple of mem.busses
    // TODO: Restrict search to the given BusTuple
    constexpr size_t length = std::tuple_size<BusTuple>::value;
    return detail::ReadHelper<DataType, BusTuple>(mem, address, std::make_index_sequence<length>{});
}

/**
 * Write to PhysicalMemory under the constraint that only the address ranges on
 * the BusTuple are considered.
 * @throws std::runtime_error when the given address is outside any of the
 *         given Bus address ranges
 */
template<typename DataType, typename BusTuple = PhysicalMemory::Busses>
void WriteLegacy(PhysicalMemory& mem, uint32_t address, DataType value) {
    // TODO: Statically assert that BusTuple is a subtuple of mem.busses
    // TODO: Restrict search to the given BusTuple
    constexpr size_t length = std::tuple_size<BusTuple>::value;
    detail::WriteHelper<DataType, BusTuple>(mem, address, value, std::make_index_sequence<length>{});
}

template<uint32_t PAddrStart, uint32_t Size>
inline constexpr bool IsMemoryBus(const MemoryBus<PAddrStart, Size>&) {
    return true;
}

template<typename T>
inline constexpr bool IsMemoryBus(const T&) {
    return false;
}

inline HostMemoryBackedPage LookupMemoryBackedPage(PhysicalMemory& mem, PAddr address) {
    HostMemoryBackedPage out_page = { nullptr };
    auto callback = [&](auto& bus) {
        if (IsInside{address}(bus)) {
            // Return null page if this is not a memory bus
            // TODO: Also return null page if any hooks are set up
//            if (!bus.write_hooks[(address - bus.start) >> 12] && !bus.read_hooks[(address - bus.start) >> 12]) {
            if (IsMemoryBus(bus)) {
                out_page = detail::GetMemoryBackedPageFor(bus, address);
            }
            return true;
        }

        return false;
    };

    detail::ForEachMemoryBus(mem.memory, callback);
    return out_page;
}

// Deprecated since this doesn't check for or trigger any memory hooks
[[deprecated]] HostMemoryBackedPages LookupContiguousMemoryBackedPage(PhysicalMemory& mem, PAddr address, uint32_t num_bytes);

template<HookKind AccessMode>
HostMemoryBackedPages LookupContiguousMemoryBackedPage(PhysicalMemory& mem, PAddr address, uint32_t num_bytes);

template<typename DataType>
DataType Read(HostMemoryBackedPage page, uint32_t offset) noexcept {
    static_assert(std::is_same_v<DataType, uint8_t> || std::is_same_v<DataType, uint16_t> || std::is_same_v<DataType, uint32_t>, "Invalid DataType");
    if constexpr (std::is_same_v<DataType, uint8_t>) {
        return page.data[offset];
    } else {
        DataType ret;
        memcpy(&ret, &page.data[offset], sizeof(ret));
        return boost::endian::little_to_native(ret);
    }
}

template<typename DataType>
DataType Read(HostMemoryBackedPages pages, uint32_t offset) noexcept {
    return Read<DataType>(HostMemoryBackedPage { pages.data }, offset);
}

template<typename DataType>
void Write(HostMemoryBackedPage page, uint32_t offset, DataType value) noexcept {
    static_assert(std::is_same_v<DataType, uint8_t> || std::is_same_v<DataType, uint16_t> || std::is_same_v<DataType, uint32_t>, "Invalid DataType");
    if constexpr (std::is_same_v<DataType, uint8_t>) {
        page.data[offset] = value;
    } else {
        value = boost::endian::native_to_little(value);
        memcpy(&page.data[offset], &value, sizeof(value));
    }
}

template<typename DataType>
void Write(HostMemoryBackedPages pages, uint32_t offset, DataType value) noexcept {
    Write(HostMemoryBackedPage { pages.data }, offset, value);
}

/**
 * Assigns the given Hook to all pages in the specified address range.
 *
 * @pre The address range may not cross a bus boundary
 */
template<HookKind Kind>
void SetHook(PhysicalMemory&, PAddr start, uint32_t num_bytes, HookHandler<Kind>&);

template<HookKind Kind, typename F>
auto GenerateHooks(PhysicalMemory& mem, PAddr start, uint32_t num_bytes, const F& callback)
    -> std::enable_if_t<std::is_invocable_r_v<HookHandler<Kind>*, F, PAddr>> {
    static_assert(  Kind == HookKind::Read || Kind == HookKind::Write,
                    "Must pick either read or write hooks");

    uint32_t end_page_index = ((start & 0xfff) + 0x1000 + num_bytes - 1) >> 12;
    for (uint32_t page_index = 0; page_index < end_page_index; ++page_index) {
        PAddr page_start = ((start >> 12) + page_index) << 12;
        // Pass 1 as the region size for SetWriteHook since it's not used for single-page assignments anyway
        PAddr hook_range_start = page_index == 0 ? start : page_start;
        PAddr hook_range_end = (page_index == end_page_index - 1) ? start + num_bytes : (page_start + 0x1000);
        auto hook = callback(page_start);
        if (hook) {
            SetHook<Kind>(mem, hook_range_start, (hook_range_end - hook_range_start), *hook);
        }
    }
}

// TODO: Should read the expected HookHandler<Kind>& for validation
void ClearHook(PhysicalMemory& mem, HookKind kind, PAddr start, uint32_t num_bytes);

} // namespace Memory
