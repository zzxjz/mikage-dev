#pragma once

#include "primitive_assembly.h"
#include "shader.hpp"

#include <platform/gpu/pica.hpp>

#include <array>
#include <memory>

namespace spdlog {
class logger;
}

struct InterruptListener;

namespace Memory {
struct PhysicalMemory;
}

namespace Profiler {
class Activity;
}

namespace Settings {
struct Settings;
}

namespace Debugger {
class DebugServer;
}

namespace Pica {

class Renderer;

struct Context {
    Regs registers;

    // Vertex attribute defaults as configured by the application
    std::array<std::array<float24, 4>, 12> vertex_attribute_defaults;

    PrimitiveAssembler<VertexShader::OutputVertex> primitive_assembler;
    PrimitiveAssembler<VertexShader::InputVertex> primitive_assembler_new;

    VertexShader::EnginePool shader_engines;

    VertexShader::ShaderUniforms shader_uniforms;

    std::array<u32, 1024> shader_memory;
    std::array<u32, 1024> swizzle_data;

    // Data for each of the 24 light LUTs
    std::array<std::array<uint32_t, 256>, 24> light_lut_data;

    Debugger::DebugServer* debug_server = nullptr;

    Settings::Settings* settings = nullptr;

    InterruptListener* os = nullptr;
    Memory::PhysicalMemory* mem = nullptr;

    Renderer* renderer = nullptr;
    Profiler::Activity* activity = nullptr;

    std::shared_ptr<spdlog::logger> logger;
};

} // namespace Pica
