#pragma once

#include "ipc.hpp"

namespace Platform {

/**
 * CDC = Communication Device Class.
 */
namespace CDC {

namespace HID {

// NOTE: Response headers for these aren't verified!
using GetTouchData = IPC::IPCCommand<0x1>
                        ::response::add_uint32::add_uint32;
using Initialize = IPC::IPCCommand<0x2>
                      ::response;

}

} // namespace CDC

} // namespace Platform
