#pragma once

#include "framework/config_framework.hpp"

#include <boost/variant.hpp>

#include <string>
#include <variant>

namespace Settings {

enum class CPUEngine {
    NARMive,
};

struct CPUEngineTag : Config::Option {
    static constexpr const char* name = "CPUEngine";
    using type = CPUEngine;
    static type default_value() { return default_val; }

    static CPUEngine default_val;
};

struct InitialApplicationTag : Config::Option {

    struct HostFile {
        std::string filename;
    };

    struct FileDescriptor {
        int fd;
    };

    static constexpr const char* name = "InitialApplication";
    using type = std::variant<std::monostate, HostFile, FileDescriptor>;
    static type default_value() { return std::monostate {}; }
};

// Launches the Home Menu (or other menu with the given title ID) from emulated NAND upon OS startup
struct BootToHomeMenu : Config::BooleanOption<BootToHomeMenu> {
    static constexpr const char* name = "BootToHomeMenu";
};

// Use the native HID module upon OS startup
struct UseNativeHID : Config::BooleanOption<UseNativeHID> {
    static constexpr const char* name = "UseNativeHID";
};

// Use the native FS module upon OS startup
struct UseNativeFS : Config::BooleanOption<UseNativeFS> {
    static constexpr const char* name = "UseNativeFS";
};

// Dump displayed frames to a series of binary files
struct DumpFrames : Config::BooleanOption<DumpFrames> {
    static constexpr const char* name = "DumpFrames";
};

// Connect to a debugger via the GDB remote protocol on startup
struct ConnectToDebugger : Config::BooleanOption<ConnectToDebugger> {
    static constexpr const char* name = "ConnectToDebugger";
};

// Connect to a debugger via the GDB remote protocol on startup
struct AttachToProcessOnStartup : Config::IntegralOption<unsigned, AttachToProcessOnStartup> {
    static constexpr const char* name = "ProcessToAttachTo";
};

struct AppMemType : Config::IntegralOption<unsigned, AppMemType> {
    static constexpr const char* name = "AppMemType";
};

enum class Renderer {
    Software,
    Vulkan,
};

struct RendererTag : Config::OptionDefault<Renderer> {
    static constexpr const char* name = "Renderer";
};

enum class ShaderEngine {
    Interpreter,
    Bytecode,
    GLSL,
};

struct ShaderEngineTag : Config::OptionDefault<ShaderEngine> {
    static constexpr const char* name = "ShaderEngine";
};


struct Settings : Config::Options<CPUEngineTag,
                                  InitialApplicationTag,
                                  BootToHomeMenu,
                                  UseNativeHID,
                                  UseNativeFS,
                                  DumpFrames,
                                  ConnectToDebugger,
                                  AttachToProcessOnStartup,
                                  AppMemType,
                                  RendererTag,
                                  ShaderEngineTag> { };

} // namespace Settings
