#pragma once

#include "debug_server.hpp"
#include "input.hpp"
#include "pica.hpp"
#include "interpreter.h"

#include <vulkan_utils/device_manager.hpp>

#include <framework/profiler.hpp>

class NetworkConsole;

namespace Loader {
class GameCard;
}

struct KeyDatabase;

struct EmuSession {
    Profiler::Profiler profiler;
    Debugger::DebugServer debug_server;

    // TODO: Does this still need to be a shared_ptr?
    std::shared_ptr<Interpreter::Setup> setup;

    PicaContext pica;

    InputSource input;
    std::pair<float, float> circle_pad { };

    std::thread emuthread;
    std::exception_ptr emuthread_exception = nullptr;

    std::unique_ptr<NetworkConsole> network_console;
    std::thread console_thread;

    EmuSession( LogManager&, Settings::Settings&,
                VulkanDeviceManager&, EmuDisplay::EmuDisplay&,
                const KeyDatabase&, std::unique_ptr<Loader::GameCard>);

    void Run();

    ~EmuSession();
};

std::unique_ptr<Loader::GameCard> LoadGameCard(spdlog::logger& logger, Settings::Settings& settings);
