#pragma once

#include "os_hypervisor_private.hpp"

namespace HLE {

namespace OS {

namespace HPV {

struct CFGContext : SessionContext {
};

HPV::RefCounted<Object> CreateCfgService(RefCounted<Port> port, CFGContext&);

} // namespace HPV

} // namespace HOS

} // namespace HLE
