#include "hid.hpp"
#include "platform/hid.hpp"
#include "platform/sm.hpp" // Needed for triggering home menu

#include <framework/exceptions.hpp>

#include <range/v3/view/take.hpp>

namespace HLE {

namespace OS {

namespace {

struct BinaryState {
    uint32_t raw;
};

struct BinaryStateAndCirclePad {
    BOOST_HANA_DEFINE_STRUCT(BinaryStateAndCirclePad,
        (BinaryState, current_binary),     // E.g. button presses
        (BinaryState, activated_binary),   // E.g. button pushes (previously unpressed)
        (BinaryState, deactivated_binary), // E.g. button releases (previously pressed)
        (uint32_t, circle_pad)
    );
};

struct TouchState {
    BOOST_HANA_DEFINE_STRUCT(TouchState,
        (uint16_t, x),      // Pixel coordinate (0 = left edge, 320 = right edge, subject to proper hardware calibration)
        (uint16_t, y),      // Pixel coordinate (0 = top edge, 240 = bottom edge, subject to proper hardware calibration)
        (uint32_t, flags)   // Non-zero if touch is pressed (TODOTEST: Which value is used specifically?)
    );
};

template<typename T>
struct SharedMemorySection;

template<>
struct SharedMemorySection<BinaryStateAndCirclePad> {
    BOOST_HANA_DEFINE_STRUCT(SharedMemorySection,
        (uint64_t, timestamp),           // Value obtained from SVCGetSystemTick
        (uint64_t, previous_timestamp),  // Timestamp of previous update
        (uint32_t, active_entry),        // Index to the last element in entries updated by HID
        (uint32_t, unknown1),
        (uint32_t, slider3d_percentage), // Float value from 0.0 - 100.0
        (BinaryState, current_data),
        (uint32_t, circle_pad),
        (uint32_t, unknown3),
        (std::array<BinaryStateAndCirclePad, 8>, entries)
    );
};

template<>
struct SharedMemorySection<TouchState> {
    BOOST_HANA_DEFINE_STRUCT(SharedMemorySection,
        (uint64_t, timestamp),          // Value obtained from SVCGetSystemTick
        (uint64_t, previous_timestamp), // Timestamp of previous update
        (uint32_t, active_entry),       // Index to the last element in entries updated by HID
        (uint32_t, unknown1),
        (uint64_t, unknown2),           // Raw touch state
        (std::array<TouchState, 8>, entries)
    );
};

struct SharedMemory {
    BOOST_HANA_DEFINE_STRUCT(SharedMemory,
        (SharedMemorySection<BinaryStateAndCirclePad>, binary_state_and_circle_pad),
        (SharedMemorySection<TouchState>, touch_state)
    );
};

} // anonymous namespace

struct FakeHID : TagMapHIDMMIO {
public:
    FakeHID(FakeThread& thread);
    virtual ~FakeHID() = default;

    spdlog::logger& logger;

    VAddr shared_mem_vaddr;
    static constexpr const uint32_t shared_mem_size = 0x1000;

    Handle shared_memory;

    // NOTE: The last of these events refers to the debug pad, which we
    //       don't currently emulate. Hence, it never gets signaled.
    std::array<Handle, 5> data_ready_events;

    // Data is polled and synchronized to shared memory whenever this timer fires
    HandleTable::Entry<Timer> data_polling_timer;
    static constexpr const uint32_t data_polling_timer_period = 4'000'000; // Every 4 milliseconds

    // Previous state of binary values, used to store change masks in shared memory
    // TODO: What's the initial value of this? Since the pad state includes
    //       button changes, this value affects the first entry written
    //       during hid execution
    BinaryState previous_binary_state = { 0 };

    decltype(ServiceHelper::SendReply) OnIPCRequest(FakeThread& thread, const IPC::CommandHeader& header);

    OS::ResultAnd<> EnableGyroscope(FakeThread& thread) {
        return RESULT_OK;
    }

    OS::ResultAnd<uint32_t> GetGyroscopeSensitivity(FakeThread& thread) {
        float dps = 14.375; // degrees per second
        uint32_t dps_uint;
        memcpy(&dps_uint, &dps, sizeof(dps));
        return { RESULT_OK, dps_uint };
    }

    OS::ResultAnd<Platform::HID::User::GyroscopeCalibrationData> GetGyroscopeCalibrationData(FakeThread& thread) {
        return { RESULT_OK, Platform::HID::User::GyroscopeCalibrationData {} };
    }

