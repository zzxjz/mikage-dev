#pragma once

#include "os_hypervisor_private.hpp"

namespace HLE {

namespace OS {

namespace HPV {

struct NSContext : SessionContext {
};

HPV::RefCounted<Object> CreateAPTService(RefCounted<Port> port, NSContext& context);
HPV::RefCounted<Object> CreateNSService(RefCounted<Port> port, NSContext& context);

} // namespace HPV

} // namespace HOS

} // namespace HLE
