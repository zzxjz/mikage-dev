// video_core has its own version of BitField that collides with our BitField. Hence, we initialize the video_core context in this separate translation unit, where we don't include BitField.

#include "pica.hpp"
#include "video_core/src/video_core/vulkan/renderer.hpp"

#include "video_core/src/video_core/context.h"

#include <framework/profiler.hpp>

#include <spdlog/logger.h>

PicaContext::PicaContext(   std::shared_ptr<spdlog::logger> logger, Debugger::DebugServer& debug_server,
                            Settings::Settings& settings, Memory::PhysicalMemory& mem,
                            Profiler::Profiler& profiler,vk::PhysicalDevice physical_device, vk::Device device,
                            uint32_t graphics_queue_index, vk::Queue render_graphics_queue)
    : context(std::make_unique<Pica::Context>()) {
    renderer = std::make_unique<Pica::Vulkan::Renderer>(mem, logger, profiler, physical_device, device, graphics_queue_index, render_graphics_queue);
    context->debug_server = &debug_server;
    context->settings = &settings;
    context->renderer = renderer.get();
    context->activity = &profiler.GetActivity("GPU");
    context->logger = logger;
}

PicaContext::~PicaContext() = default;

void PicaContext::InjectDependency(InterruptListener& os) {
    context->os = &os;
}

void PicaContext::InjectDependency(Memory::PhysicalMemory& memory) {
    context->mem = &memory;
}

Pica::Renderer* GetRenderer(Pica::Context& context) {
    return context.renderer;
}
