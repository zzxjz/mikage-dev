#pragma once

#include "ipc.hpp"

namespace Platform::CSND {

using Initialize = IPC::IPCCommand<0x1>::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32
                         ::response::add_handles<IPC::HandleType::Object, 2>;

using FlushDataCache = IPC::IPCCommand<0x9>::add_uint32::add_uint32::add_handle<IPC::HandleType::Process>
                          ::response;

} // namespace Platform::CSND
