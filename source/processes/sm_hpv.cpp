#include "sm_hpv.hpp"
#include "os_hypervisor_private.hpp"
#include "os.hpp"

#include <platform/sm.hpp>

#include <framework/exceptions.hpp>

#include <range/v3/view/iota.hpp>
#include <range/v3/view/intersperse.hpp>
#include <range/v3/view/transform.hpp>
#include <range/v3/numeric/accumulate.hpp>

namespace HLE {

namespace OS {

namespace HPV {

namespace {

struct SrvService : SessionToPort {
    SrvService(RefCounted<Port> port, SMContext& context) : SessionToPort(port, context) {
    }

    void OnRequest(Hypervisor& hypervisor, Thread& thread, Handle session) override {
        namespace SMSRV = Platform::SM::SRV;

        const uint32_t command_header = thread.ReadTLS(0x80);
        auto dispatcher = RequestDispatcher<> { thread, *this, command_header };

        dispatcher.DecodeRequest<SMSRV::RegisterClient>([&](auto&, ProcessId process_id) {
            Session::OnRequest(hypervisor, thread, session, fmt::format("RegisterClient, process {}", process_id));
        });

        dispatcher.DecodeRequest<SMSRV::EnableNotification>([&](auto& response) {
            Session::OnRequest(hypervisor, thread, session, "EnableNotification");

            response.OnResponse([=](Hypervisor&, Thread& thread, Result result, Handle notification_handle) {
                // TODO: Check result code

                auto semaphore = thread.GetProcessHandleTable().FindObject<Semaphore>(notification_handle);
                assert(semaphore);
                semaphore->name = "SMNotification";
            });
        });

        dispatcher.DecodeRequest<SMSRV::RegisterService>([&](auto& response, const Platform::SM::PortName& port_name, uint32_t max_sessions) {
            auto description = fmt::format("RegisterService, name=\"{}\", max_sessions={:#x}",
                                           port_name.ToString(), max_sessions);
            Session::OnRequest(hypervisor, thread, session, description);

            response.OnResponse([=](Hypervisor& hv, Thread& thread, Result result, Handle port_handle) {
                if (result != RESULT_OK) {
                    throw Mikage::Exceptions::Invalid("Failed to register service {}", port_name.ToString());
                }

                hv.SetPortName(thread.GetParentProcess().GetId(), port_handle, port_name.ToString());

                auto server_port = thread.GetProcessHandleTable().FindObject<ServerPort>(port_handle);
                assert(server_port);
                server_port->name = "SPort_" + port_name.ToString();
                if (auto client_port = server_port->port->client.lock()) {
                    client_port->name = "CPort_" + port_name.ToString();
                }
            });
        });

        dispatcher.DecodeRequest<SMSRV::GetServiceHandle>([&](auto& response, const Platform::SM::PortName& port_name, uint32_t flags) {
            auto description = fmt::format("GetServiceHandle, name=\"{}\", flags={:#x}",
                                           port_name.ToString(), flags);
            Session::OnRequest(hypervisor, thread, session, description);

            response.OnResponse([=](Hypervisor&, Thread& thread, Result result, Handle session_handle) {
                if (result != RESULT_OK) {
                    throw Mikage::Exceptions::Invalid("Failed to get handle for service {}", port_name.ToString());
                }

                auto client_session = thread.GetProcessHandleTable().FindObject<ClientSession>(session_handle);
                assert(client_session);
                client_session->name = "CSession_" + port_name.ToString();
            });
        });

        dispatcher.DecodeRequest<SMSRV::Subscribe>([&](auto&, uint32_t notification_id) {
            auto description = fmt::format("Subscribe, id={:#x}", notification_id);
            Session::OnRequest(hypervisor, thread, session, description);
        });

        dispatcher.OnUnknown([&]() { Session::OnRequest(hypervisor, thread, session); });
    }
};

struct SrvPmService : SrvService {
    SrvPmService(RefCounted<Port> port, SMContext& context) : SrvService(port, context) {
    }

    void OnRequest(Hypervisor& hypervisor, Thread& thread, Handle session) override {
        namespace SMSRVPM = Platform::SM::SRVPM;

        const uint32_t command_header = thread.ReadTLS(0x80);
        auto dispatcher = RequestDispatcher<> { thread, *this, command_header };

        dispatcher.DecodeRequest<SMSRVPM::RegisterProcess>([&](auto& response, ProcessId process_id, uint32_t service_list_num_words, IPC::StaticBuffer service_list) {
            if (service_list_num_words * 4 > service_list.size) {
                throw std::runtime_error(fmt::format("RegisterProcess: {} bytes of data requested, but only {} available", service_list_num_words * 4, service_list.size));
            }

            auto description = fmt::format("RegisterProcess, pid={}, services=[",
                                           process_id);
            auto read_port_name = [&](auto index) {
                auto addr = service_list.addr + index * 8;
                return Platform::SM::PortName::IPCDeserialize(thread.ReadMemory32(addr), thread.ReadMemory32(addr + 4), 8).ToString();
            };
            description += ranges::accumulate(ranges::view::iota(uint32_t { }, service_list_num_words / 2)
                                              | ranges::view::transform(read_port_name)
                                              | ranges::view::intersperse(", "), std::string{});
            description += ']';
            Session::OnRequest(hypervisor, thread, session, description);
        });

        dispatcher.OnUnknown([&]() { SrvService::OnRequest(hypervisor, thread, session); });
    }
};

} // anonymous namespace

HPV::RefCounted<Object> CreateSMSrvService(RefCounted<Port> port, SMContext& context) {
    return HPV::RefCounted<Object>(new SrvService(port, context));
}

HPV::RefCounted<Object> CreateSMSrvPmService(RefCounted<Port> port, SMContext& context) {
    return HPV::RefCounted<Object>(new SrvPmService(port, context));
}

} // namespace HPV

} // namespace HOS

} // namespace HLE
