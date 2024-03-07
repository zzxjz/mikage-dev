#pragma once

#include "../ipc.hpp"

namespace Platform {

namespace CTR {

namespace DSP {

/**
 * Inputs:
 * - Semaphore value (TODOTEST: upper 16 bit ignored?)
 */
using SetSemaphore = HLE::IPC::IPCCommand<0x7>::add_uint32
                             ::response;

using WriteProcessPipe = HLE::IPC::IPCCommand<0xd>::add_uint32::add_uint32::add_static_buffer
                                 ::response;

using ReadPipeIfPossible = HLE::IPC::IPCCommand<0x10>::add_uint32::add_uint32::add_uint32
                                   ::response::add_uint32::add_static_buffer;

using LoadComponent = HLE::IPC::IPCCommand<0x11>::add_uint32::add_uint32::add_uint32::add_buffer_mapping_read
                              ::response::add_uint32::add_buffer_mapping_read;

/**
 * Inputs:
 * - Virtual base address
 * - Number of bytes
 * - Process owning the flushed memory
 */
using FlushDataCache = HLE::IPC::IPCCommand<0x13>::add_uint32::add_uint32::add_handle<IPC::HandleType::Process>
                               ::response;

/**
 * Inputs:
 * - Virtual base address
 * - Number of bytes
 * - Process owning the invalidated memory
 */
using InvalidateDataCache = HLE::IPC::IPCCommand<0x14>::add_uint32::add_uint32::add_handle<IPC::HandleType::Process>
                               ::response;

using RegisterInterruptEvents = HLE::IPC::IPCCommand<0x15>::add_uint32::add_uint32::add_handle<HLE::IPC::HandleType::Event>
                                        ::response;

/**
 * Outputs:
 * - Event for the application to signal whenever it has written data for an audio frame
 */
using GetSemaphoreEventHandle = HLE::IPC::IPCCommand<0x16>
                                        ::response::add_handle<HLE::IPC::HandleType::Event>;

using SetSemaphoreMask = HLE::IPC::IPCCommand<0x17>::add_uint32
                                 ::response;

} // namespace DSP

} // namespace CTR

} // namespace Platform
