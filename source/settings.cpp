#include "framework/settings.hpp"

namespace Settings {

// std::filesystem doesn't understand "~", so we replace the defaults with
// "/HOME~" for safety. The frontend must replace this substring with the true
// home folder path on startup.
// If a buggy frontend doesn't do this, accessing "/HOME~" will reliably fail
// instead of constructing a relative path.
std::string PathConfigDir::default_val = "/HOME~/.config/mikage";
std::string PathImmutableDataDir::default_val = "/usr/share/mikage";
std::string PathDataDir::default_val = "/HOME~/.local/share/mikage";
std::string PathCacheDir::default_val = "/HOME~/.cache/mikage";

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

template<>
bool BooleanOption<Settings::EnableAudioEmulation>::default_val = false;

} // namespace Config
