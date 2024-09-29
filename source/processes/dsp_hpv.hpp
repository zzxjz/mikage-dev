#pragma once

#include "os_hypervisor_private.hpp"

struct AudioFrontend;

namespace Settings {
struct Settings;
}

namespace HLE {

namespace OS {

namespace HPV {

struct DSPContext : SessionContext {
    Settings::Settings* settings = nullptr;
    AudioFrontend* frontend = nullptr;
};

HPV::RefCounted<Object> CreateDspService(RefCounted<Port> port, DSPContext&);

} // namespace HPV

} // namespace HOS

} // namespace HLE