    // Called when data_polling_timer is signaled
    void OnDataPollingTimer(FakeThread& thread) {
        // TODO: Actually, the mcu module should read the Home button status via device 3 and publish this notification

        static bool latch = false;
        // Latch to ensure we don't send out multiple notifications... TODO: Should be moved to notifier!
        auto pressed = Memory::ReadLegacy<uint16_t>(thread.GetParentProcess().interpreter_setup.mem, Memory::IO_HID::start + 0x108);

        if (!latch && pressed) {
            auto [result, srv_session] = thread.CallSVC(&OS::SVCConnectToPort, "srv:");
            if (result != RESULT_OK) {
                throw std::runtime_error("FAILED to get srv:pm handle");
            }

            // Publish notification 0x204: Home button pressed
            static int lol = 0;
            if (lol++ == 0) {
                IPC::SendIPCRequest<Platform::SM::SRV::PublishToSubscriber>(thread, srv_session.first, 0x204, 0); // home
            } else {
                IPC::SendIPCRequest<Platform::SM::SRV::PublishToSubscriber>(thread, srv_session.first, 0x202, 0); // power down
            }

            thread.CallSVC(&OS::SVCCloseHandle, srv_session.first);
        }
        latch = pressed;

        // TODO: cdc:HID GetTouchData (Gets touch screen and circle pad data from SPI device 3)

        auto [result] = thread.CallSVC(&OS::SVCSetTimer, *data_polling_timer.second, data_polling_timer_period, 0);
        if (result != RESULT_OK)
            thread.CallSVC(&OS::SVCBreak, OS::BreakReason::Panic);

        // TODO: Currently, thread.ReadMemory16 does a byte-wise read rather than a 16-bit one, so we need to use an overly complicated Read instead...
        // NOTE: HID button state is a negative mask, i.e. bits that are set indicate *unpressed* buttons
        // TODO: Add circle pad info to the topmost bits of pad_state
        BinaryState binary_state = { static_cast<uint16_t>(~Memory::ReadLegacy<uint16_t>(thread.GetParentProcess().interpreter_setup.mem, Memory::IO_HID::start)) };

        // TODO: These are supposed to be retrieved from cdc:HID:GetTouchData
        uint16_t circle_pad_x = static_cast<uint16_t>(static_cast<int16_t>(Memory::ReadLegacy<uint16_t>(thread.GetParentProcess().interpreter_setup.mem, Memory::IO_HID::start + 0x104)) - 0x9c);
        uint16_t circle_pad_y = static_cast<uint16_t>(static_cast<int16_t>(Memory::ReadLegacy<uint16_t>(thread.GetParentProcess().interpreter_setup.mem, Memory::IO_HID::start + 0x106)) - 0x9c);
        uint32_t circle_pad_state = circle_pad_x | (static_cast<uint32_t>(circle_pad_y) << 16);

        auto [tick] = thread.CallSVC(&OS::SVCGetSystemTick);

        // Button and circle pad state
        {
            uint32_t entry_index = 1 + thread.ReadMemory32(shared_mem_vaddr + 0x10);
            entry_index = entry_index % std::tuple_size_v<decltype(SharedMemorySection<BinaryStateAndCirclePad>::entries)>;
            thread.WriteMemory32(shared_mem_vaddr + 0x10, entry_index);

            if (entry_index == 0) {
                thread.WriteMemory32(shared_mem_vaddr + 0x8, thread.ReadMemory32(shared_mem_vaddr));
                thread.WriteMemory32(shared_mem_vaddr + 0xc, thread.ReadMemory32(shared_mem_vaddr) + 4);
                // TODO: Tick is only supposed to be updated when writing the first entry
                // TODO: Ok, we do that now
                thread.WriteMemory32(shared_mem_vaddr + 0x0, tick & 0xFFFFFFFF);
                thread.WriteMemory32(shared_mem_vaddr + 0x4, tick >> 32);
            }

            thread.GetLogger()->error("PAD State: {:#x}, index {}", binary_state.raw, entry_index);

            float slider3d_percentage = 100.0;
            uint32_t slider3d_percentage_raw;
            memcpy(&slider3d_percentage_raw, &slider3d_percentage, sizeof(slider3d_percentage));
            thread.WriteMemory32(shared_mem_vaddr + 0x18, slider3d_percentage_raw);
            thread.WriteMemory32(shared_mem_vaddr + 0x1c, binary_state.raw);
            thread.WriteMemory32(shared_mem_vaddr + 0x20, circle_pad_state);

            auto entry = BinaryStateAndCirclePad {
                binary_state,
                BinaryState { ~previous_binary_state.raw & binary_state.raw },
                BinaryState { previous_binary_state.raw & ~binary_state.raw },
                circle_pad_state
            };
            thread.WriteMemory32(shared_mem_vaddr + 0x28 + entry_index * sizeof(entry), entry.current_binary.raw);
            thread.WriteMemory32(shared_mem_vaddr + 0x2c + entry_index * sizeof(entry), entry.activated_binary.raw);
            thread.WriteMemory32(shared_mem_vaddr + 0x30 + entry_index * sizeof(entry), entry.deactivated_binary.raw);
            thread.WriteMemory32(shared_mem_vaddr + 0x34 + entry_index * sizeof(entry), circle_pad_state);

            previous_binary_state = binary_state;
        }

        // Touch state
        // TODO: Actually, HID should query this data from cdc:HID's GetData
        //       (which in turn queries it from SPI device number 3)
        //       and convert it to screen coordinates using the touch
        //       calibration data in config block 0x40000 (which in turn
        //       originate in ctrnand:/ro/sys/HWCAL0.dat and HWCAL1.dat)
        {
            auto block_start = shared_mem_vaddr + 0xa8;
            uint32_t entry_index = 1 + thread.ReadMemory32(block_start + 0x10);
            entry_index = entry_index % std::tuple_size_v<decltype(SharedMemorySection<TouchState>::entries)>;
            thread.WriteMemory32(block_start + 0x10, entry_index);

            if (entry_index == 0) {
                thread.WriteMemory32(block_start + 0x8, thread.ReadMemory32(block_start));
                thread.WriteMemory32(block_start + 0xc, thread.ReadMemory32(block_start) + 4);
                // TODO: Tick is only supposed to be updated when writing the first entry
                // TODO: Ok, we do that now
                thread.WriteMemory32(block_start + 0x0, tick & 0xFFFFFFFF);
                thread.WriteMemory32(block_start + 0x4, tick >> 32);
            }

            auto entry = TouchState {
                Memory::ReadLegacy<uint16_t>(thread.GetParentProcess().interpreter_setup.mem, Memory::IO_HID::start + 0x100),
                Memory::ReadLegacy<uint16_t>(thread.GetParentProcess().interpreter_setup.mem, Memory::IO_HID::start + 0x102),
                0
            };
            const bool touched = (0xffff != entry.x || 0xffff != entry.y);
            entry.flags = touched;
            if (!touched) {
                entry.x = 0;
                entry.y = 0;
            }
            thread.WriteMemory32(block_start + 0x20 + entry_index * sizeof(entry), (static_cast<uint32_t>(entry.y) << 16) | entry.x);
            thread.WriteMemory32(block_start + 0x24 + entry_index * sizeof(entry), entry.flags);
        }

        // TODO: Read gyroscope data using mcu::HID

        // TODO: Read accelerometer data

        for (auto& data_ready_event : ranges::view::take(4)(data_ready_events)) {
            std::tie(result) = thread.CallSVC(&OS::SVCSignalEvent, data_ready_event);
            if (result != RESULT_OK)
                thread.CallSVC(&OS::SVCBreak, OS::BreakReason::Panic);

            // TODO: Accelerometer (index 2) and gyroscope (index 3) should be
            //       signaled at a lower frequency (about every 4-5 iterations)
        }
    }
};

FakeHID::FakeHID(FakeThread& thread) : logger(*thread.GetLogger()) {
    // Create shared memory block (0x1000 bytes, matching the native HID module)
    // TODO: Should we zero-initialize this?
    OS::Result result;
    std::tie(result,shared_mem_vaddr) = thread.CallSVC(&OS::SVCControlMemory, 0, 0, shared_mem_size, 3/*ALLOC*/, 0x3/*RW*/);
    if (result != RESULT_OK)
        thread.CallSVC(&OS::SVCBreak, OS::BreakReason::Panic);

    {
        auto [result, shared_memory_entry] = thread.CallSVC(&OS::SVCCreateMemoryBlock, shared_mem_vaddr, shared_mem_size, 0x3/*RW*/, 0x1/*Read-only*/);
        if (result != RESULT_OK)
            thread.CallSVC(&OS::SVCBreak, OS::BreakReason::Panic);
        shared_memory = shared_memory_entry.first;
    }

    // TODO: Read calibration data from cfg blocks 0x40000, 0x40002, and 0x40003

    for (auto& handle : data_ready_events) {
        auto [result, event_entry] = thread.CallSVC(&OS::SVCCreateEvent, ResetType::OneShot);
        if (result != RESULT_OK)
            thread.CallSVC(&OS::SVCBreak, OS::BreakReason::Panic);
        handle = event_entry.first;
    }

    std::tie(result, data_polling_timer) = thread.CallSVC(&OS::SVCCreateTimer, ResetType::OneShot);
    if (result != RESULT_OK)
        thread.CallSVC(&OS::SVCBreak, OS::BreakReason::Panic);

    std::tie(result) = thread.CallSVC(&OS::SVCSetTimer, *data_polling_timer.second, data_polling_timer_period, 0);
    if (result != RESULT_OK)
        thread.CallSVC(&OS::SVCBreak, OS::BreakReason::Panic);

    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, "hid:USER", 5));
    service.Append(ServiceUtil::SetupService(thread, "hid:SPVR", 5)); // Same as hid:USER; used by Home Menu and other system software
    service.Append(data_polling_timer);

