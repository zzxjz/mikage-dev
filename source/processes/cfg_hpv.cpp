#include "cfg_hpv.hpp"
#include "os_hypervisor_private.hpp"
#include "os.hpp"

#include <platform/config.hpp>

#include <framework/exceptions.hpp>

namespace HLE {

namespace OS {

namespace HPV {

namespace {

struct CfgService : SessionToPort {
    CfgService(RefCounted<Port> port_, CFGContext& context_) : SessionToPort(port_, context_) {
    }

    void OnRequest(Hypervisor& hypervisor, Thread& thread, Handle session) override {
        const uint32_t command_header = thread.ReadTLS(0x80);
        auto dispatcher = RequestDispatcher<> { thread, *this, command_header };

        namespace Cmd = Platform::Config;

        dispatcher.DecodeRequest<Cmd::GetConfigInfoBlk2>([&](auto& response, uint32_t size, uint32_t block_id, IPC::MappedBuffer output) {
            auto description = fmt::format( "GetConfigInfoBlk2, size={:#x}, block_id={:#x}",
                                            size, block_id);
            Session::OnRequest(hypervisor, thread, session, description);
        });
    }
};

} // anonymous namespace

HPV::RefCounted<Object> CreateCfgService(RefCounted<Port> port, CFGContext& context) {
    return HPV::RefCounted<Object>(new CfgService(port, context));
}

} // namespace HPV

} // namespace OS

} // namespace HLE
