#pragma once

#include <platform/crypto.hpp>

#include <filesystem>

namespace spdlog {
class logger;
}

KeyDatabase LoadKeyDatabase(spdlog::logger&, const std::filesystem::path& filename);
