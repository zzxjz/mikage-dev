#pragma once

#include "../processes/pxi_fs.hpp"

namespace FileSystem {

std::unique_ptr<HLE::PXI::FS::File> OpenNativeFile(int file_desctiptor);

} // namespace FileSystem
