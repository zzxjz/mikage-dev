#pragma once

#include "ipc.hpp"

namespace Platform {

/**
 * Manages input peripherals and provides the interface for applications to
 * gather information about the current input state.
 */
namespace HID {

namespace User {

namespace IPC = Platform::IPC;

/**
 * Returns a shared memory block for input data. The five output events signal
 * new data being available in each of the five shared memory regions.
 */
using GetIPCHandles = IPC::IPCCommand<0xa>
                         ::response::add_handle<IPC::HandleType::SharedMemoryBlock>::add_handle<IPC::HandleType::Event>
                                   ::add_handle<IPC::HandleType::Event>::add_handle<IPC::HandleType::Event>
                                   ::add_handle<IPC::HandleType::Event>::add_handle<IPC::HandleType::Event>; // TODO: Mark events OneShot

using EnableGyroscope = IPC::IPCCommand<0x13>::response;

struct GyroscopeCalibrationData {
    uint32_t data[5];

    static auto IPCSerialize(const GyroscopeCalibrationData& data) {
        return std::make_tuple(data.data[0], data.data[1], data.data[2], data.data[3], data.data[4]);
    }

    static GyroscopeCalibrationData IPCDeserialize(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
        return { a, b, c, d, e };
    }

    using ipc_data_size = std::integral_constant<size_t, 5>;
};

// Maps the coefficient used to convert raw gyroscope values to degrees per second
using GetGyroscopeSensitivity = IPC::IPCCommand<0x15>::response::add_uint32;

using GetGyroscopeCalibrationData = IPC::IPCCommand<0x16>::response::add_serialized<GyroscopeCalibrationData>;

} // namespace User

} // namespace HID

} // namespace Platform
