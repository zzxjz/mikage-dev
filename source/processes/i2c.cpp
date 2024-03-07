#include "i2c.hpp"
#include "os.hpp"

#include "platform/i2c.hpp"

namespace HLE {

namespace OS {

FakeI2C::FakeI2C(FakeThread& thread)
    : os(thread.GetOS()),
      logger(*thread.GetLogger()) {

    thread.name = "i2c::HIDThread";

    auto lcd_thread = std::make_shared<WrappedFakeThread>(thread.GetParentProcess(),
                                                            [this](FakeThread& thread) { return I2CThread(thread, "i2c::LCD", 0b1100000); });
    lcd_thread->name = "i2c::LCDThread";
    thread.GetParentProcess().AttachThread(lcd_thread);

    {
        auto new_thread = std::make_shared<WrappedFakeThread>(thread.GetParentProcess(),
                                                                [this](FakeThread& thread) { return I2CThread(thread, "i2c::MCU", 0b1001); });
        new_thread->name = "i2c::MCUThread";
        thread.GetParentProcess().AttachThread(new_thread);
    }

    {
        auto new_thread = std::make_shared<WrappedFakeThread>(thread.GetParentProcess(),
                                                                [this](FakeThread& thread) { return I2CThread(thread, "i2c::CAM", 0b10110); });
        new_thread->name = "i2c::CAMThread";
        thread.GetParentProcess().AttachThread(new_thread);
    }

    {
        // TODO: On N3DS, this also has access to device 17
        auto new_thread = std::make_shared<WrappedFakeThread>(thread.GetParentProcess(),
                                                                [this](FakeThread& thread) { return I2CThread(thread, "i2c::IR", 0b10000000000000); });
        new_thread->name = "i2c::IRThread";
        thread.GetParentProcess().AttachThread(new_thread);
    }

    I2CThread(thread, "i2c::HID", 0b1111000000000);
}

void FakeI2C::I2CThread(FakeThread& thread, const char* service_name, uint32_t device_mask) {
    // Set up static buffers
    static_buffers[0].addr = thread.GetParentProcess().AllocateStaticBuffer(0x80);
    static_buffers[0].size = 0x80;
    static_buffers[0].id = 0;
    static_buffers[1].addr = thread.GetParentProcess().AllocateStaticBuffer(0x80);
    static_buffers[1].size = 0x80;
    static_buffers[1].id = 1;

    for (auto i : {0, 1}) {
        thread.WriteTLS(0x180 + 8 * i, IPC::TranslationDescriptor::MakeStaticBuffer(i, static_buffers[i].size).raw);
        thread.WriteTLS(0x184 + 8 * i, static_buffers[i].addr);
    }
    for (unsigned i = 2; i < 16; ++i) {
        thread.WriteTLS(0x180 + 8 * i, 0);
        thread.WriteTLS(0x184 + 8 * i, 0);
    }

    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, service_name, 5));

    Handle last_signalled = HANDLE_INVALID;

    for (;;) {
        OS::Result result;
        int32_t index;
        std::tie(result,index) = thread.CallSVC(&OS::SVCReplyAndReceive, service.handles.data(), service.handles.size(), last_signalled);
        last_signalled = HANDLE_INVALID;
        if (result == 0xc920181a) {
            if (index == -1) {
                // Reply target was closed... TODO: Implement
                os.SVCBreak(thread, OS::BreakReason::Panic);
            } else {
                // We woke up because a handle was closed; ServiceUtil has taken
                // care of closing the signalled handle already, so just continue
                service.Erase(index);
            }
        } else if (result != RESULT_OK) {
            os.SVCBreak(thread, OS::BreakReason::Panic);
        } else {
            if (index == 0) {
                // ServerPort: Incoming client connection

                HandleTable::Entry<ServerSession> session;
                std::tie(result,session) = thread.CallSVC(&OS::SVCAcceptSession, *service.GetObject<ServerPort>(index));
                if (result != RESULT_OK) {
                    os.SVCBreak(thread, OS::BreakReason::Panic);
                }

                service.Append(session);
            } else {
                // server_session: Incoming IPC command from the indexed client
                logger.info("{}received IPC request", ThreadPrinter{thread});
                Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
                auto signalled_handle = service.handles[index];
                CommandHandler(thread, header, device_mask);
                last_signalled = signalled_handle;
            }
        }
    }
}

template<typename Class, typename Func>
static auto BindMemFn(Func f, Class* c) {
    return [f,c](auto&&... args) { return std::mem_fn(f)(c, args...); };
}

