#pragma once

#include <framework/logging.hpp>

#include <memory.h>

#include <teakra/teakra.h>

namespace Memory {

struct DSPDevice : MemoryAccessHandler {
    std::shared_ptr<spdlog::logger> logger;

    DSPDevice(LogManager& log_manager);

//    Teakra::Teakra teakra;
};

} // namespace Memory
