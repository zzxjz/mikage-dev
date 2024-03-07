#include "ro_hpv.hpp"
#include "os_hypervisor_private.hpp"
#include "os.hpp"

#include <framework/exceptions.hpp>

namespace HLE {

namespace OS {

namespace HPV {

namespace {

struct RoService : SessionToPort {
    RoService(RefCounted<Port> port_, ROContext& context_) : SessionToPort(port_, context_) {
    }

    void OnRequest(Hypervisor& hypervisor, Thread& thread, Handle session) override {
        const uint32_t command_header = thread.ReadTLS(0x80);
        auto dispatcher = RequestDispatcher<> { thread, *this, command_header };

// TODO: Also add UnloadCRO (command 5)

        using LoadCRO = IPC::IPCCommand<0x4>::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_handle<IPC::HandleType::Process>::response::add_uint32;
        using LoadCRONew = IPC::IPCCommand<0x9>::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_uint32::add_handle<IPC::HandleType::Process>::response::add_uint32;
        dispatcher.DecodeRequest<LoadCRO>([&](auto& response, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, Handle) {
            Session::OnRequest(hypervisor, thread, session, "LoadCRO");
            response.OnResponse([](Hypervisor&, Thread&, Result result, uint32_t) {
                if (result != RESULT_OK) {
                    throw Mikage::Exceptions::Invalid("LoadCRO failed");
                }
            });
        });
        dispatcher.DecodeRequest<LoadCRONew>([&](auto& response, uint32_t source_addr, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, Handle process_handle) {
            Session::OnRequest(hypervisor, thread, session, "LoadCRONew");

            auto source_process = thread.GetProcessHandleTable().FindObject<EmuProcess>(process_handle);

            auto magic = source_process->ReadMemory32(source_addr + 0x80);
            if (magic != 0x304f5243) {
                throw Mikage::Exceptions::Invalid("LoadCRONew argument is not a valid CRO file");
            }

            response.OnResponse([](Hypervisor&, Thread&, Result result, uint32_t) {
                if (result == 0xd8e12c03) {
                    throw Mikage::Exceptions::Invalid("LoadCRONew failed (invalid hash?)");
                }
                if (result != RESULT_OK) {
                    throw Mikage::Exceptions::Invalid("LoadCRONew failed");
                }
            });
        });
    }
};

} // anonymous namespace

HPV::RefCounted<Object> CreateRoService(RefCounted<Port> port, ROContext& context) {
    return HPV::RefCounted<Object>(new RoService(port, context));
}

} // namespace HPV

} // namespace OS

} // namespace HLE
