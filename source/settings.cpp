#include "framework/settings.hpp"

namespace Settings {

CPUEngine CPUEngineTag::default_val = CPUEngine::NARMive;

}

namespace Config {

template<>
bool BooleanOption<Settings::BootToHomeMenu>::default_val = false;

template<>
bool BooleanOption<Settings::UseNativeHID>::default_val = false;

template<>
bool BooleanOption<Settings::UseNativeFS>::default_val = false;

template<>
bool BooleanOption<Settings::DumpFrames>::default_val = false;

template<>
bool BooleanOption<Settings::ConnectToDebugger>::default_val = false;

template<>
unsigned IntegralOption<unsigned, Settings::AttachToProcessOnStartup>::default_val = 0;

template<>
unsigned IntegralOption<unsigned, Settings::AppMemType>::default_val = 0;

template<>
Settings::Renderer OptionDefault<Settings::Renderer>::default_val = Settings::Renderer::Vulkan;

template<>
Settings::ShaderEngine OptionDefault<Settings::ShaderEngine>::default_val = Settings::ShaderEngine::Bytecode;

} // namespace Config
