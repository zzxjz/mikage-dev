#pragma once

#include "os_hypervisor_private.hpp"

namespace HLE {

namespace OS {

namespace HPV {

struct SMContext : SessionContext {
};

HPV::RefCounted<Object> CreateSMSrvService(RefCounted<Port> port, SMContext& context);
HPV::RefCounted<Object> CreateSMSrvPmService(RefCounted<Port> port, SMContext& context);

} // namespace HPV

} // namespace HOS

} // namespace HLE
