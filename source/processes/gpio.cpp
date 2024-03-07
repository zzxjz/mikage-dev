#include "gpio.hpp"
#include "os.hpp"

#include "platform/gpio.hpp"

namespace HLE {

namespace OS {

FakeGPIO::FakeGPIO(FakeThread& thread)
    : os(thread.GetOS()),
      logger(*thread.GetLogger()) {

    GPIOThread(thread, "gpio:HID", 0x4301);
}

void FakeGPIO::GPIOThread(FakeThread& thread, const char* service_name, uint32_t interrupt_mask) {
    ServiceUtil service(thread, service_name, 1);

    Handle last_signalled = HANDLE_INVALID;

    for (;;) {
        OS::Result result;
        int32_t index;
        std::tie(result,index) = service.ReplyAndReceive(thread, last_signalled);
        last_signalled = HANDLE_INVALID;
        if (result != RESULT_OK)
            os.SVCBreak(thread, OS::BreakReason::Panic);

        if (index == 0) {
            // ServerPort: Incoming client connection

            int32_t session_index;
            std::tie(result,session_index) = service.AcceptSession(thread, index);
            if (result != RESULT_OK) {
                auto session = service.GetObject<ServerSession>(session_index);
                if (!session) {
                    logger.error("{}Failed to accept session.", ThreadPrinter{thread});
                    os.SVCBreak(thread, OS::BreakReason::Panic);
                }

                auto session_handle = service.GetHandle(session_index);
                logger.warn("{}Failed to accept session. Maximal number of sessions exhausted? Closing session handle {}",
                            ThreadPrinter{thread}, HandlePrinter{thread,session_handle});
                os.SVCCloseHandle(thread, session_handle);
            }
        } else {
            // server_session: Incoming IPC command from the indexed client
            logger.info("{}received IPC request", ThreadPrinter{thread});
            Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
            auto signalled_handle = service.GetHandle(index);
            CommandHandler(thread, header, interrupt_mask);
            last_signalled = signalled_handle;
        }
    }
}


template<typename Class, typename Func>
static auto BindMemFn(Func f, Class* c) {
    return [f,c](auto&&... args) { return std::mem_fn(f)(c, args...); };
}

void FakeGPIO::CommandHandler(FakeThread& thread, const IPC::CommandHeader& header, uint32_t interrupt_mask) try {
    using namespace Platform::GPIO;

    switch (header.command_id) {
    case 0x2: // Unknown
    {
        logger.info("{}received Unknown0x2 with arg1={:#x} and arg2={:#x}",
                          ThreadPrinter{thread}, thread.ReadTLS(0x84), thread.ReadTLS(0x88));

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x4: // Unknown
    {
        logger.info("{}received Unknown0x4 with arg1={:#x} and arg2={:#x}",
                          ThreadPrinter{thread}, thread.ReadTLS(0x84), thread.ReadTLS(0x88));

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x6: // Unknown
    {
        logger.info("{}received Unknown0x6 with arg1={:#x} and arg2={:#x}",
                          ThreadPrinter{thread}, thread.ReadTLS(0x84), thread.ReadTLS(0x88));

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;
    }

    case 0x7: // Unknown
    {
        logger.info("{}received Unknown0x7 with arg={:#x}",
                          ThreadPrinter{thread}, thread.ReadTLS(0x84));

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0); // Unknown return value
        break;
    }

    case BindInterrupt::id:
        return IPC::HandleIPCCommand<BindInterrupt>(BindMemFn(&FakeGPIO::HandleBindInterrupt, this), thread, thread, interrupt_mask);

    default:
        throw IPC::IPCError{header.raw, 0xdeadbeef};
    }
} catch (const IPC::IPCError& err) {
    throw std::runtime_error(fmt::format("Unknown GPIO service command with header {:#010x}", err.header));
}

OS::ResultAnd<> FakeGPIO::HandleBindInterrupt(FakeThread& thread, uint32_t service_interrupt_mask,
                                              uint32_t interrupt_mask, uint32_t priority, Handle event_handle) {
    logger.info("{}received BindInterrupt with mask={:#x}, priority={:#x}",
                        ThreadPrinter{thread}, interrupt_mask, priority);

    if ((service_interrupt_mask & interrupt_mask) != interrupt_mask) {
        logger.error("{}provided interrupt mask {:#010x} that is not compatible with this service (mask {:#010x})!",
                     ThreadPrinter{thread}, interrupt_mask, service_interrupt_mask);
        os.SVCBreak(thread, OS::BreakReason::Panic);
    }

    auto event = thread.GetProcessHandleTable().FindObject<Event>(event_handle);

    // Bind interrupts according to the given bitmask
    // Behavior of bits outside the range is unknown
    for (uint32_t bit = 1; bit < 18; ++bit) {
        if (0 == (interrupt_mask & (1u << bit)))
            continue;

        uint32_t interrupt_index;
        switch (bit) {
            case 1:
                interrupt_index = 0x63;
                break;

            case 2:
                interrupt_index = 0x60;
                break;

            case 3:
                interrupt_index = 0x64;
                break;

            case 4:
                interrupt_index = 0x66;
                break;

            // Behavior of this bit is unknown
            case 5:
                os.SVCBreak(thread, OS::BreakReason::Panic);

            default:
                interrupt_index = 0x62 + bit;
                break;
        }
        os.SVCBindInterrupt(thread, interrupt_index, event, priority, true);
    }

    return std::make_tuple(RESULT_OK);
}

}  // namespace OS

}  // namespace HLE
