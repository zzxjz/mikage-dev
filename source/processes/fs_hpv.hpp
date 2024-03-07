#pragma once

#include "os_hypervisor_private.hpp"

#include <platform/fs.hpp>

#include <boost/container/static_vector.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <variant>

namespace HLE {

namespace OS {

namespace HPV {

// TODO: This class could be shared with the actual emulation core
struct Path {
    static constexpr uint32_t max_length = 256; // Unverified. TODO: What's the actual maximum?

    using Utf8PathType = std::string;
    using BinaryPathType = boost::container::static_vector<uint8_t, max_length>;
    std::variant<Utf8PathType, BinaryPathType> data;

    Path(Thread&, uint32_t type, uint32_t addr, uint32_t num_bytes);
    Path(Thread&, uint32_t type, uint32_t num_bytes, std::function<uint8_t()> read_next_byte);

    std::string ToString() const;
};

struct Archive {
    virtual ~Archive() {
    }

    virtual std::string Describe() const = 0;
};

struct ProcessInfo {
    Platform::FS::ProgramInfo program;
    Platform::FS::StorageInfo storage;
};

struct FSContext : SessionContext {
    // Maps each session handle to the process ids registered on that handle
    std::unordered_map<Handle, ProcessId> process_ids;

    // Maps process ids to the ProcessInfo registered to that process via fs:LDR
    std::unordered_map<ProcessId, ProcessInfo> process_infos;

    /**
     * Maps each archive handle as used in the IPC interface to their
     * corresponding Archive object. Note that Archive ownership is shared
     * between this and various File instances, since e.g. via
     * OpenFileDirectly files may be opened without assigning a public
     * archive handle.
     */
    std::unordered_map<Platform::FS::ArchiveHandle, std::shared_ptr<Archive>> archives;
};

HPV::RefCounted<Object> CreateFSUserService(RefCounted<Port> port, FSContext& context);
HPV::RefCounted<Object> CreateFSLdrService(RefCounted<Port> port, FSContext& context);
HPV::RefCounted<Object> CreateFSRegService(RefCounted<Port> port, FSContext& context);

} // namespace HPV

} // namespace HOS

} // namespace HLE