    auto InvokeCommandHandler = [&](FakeThread& thread, uint32_t signalled_handle_index) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        auto signalled_handle = service.handles[signalled_handle_index];
        if (signalled_handle == data_polling_timer.first) {
            OnDataPollingTimer(thread);
            return ServiceHelper::DoNothing;
        } else {
            return OnIPCRequest(thread, header);
        }
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

template<typename Class, typename Func>
static auto BindMemFn(Func f, Class* c) {
    return [f,c](auto&&... args) { return std::mem_fn(f)(c, args...); };
}

decltype(ServiceHelper::SendReply) FakeHID::OnIPCRequest(FakeThread& thread, const IPC::CommandHeader& header) {
    namespace HIDU = Platform::HID::User;

    switch (header.command_id) {
    case 0xa: // GetIPCHandles
    {
        logger.info("{} received GetIPCHandles", ThreadPrinter{thread});

        // Validate command
        if (header.raw != 0x000A0000)
            thread.CallSVC(&OS::SVCBreak, OS::BreakReason::Panic);

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 7).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, IPC::TranslationDescriptor::MakeHandles(6).raw);
        thread.WriteTLS(0x8c, shared_memory.value);
        thread.WriteTLS(0x90, data_ready_events[0].value);
        thread.WriteTLS(0x94, data_ready_events[1].value);
        thread.WriteTLS(0x98, data_ready_events[2].value);
        thread.WriteTLS(0x9c, data_ready_events[3].value);
        thread.WriteTLS(0xa0, data_ready_events[4].value);

        break;
    }

