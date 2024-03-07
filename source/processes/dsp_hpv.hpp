#pragma once

#include "os_hypervisor_private.hpp"

namespace HLE {

namespace OS {

namespace HPV {

struct DSPContext : SessionContext {
};

HPV::RefCounted<Object> CreateDspService(RefCounted<Port> port, DSPContext&);

} // namespace HPV

} // namespace HOS

} // namespace HLE
