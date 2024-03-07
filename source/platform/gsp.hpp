#pragma once

#include "ipc.hpp"

#include <boost/hana/define_struct.hpp>

namespace Platform {

/**
 * GPU and LCD interface for applications
 */
namespace GSP {

namespace GPU {

namespace IPC = Platform::IPC;

/**
 * Framebuffer configuration used for SetBufferSwap and for the shared memory
 * framebuffer info block.
 */
struct FramebufferDescriptor {
    BOOST_HANA_DEFINE_STRUCT(FramebufferDescriptor,
        (uint32_t, index_of_configured_fb), // Select whether the following fields apply to the primary (0) or to the secondary (1) framebuffer
        (uint32_t, source_address),         // Virtual address to source data for left eye
        (uint32_t, source_address_right),   // Virtual address to source data for right eye (only used for the 3D screen)
        (uint32_t, stride),                 // Distance between rows in bytes
        (uint32_t, config),                 // Configuration: Pixel format, enable/disable 3D, etc
        (uint32_t, index_of_displayed_fb),  // Select whether to display the primary (0) or the secondary (1) framebuffer on screen
        (uint32_t, unknown)
    );

    static auto IPCSerialize(const FramebufferDescriptor& desc) {
        return std::make_tuple(desc.index_of_configured_fb, desc.source_address, desc.source_address_right, desc.stride, desc.config, desc.index_of_displayed_fb, desc.unknown);
    }

    static FramebufferDescriptor IPCDeserialize(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g) {
        return { a, b, c, d, e, f, g };
    }

    using ipc_data_size = std::integral_constant<size_t, 7>;
};

struct DisplayCaptureInfo {
    BOOST_HANA_DEFINE_STRUCT(DisplayCaptureInfo,
        (uint32_t, source_address),         // Virtual address to source data for left eye
        (uint32_t, source_address_right),   // Virtual address to source data for right eye (only used for the 3D screen)
        (uint32_t, config),                 // Configuration: Pixel format, enable/disable 3D, etc
        (uint32_t, stride)                 // Distance between rows in bytes
    );

    static auto IPCSerialize(const DisplayCaptureInfo& desc) {
        return std::make_tuple(desc.source_address, desc.source_address_right, desc.config, desc.stride);
    }

    static DisplayCaptureInfo IPCDeserialize(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        return { a, b, c, d };
    }

    using ipc_data_size = std::integral_constant<size_t, 4>;
};

/**
 * Inputs:
 * - Base register offset (added to 0x1eb00000)
 * - Number of bytes to write (must be a multiple of 4)
 * - Buffer of register values
 */
using WriteHWRegs = IPC::IPCCommand<0x1>::add_uint32::add_uint32::add_static_buffer::response;

/**
 * Inputs:
 * - Base register offset (added to 0x1eb00000)
 * - Number of bytes to write (must be a multiple of 4)
 * - Buffer of register values
 * - Buffer of register write masks
 */
using WriteHWRegsWithMask = IPC::IPCCommand<0x2>::add_uint32::add_uint32::add_static_buffer::add_static_buffer::response;

/**
 * Inputs:
 * - Base register offset (added to 0x1eb00000)
 * - Number of bytes to read (must be a multiple of 4)
 */
using ReadHWRegs = IPC::IPCCommand<0x4>::add_uint32::add_uint32::response::add_static_buffer;

/**
 * Inputs:
 * - Screen id (0: Top screen, 1: Bottom screen)
 * - Framebuffer descriptor to apply
 */
using SetBufferSwap = IPC::IPCCommand<0x5>::add_uint32::add_serialized<FramebufferDescriptor>::response;

/**
 * Inputs:
 * - Range start adress (must be part of linear virtual memory)
 * - Range size (in bytes)
 * - Process
 *
 * Error codes:
 * - 0xe0e02bf5: Given address range is not in virtual memory (TODOTEST)
 */
using FlushDataCache = IPC::IPCCommand<0x8>::add_uint32::add_uint32::add_handle<IPC::HandleType::Process>::response;

using RegisterInterruptRelayQueue = IPC::IPCCommand<0x13>::add_uint32::add_handle<IPC::HandleType::Event>
                                       ::response::add_uint32::add_uint32::add_handle<IPC::HandleType::SharedMemoryBlock>;

using UnregisterInterruptRelayQueue = IPC::IPCCommand<0x14>::response;

/**
 * Enables the given process to trigger shared memory command processing.
 * TODO: Are other operations guarded by this, too?
 *
 * Inputs:
 * - Flags (purpose unknown; usually 0)
 * - Process handle of the process to acquire rights for
 */
using AcquireRight = IPC::IPCCommand<0x16>::add_uint32::add_handle<IPC::HandleType::Process>::response;

using ReleaseRight = IPC::IPCCommand<0x17>::response;

using ImportDisplayCaptureInfo = IPC::IPCCommand<0x18>::response::add_serialized<DisplayCaptureInfo>::add_serialized<DisplayCaptureInfo>;

} // namespace GPU


} // namespace GSP

} // namespace Platform