    case 0x11: // EnableAccelerometer
        logger.info("{}received EnableAccelerometer", ThreadPrinter{thread});

        // Sure, whatever
//         LogStub(header);
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0x12: // DisableAccelerometer
        logger.info("{}received DisableAccelerometer", ThreadPrinter{thread});

        // Sure, whatever
//         LogStub(header);
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case HIDU::EnableGyroscope::id:
        IPC::HandleIPCCommand<HIDU::EnableGyroscope>(BindMemFn(&FakeHID::EnableGyroscope, this), thread, thread);
        break;

    case 0x14: // DisableGyroscope
        logger.info("{}received DisableGyroscope", ThreadPrinter{thread});

        // Sure, whatever
//         LogStub(header);
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case HIDU::GetGyroscopeSensitivity::id:
        IPC::HandleIPCCommand<HIDU::GetGyroscopeSensitivity>(BindMemFn(&FakeHID::GetGyroscopeSensitivity, this), thread, thread);
        break;

    case HIDU::GetGyroscopeCalibrationData::id:
        IPC::HandleIPCCommand<HIDU::GetGyroscopeCalibrationData>(BindMemFn(&FakeHID::GetGyroscopeCalibrationData, this), thread, thread);
        break;

    case 0x17: // GetSoundVolume
        logger.info("{}received GetSoundVolume", ThreadPrinter{thread});

        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0x3f); // Maximum volume
        break;

    default:
        throw std::runtime_error(fmt::format("Unknown HID IPC request with header {:#x}", header.raw));
    }
    return ServiceHelper::SendReply;
}

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<FakeHID>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<FakeHID>(os, setup, pid, name);
}

}  // namespace OS

}  // namespace HLE
