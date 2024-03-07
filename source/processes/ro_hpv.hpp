#pragma once

#include "os_hypervisor_private.hpp"

namespace HLE {

namespace OS {

namespace HPV {

struct ROContext : SessionContext {
};

HPV::RefCounted<Object> CreateRoService(RefCounted<Port> port, ROContext&);

} // namespace HPV

} // namespace HOS

} // namespace HLE
