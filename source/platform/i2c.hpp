#pragma once

#include "ipc.hpp"

namespace Platform {

/**
 * Interface to access I2C devices. Cf. the I2C registers for details.
 */
namespace I2C {

// All IPC commands are common between the services in this module, hence they
// are not put in a nested namespace

namespace IPC = Platform::IPC;

// TODO: The response headers for these are not verified at all, yet

using SetRegisterBits8 = IPC::IPCCommand<0x1>::add_uint32::add_uint32::add_uint32::add_uint32
                            ::response;
using EnableRegisterBits8 = IPC::IPCCommand<0x2>::add_uint32::add_uint32::add_uint32
                               ::response;
using DisableRegisterBits8 = IPC::IPCCommand<0x3>::add_uint32::add_uint32::add_uint32
                                ::response;
using WriteRegister8 = IPC::IPCCommand<0x5>::add_uint32::add_uint32::add_uint32
                          ::response;
using ReadRegister8 = IPC::IPCCommand<0x9>::add_uint32::add_uint32
                          ::response::add_uint32;
using WriteRegisterBuffer8_0xb = IPC::IPCCommand<0xb>::add_uint32::add_uint32::add_uint32::add_static_buffer
                                    ::response;
using ReadRegisterBuffer8 = IPC::IPCCommand<0xd>::add_uint32::add_uint32::add_uint32
                               ::response::add_static_buffer;
using WriteRegisterBuffer8_0xe = IPC::IPCCommand<0xe>::add_uint32::add_uint32::add_uint32::add_static_buffer
                                    ::response;
using Unknown_0xf = IPC::IPCCommand<0xf>::add_uint32::add_uint32::add_uint32
                       ::response;
using WriteRegisterBuffer_0x11 = IPC::IPCCommand<0x11>::add_uint32::add_uint32::add_uint32::add_buffer_mapping_read
                                    ::response;// TODO: unmap buffer
using ReadRegisterBuffer_0x12 = IPC::IPCCommand<0x12>::add_uint32::add_uint32::add_uint32::add_buffer_mapping_write
                                   ::response;// TODO: unmap buffer

} // namespace I2C

} // namespace Platform
