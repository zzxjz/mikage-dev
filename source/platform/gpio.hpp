#pragma once

#include "ipc.hpp"

namespace Platform {

/**
 * GPIO: Interface for system processes to hardware interrupts.
 */
namespace GPIO {

// All IPC commands are common between the services in this module, hence they
// are not put in a nested namespace

namespace IPC = Platform::IPC;

// TODO: Verify the input handle type for Unb/BindInterrupt
using BindInterrupt = IPC::IPCCommand<0x9>::add_uint32::add_uint32::add_handle<IPC::HandleType::Event>
                         ::response;
using UnbindInterrupt = IPC::IPCCommand<0xa>::add_uint32::add_handle<IPC::HandleType::Event>
                         ::response;

} // namespace GPIO

} // namespace Platform