void FakeI2C::CommandHandler(FakeThread& thread, const IPC::CommandHeader& header, uint32_t device_mask) try {
    namespace I2C = Platform::I2C;

    // Each I2C service is limited to a subset of devices, hence we check here
    // that the given device ID (which seems to be specified at the same TLS
    // offset for all IPC commands) and make sure it fits to the device mask
    // for this service
    auto device_id = thread.ReadTLS(0x84);
    if (((1 << device_id) & device_mask) == 0) {
//        logger.error("{}Device ID {:#x} is not accessible by this service (device mask {:#x})",
//                     ThreadPrinter{thread}, device_id, device_mask);
//        os.SVCBreak(thread, OS::BreakReason::Panic);
    }

    switch (header.command_id) {
    case I2C::SetRegisterBits8::id:
        return IPC::HandleIPCCommand<I2C::SetRegisterBits8>(BindMemFn(&FakeI2C::SetRegisterBits8, this), thread, thread);

    case I2C::EnableRegisterBits8::id:
        return IPC::HandleIPCCommand<I2C::EnableRegisterBits8>(BindMemFn(&FakeI2C::EnableRegisterBits8, this), thread, thread);

    case I2C::DisableRegisterBits8::id:
        return IPC::HandleIPCCommand<I2C::DisableRegisterBits8>(BindMemFn(&FakeI2C::DisableRegisterBits8, this), thread, thread);

    case I2C::ReadRegister8::id:
        return IPC::HandleIPCCommand<I2C::ReadRegister8>(BindMemFn(&FakeI2C::ReadRegister8, this), thread, thread);

    case I2C::WriteRegister8::id:
        return IPC::HandleIPCCommand<I2C::WriteRegister8>(BindMemFn(&FakeI2C::WriteRegister8, this), thread, thread);

    case 0x6:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0x15:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0); // TODO
        break;

    case I2C::WriteRegisterBuffer8_0xb::id:
        return IPC::HandleIPCCommand<I2C::WriteRegisterBuffer8_0xb>(BindMemFn(&FakeI2C::WriteRegisterBuffer8_0xb, this), thread, thread);

    case I2C::ReadRegisterBuffer8::id:
        return IPC::HandleIPCCommand<I2C::ReadRegisterBuffer8>(BindMemFn(&FakeI2C::ReadRegisterBuffer8, this), thread, thread);

    case I2C::WriteRegisterBuffer8_0xe::id:
        return IPC::HandleIPCCommand<I2C::WriteRegisterBuffer8_0xe>(BindMemFn(&FakeI2C::WriteRegisterBuffer8_0xe, this), thread, thread);

    case I2C::Unknown_0xf::id:
        return IPC::HandleIPCCommand<I2C::Unknown_0xf>(BindMemFn(&FakeI2C::Unknown_0xf, this), thread, thread);

    case I2C::WriteRegisterBuffer_0x11::id:
        return IPC::HandleIPCCommand<I2C::WriteRegisterBuffer_0x11>(BindMemFn(&FakeI2C::WriteRegisterBuffer_0x11, this), thread, thread);

    case I2C::ReadRegisterBuffer_0x12::id:
        return IPC::HandleIPCCommand<I2C::ReadRegisterBuffer_0x12>(BindMemFn(&FakeI2C::ReadRegisterBuffer_0x12, this), thread, thread);

    default:
        throw IPC::IPCError{header.raw, 0xdeadbeef};
    }
} catch (const IPC::IPCError& err) {
    throw std::runtime_error(fmt::format("Unknown I2C service command with header {:#010x}", err.header));
}

OS::ResultAnd<> FakeI2C::SetRegisterBits8(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t reg_data, uint32_t enable_mask) {
    logger.info("{}received EnableRegisterBits8 (device_id={:#x}, regid={:#x}, regdata={:#x}, enablemask={:#x})",
                ThreadPrinter{thread}, device_id, reg_id, reg_data, enable_mask);
    return std::make_tuple(RESULT_OK);
}

OS::ResultAnd<> FakeI2C::EnableRegisterBits8(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t enable_mask) {
    logger.info("{}received EnableRegisterBits8 (device_id={:#x}, regid={:#x}, enablemask={:#x})",
                ThreadPrinter{thread}, device_id, reg_id, enable_mask);
    return std::make_tuple(RESULT_OK);
}

OS::ResultAnd<> FakeI2C::DisableRegisterBits8(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t disable_mask) {
    logger.info("{}received DisableRegisterBits8 (device_id={:#x}, regid={:#x}, disablemask={:#x})",
                ThreadPrinter{thread}, device_id, reg_id, disable_mask);
    return std::make_tuple(RESULT_OK);
}

