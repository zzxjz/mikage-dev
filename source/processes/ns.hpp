#pragma once

#include "fake_process.hpp"

namespace FileFormat {
struct ExHeader;
}

namespace HLE {

namespace OS {

class OS;
class FakeThread;

// TODO: Remove this function once BootThread doesn't need access to it anymore
OS::ResultAnd<uint32_t> LaunchTitleInternal(FakeThread& source, bool from_firm, uint64_t title_id, uint32_t flags);

/**
 * Creates a process from the code in the given NCCH file.
 *
 * Performs minimal setup, but doesn't register the process to any services.
 */
HandleTable::Entry<Process> LoadProcessFromFile(FakeThread&,
                                                bool from_firm,
                                                const FileFormat::ExHeader&,
                                                std::unique_ptr<HLE::PXI::FS::File> ncch_file, bool is_exefs = false /* TODO: Get rid of this */);

}  // namespace OS

}  // namespace HLE
