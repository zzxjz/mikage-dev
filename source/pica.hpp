#pragma once

#include <memory>

namespace spdlog {
class logger;
}

namespace vk {
class PhysicalDevice;
class Device;
class Queue;
}

namespace Pica {
struct Context;
class Renderer;
}

namespace Debugger {
class DebugServer;
}

namespace Settings {
struct Settings;
}

namespace Profiler {
class Profiler;
}

class InterruptListener;

namespace Memory {
struct PhysicalMemory;
}

class PicaContext {
public:
    PicaContext(std::shared_ptr<spdlog::logger>, Debugger::DebugServer&,
                Settings::Settings&, Memory::PhysicalMemory&,
                Profiler::Profiler&, vk::PhysicalDevice, vk::Device,
                uint32_t graphics_queue_index, vk::Queue render_graphics_queue);
    ~PicaContext();

    void InjectDependency(InterruptListener& listener);
    void InjectDependency(Memory::PhysicalMemory& memory);

    std::unique_ptr<Pica::Context> context;
    std::unique_ptr<Pica::Renderer> renderer;
};

// Interface functions so that we don't need to define Pica::Context in here...
Pica::Renderer* GetRenderer(Pica::Context&);