OS::ResultAnd<> FakeI2C::WriteRegister8(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t data) {
    logger.info("{}received WriteRegister8 (device_id={:#x}, regid={:#x}, data={:#x})",
                ThreadPrinter{thread}, device_id, reg_id, data);
    return std::make_tuple(RESULT_OK);
}

OS::ResultAnd<uint32_t> FakeI2C::ReadRegister8(FakeThread& thread, uint32_t device_id, uint32_t reg_id) {
    logger.info("{}received ReadRegister8 (device_id={:#x}, regid={:#x})",
                ThreadPrinter{thread}, device_id, reg_id);
    return std::make_tuple(RESULT_OK, uint32_t{0});
}

OS::ResultAnd<> FakeI2C::WriteRegisterBuffer8_0xb(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t size, const IPC::StaticBuffer& data) {
    logger.info("{}received WriteRegisterBuffer8_0xb (device_id={:#x}, regid={:#x}, size={:#x})",
                ThreadPrinter{thread}, device_id, reg_id, size);
    return std::make_tuple(RESULT_OK);
}

OS::ResultAnd<IPC::StaticBuffer> FakeI2C::ReadRegisterBuffer8(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t size) {
    logger.info("{}received ReadRegisterBuffer8 (device_id={:#x}, regid={:#x}, size={:#x})",
                ThreadPrinter{thread}, device_id, reg_id, size);

    if (size > static_buffers[0].size) {
        logger.error("{}requested size {:#x} that is larger than the static buffer size {:#x}",
                     ThreadPrinter{thread}, size, static_buffers[0].size);
        os.SVCBreak(thread, OS::BreakReason::Panic);
    }

    // Return a dummy buffer for now
    for (uint32_t offset = 0; offset < static_buffers[0].size; ++offset) {
        thread.WriteMemory(static_buffers[0].addr + offset, 0);
    }

    return std::make_tuple(RESULT_OK, IPC::StaticBuffer{static_buffers[0].addr, size, static_buffers[0].id});
}

OS::ResultAnd<> FakeI2C::WriteRegisterBuffer8_0xe(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t size, const IPC::StaticBuffer& data) {
    logger.info("{}received WriteRegisterBuffer8_0xe (device_id={:#x}, regid={:#x}, size={:#x})",
                ThreadPrinter{thread}, device_id, reg_id, size);
    return std::make_tuple(RESULT_OK);
}

OS::ResultAnd<> FakeI2C::Unknown_0xf(FakeThread& thread, uint32_t param1, uint32_t param2, uint32_t param3) {
    logger.info("{}received Unknown_0xf (param1={:#x}, param2={:#x}, param3={:#x})",
                ThreadPrinter{thread}, param1, param2, param3);
    return std::make_tuple(RESULT_OK);
}

OS::ResultAnd<> FakeI2C::WriteRegisterBuffer_0x11(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t size, IPC::MappedBuffer input_buffer) {
    logger.info("{}received ReadRegisterBuffer_0x11 (device_id={:#x}, regid={:#x}, size={:#x}): Stub",
                ThreadPrinter{thread}, device_id, reg_id, size);

    if (size > input_buffer.size) {
        logger.error("{}requested size {:#x} that is larger than the mapped buffer size {:#x}",
                     ThreadPrinter{thread}, size, input_buffer.size);
        os.SVCBreak(thread, OS::BreakReason::Panic);
    }

    // TODO: Implement.

    return std::make_tuple(RESULT_OK);
}

OS::ResultAnd<> FakeI2C::ReadRegisterBuffer_0x12(FakeThread& thread, uint32_t device_id, uint32_t reg_id, uint32_t size, IPC::MappedBuffer output_buffer) {
    logger.info("{}received ReadRegisterBuffer_0x12 (device_id={:#x}, regid={:#x}, size={:#x})",
                ThreadPrinter{thread}, device_id, reg_id, size);

    if (size > output_buffer.size) {
        logger.error("{}requested size {:#x} that is larger than the mapped buffer size {:#x}",
                     ThreadPrinter{thread}, size, output_buffer.size);
        os.SVCBreak(thread, OS::BreakReason::Panic);
    }

    // Return a dummy buffer for now
    for (uint32_t offset = 0; offset < output_buffer.size; ++offset) {
        thread.WriteMemory(output_buffer.addr + offset, 0);
    }

    return std::make_tuple(RESULT_OK);
}

}  // namespace OS

}  // namespace HLE
