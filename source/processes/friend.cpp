#include "friend.hpp"
#include "fake_process.hpp"

namespace HLE {

namespace OS {

static auto FRDUCommandHandler(FakeFRIEND& context, FakeThread& thread, Platform::IPC::CommandHeader header) try {
    switch (header.command_id) {
    // GetMyFriendKey
    case 0x5:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        // TODO: Should return 4 more words
        break;

    // GetMyProfile
    case 0x7:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        // TODO: Should return 2 more words
        break;

    // GetMyPresence
    case 0x8:
        // TODO: This is supposed to fill out a static buffer in the source process
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // GetMyScreenName
    // Used by Bravely Default
    case 0x9:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 6, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        // "Mikage"
        thread.WriteTLS(0x88, 0x69004d00);
        thread.WriteTLS(0x8c, 0x61006b00);
        thread.WriteTLS(0x90, 0x65006700);
        thread.WriteTLS(0x94, 0);
        thread.WriteTLS(0x98, 0);
        break;

    // GetMyMii
    case 0xa:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        // TODO: Should return a lot more words
        break;

    // GetMyPlayingGame
    case 0xc:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 5, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0);
        thread.WriteTLS(0x8c, 0);
        thread.WriteTLS(0x90, 0);
        thread.WriteTLS(0x94, 0);
        break;

    // GetFriendKeyList
    case 0x11:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 2, 2).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        thread.WriteTLS(0x88, 0); /* Number of results */
        thread.WriteTLS(0x8c, IPC::TranslationDescriptor::MakeStaticBuffer(0, 0/* 0 bytes */).raw);
        thread.WriteTLS(0x90, thread.ReadTLS(0x180));
        break;

    // GetFriendPresence
    case 0x12:
        // TODO: This is supposed to fill out a static buffer in the source process
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // GetFriendScreenName
    case 0x13:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // GetFriendMii
    case 0x14:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // GetFriendProfile
    case 0x15:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // GetFriendAttributeFlags
    case 0x17:
        // TODO: This is supposed to fill out a static buffer in the source process
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // UpdateGameMode
    case 0x1e:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // AttachToEventNotification
    case 0x20:
        // TODO: Parse inputs. Just takes an event handle.
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetNotificationMask
    case 0x21:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // SetClientSdkVersion
    case 0x32:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    default:
        throw IPC::IPCError{header.raw, 0xdeadbeef};
    }

    return ServiceHelper::SendReply;
} catch (const IPC::IPCError& err) {
    throw std::runtime_error(fmt::format("Unknown frd:u/frd:a service command with header {:#010x}", err.header));
}

static auto FRDACommandHandler(FakeFRIEND& context, FakeThread& thread, Platform::IPC::CommandHeader header) try {
    switch (header.command_id) {
    case 0x405:
        // Command header is 0x04050000, i.e. no parameters
        // The response only includes a result code
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    // 3dbrew erratum: Not documented. (Takes four words as inputs; the first two seem to be a title id?)
    case 0x40a:
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    case 0x40c:
        // Command header is 0x040c0800, i.e. 32 normal parameters
        // Starting from the 25th parameter (likely up to the end), a null-terminated UTF-16 Mii name is inlined
        // The response only includes a result code
        thread.WriteTLS(0x80, IPC::CommandHeader::Make(0, 1, 0).raw);
        thread.WriteTLS(0x84, RESULT_OK);
        break;

    default:
        return FRDUCommandHandler(context, thread, header);
    }

    return ServiceHelper::SendReply;
} catch (const IPC::IPCError& err) {
    throw std::runtime_error(fmt::format("Unknown frd:a service command with header {:#010x}", err.header));
}

template<typename Handler>
static void IPCThread(FakeFRIEND& context, FakeThread& thread, const char* service_name, Handler&& handler) {
    for (auto buffer_index : {0, 2}) {
        const auto buffer_size = 0x640;
        thread.WriteTLS(0x180 + 8 * buffer_index, IPC::TranslationDescriptor::MakeStaticBuffer(0, buffer_size).raw);
        Result result;
        uint32_t buffer_addr;
        std::tie(result, buffer_addr) = thread.CallSVC(&OS::SVCControlMemory, 0, 0, 0x1000, 3 /* COMMIT */, 3 /* RW */);
        thread.WriteTLS(0x184 + 8 * buffer_index, buffer_addr);
    }

    // TODO: How many parallel sessions should we support?
    ServiceHelper service;
    service.Append(ServiceUtil::SetupService(thread, service_name, /*1*/2));

    auto InvokeCommandHandler = [&context,handler=std::move(handler)](FakeThread& thread, uint32_t) {
        Platform::IPC::CommandHeader header = { thread.ReadTLS(0x80) };
        return handler(context, thread, header);
    };

    service.Run(thread, std::move(InvokeCommandHandler));
}

FakeFRIEND::FakeFRIEND(FakeThread& thread) {
    thread.name = "frd:uThread";

    auto new_thread = std::make_shared<WrappedFakeThread>(thread.GetParentProcess(), [this](FakeThread& thread) { return IPCThread(*this, thread, "frd:a", FRDACommandHandler); });
    new_thread->name = "frd:aThread";
    thread.GetParentProcess().AttachThread(new_thread);

    IPCThread(*this, thread, "frd:u", FRDUCommandHandler);
}

}  // namespace OS

}  // namespace HLE
