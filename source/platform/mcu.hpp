#pragma once

#include "ipc.hpp"

namespace Platform {

/**
 * Low-level interaction with hardware devices.
 */
namespace MCU {

namespace GPU {

namespace IPC = Platform::IPC;

// NOTE: All command signatures below have been verified unless labelled otherwise

using GetLcdPowerState = IPC::IPCCommand<0x1>
                            ::response::add_uint32::add_uint32;
using SetLcdPowerState = IPC::IPCCommand<0x2>::add_uint32::add_uint32
                            ::response;
using GetGpuLcdInterfaceState = IPC::IPCCommand<0x3>
                                   ::response::add_uint32;
using SetGpuLcdInterfaceState = IPC::IPCCommand<0x4>::add_uint32
                                   ::response;
using GetMcuFwVerHigh = IPC::IPCCommand<0x9>
                           ::response::add_uint32;
using GetMcuFwVerLow = IPC::IPCCommand<0xa>
                          ::response::add_uint32;
using Set3dLedState = IPC::IPCCommand<0xb>::add_uint32
                         ::response;
using GetMcuGpuEvent = IPC::IPCCommand<0xd>
                          ::response::add_handle<IPC::HandleType::Event>;
using GetMcuGpuEventReason = IPC::IPCCommand<0xe>
                                ::response::add_uint32; // Reply not verified

} // namespace GPU


namespace HID {

// NOTE: All command signatures below have been verified

using Unknown0x1 = IPC::IPCCommand<0x1>::add_uint32
                      ::response;
using GetMcuHidEventHandle = IPC::IPCCommand<0xc>
                                ::response::add_handle<IPC::HandleType::Event>;
using Get3dSliderState = IPC::IPCCommand<0x7>
                            ::response::add_uint32;
using SetAccelerometerState = IPC::IPCCommand<0xf>::add_uint32
                                 ::response;

} // namespace HID

} // namespace MCU

} // namespace Platform
