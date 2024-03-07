#pragma once

#include "platform/fs.hpp"
#include "fake_process.hpp"

#include <memory>

namespace FileFormat {
struct ExHeader;
}

struct KeyDatabase;

namespace HLE {

// namespace OS {

namespace PXI {
namespace FS {
class Archive;
class File;
class FileContext;
}
}

namespace PXI {

/**
 * TODO: We impose the invariant that the static buffer data describing
 *       the physical memory chunks for this buffer may not be modified
 *       throughout the lifetime of instances of this structure. Can we make
 *       this invariant more explicit?
 */
struct PXIBuffer : IPC::StaticBuffer {
    struct ChunkDescriptor {
        uint32_t start_addr;
        uint32_t size_in_bytes;
    };

    PXIBuffer(const IPC::StaticBuffer& other);

    template<typename DataType>
    DataType Read(OS::Thread& thread, uint32_t offset) const;

    template<typename DataType>
    void Write(OS::Thread& thread, uint32_t offset, DataType data) const;

    uint32_t LookupAddress(OS::Thread& thread, uint32_t offset) const;

    // Cached version of chunk descriptors
    mutable std::vector<ChunkDescriptor> chunks;

    // Valid only when chunks is non-empty
    mutable uint32_t chunk_size = 0;
};

using ArchiveHandle = uint64_t;
using FileHandle = uint64_t;

using PMProgramHandle = Platform::PXI::PM::ProgramHandle;

struct Context {
    std::unordered_map<ArchiveHandle, std::unique_ptr<PXI::FS::Archive>> archives;
    ArchiveHandle next_archive_handle = 0;

    std::unordered_map<FileHandle, std::unique_ptr<PXI::FS::File>> files;
    FileHandle next_file_handle = 0;

    // Handle to be assigned to the next registered program
    PMProgramHandle next_program_handle = { 0x11233211 };

    // Map from PM internal program handle to program infos for the main title and the update title
    std::unordered_map<PMProgramHandle, std::pair<::Platform::FS::ProgramInfo, ::Platform::FS::ProgramInfo>> programs;

    // List of titles installed to the emulated NAND
    std::vector<uint64_t> nand_titles;
};

class FakePXI final {
    OS::OS& os;
    spdlog::logger& logger;

public:
    FakePXI(OS::FakeThread& thread);

    void FSThread(OS::FakeThread& thread, Context& context, const char* service_name);
    void PMThread(OS::FakeThread& thread, Context& context);
    void PSThread(OS::FakeThread& thread, Context& context);
    void MCThread(OS::FakeThread& thread, Context& context);
    void AMThread(OS::FakeThread& thread, Context& context);
};

std::optional<const char*> GetHLEModuleName(uint64_t title_id);

FileFormat::ExHeader GetExtendedHeader(OS::Thread&, const Platform::FS::ProgramInfo&);
FileFormat::ExHeader GetExtendedHeader(FS::FileContext&, const KeyDatabase&, FS::File& ncch_file);

}  // namespace PXI

// }  // namespace OS

}  // namespace HLE
