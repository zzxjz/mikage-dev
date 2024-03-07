#pragma once

#include <cstdint>
#include <functional>

namespace HLE {

namespace OS {

// Type of a virtual address
// TODO: Make this a strong typedef
using VAddr = uint32_t;

// Type of a physical address
// TODO: Make this a strong typedef
using PAddr = uint32_t;

// TODO: Make this a strong typedef
using ProcessId = uint32_t;

// TODO: Make this a strong typedef
using ThreadId = uint32_t;

/**
 * Type used to identify objects by a number internally
 * This is needed so that the emulated CPU core can pass around object handles
 * without actually storing objects in emulated memory.
 */
struct Handle {
    uint32_t value;

    bool operator < (const Handle& other) const {
        return value < other.value;
    }

    bool operator == (const Handle& other) const {
        return value == other.value;
    }

    bool operator != (const Handle& other) const {
        return value != other.value;
    }
};

const Handle HANDLE_INVALID = { 0 };

/**
 * Extension to the Handle type that stores additional debug information about
 * the handle. DebugHandle compares and hashes equal to the Handle it decays
 * to, so when it's used as a key into associative containers, regular Handles
 * are sufficient for lookup.
 */
struct DebugHandle : Handle {
    enum Source {
        Original,   // The owning process created the underlying object
        Duplicated, // The owning process duplicated an existing handle
        FromIPC,    // The owning process received the handle from another process via IPC
        Raw,        // Obtained from emulated application, hence no valid debug information is attached
    };

    struct DebugInfo {
        uint64_t created_time_stamp = 0;

        Source source = Raw;

        /// Header of the IPC request due to which the handle was transmitted (only valid if source is FromIPC)
        uint32_t ipc_command_header = 0;

        // TODO: For Duplicated, store the original Handle value
    } debug_info;

    DebugHandle() = default;

    DebugHandle(uint32_t value, DebugInfo debug_info) : Handle{value}, debug_info(debug_info) {}

    /// Create a DebugHandle from a plain handle, omitting the initialization of any debug information
    DebugHandle(const Handle& handle) : Handle(handle) {}

//    bool operator == (const DebugHandle& other) const {
//        return value == other.value;
//    }

//    bool operator == (const Handle& other) const {
//        return value == other.value;
//    }

//    bool operator != (const Handle& other) const {
//        return value != other.value;
//    }
};

} // namespace OS

} // namespace HLE

namespace std {
template<>
struct hash<HLE::OS::Handle> {
    auto operator()(const HLE::OS::Handle& handle) const {
        return std::hash<decltype(handle.value)>{}(handle.value);
    }
};
template<>
struct hash<HLE::OS::DebugHandle> {
    auto operator()(const HLE::OS::DebugHandle& handle) const {
        return std::hash<HLE::OS::Handle>{}(handle);
    }
};
}  // namespace std
