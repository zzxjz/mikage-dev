#pragma once

#include "os_hypervisor_private.hpp"

namespace HLE {

namespace OS {

namespace HPV {

struct AMContext : SessionContext {
};

HPV::RefCounted<Object> CreateAmService(RefCounted<Port> port, AMContext&);

} // namespace HPV

} // namespace HOS

} // namespace HLE
