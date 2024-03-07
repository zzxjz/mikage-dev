#pragma once

#include "fake_process.hpp"

namespace HLE {

namespace OS {

class FakeI2C final {
    OS& os;
    spdlog::logger& logger;

    IPC::StaticBuffer static_buffers[2];

public:
    FakeI2C(FakeThread& thread);

    void I2CThread(FakeThread& thread, const char* service_name, uint32_t device_mask);

    void CommandHandler(FakeThread& thread, const IPC::CommandHeader& header, uint32_t device_mask);

    OS::ResultAnd<> SetRegisterBits8(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t reg_data, uint32_t enable_mask);
    OS::ResultAnd<> EnableRegisterBits8(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t enable_mask);
    OS::ResultAnd<> DisableRegisterBits8(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t disable_mask);
    OS::ResultAnd<> WriteRegister8(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t data);
    OS::ResultAnd<uint32_t> ReadRegister8(FakeThread& thread, uint32_t device_id, uint32_t reg_id);
    OS::ResultAnd<> WriteRegisterBuffer8_0xb(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t size, const IPC::StaticBuffer& data);
    OS::ResultAnd<IPC::StaticBuffer> ReadRegisterBuffer8(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t size);
    OS::ResultAnd<> WriteRegisterBuffer8_0xe(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t size, const IPC::StaticBuffer& data);
    OS::ResultAnd<> Unknown_0xf(FakeThread& thread, uint32_t param1, uint32_t param2, uint32_t param3);
    OS::ResultAnd<> WriteRegisterBuffer_0x11(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t size, IPC::MappedBuffer input_buffer);
    OS::ResultAnd<> ReadRegisterBuffer_0x12(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t size, IPC::MappedBuffer output_buffer);
};

}  // namespace OS

}  // namespace HLE
