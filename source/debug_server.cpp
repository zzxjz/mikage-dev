#include "debug_server.hpp"

#include <pistache/endpoint.h>
#include <pistache/router.h>

#include <range/v3/algorithm/find_if.hpp>
#include <range/v3/view/transform.hpp>
#include <range/v3/view/indirect.hpp>

#include <string>

using namespace Pistache;

namespace Debugger {

// Forward declare external services
template<> std::unique_ptr<Service> CreateService<struct JitService>();
template<> std::unique_ptr<Service> CreateService<struct OSService>();
template<> std::unique_ptr<Service> CreateService<struct GPUService>();

class DebugServerImpl {
    std::vector<std::unique_ptr<Service>> services;

public:
    DebugServerImpl() {
        services.push_back(CreateService<GPUService>());
        services.push_back(CreateService<JitService>());
        services.push_back(CreateService<OSService>());
    }

    void Run(unsigned port) {
        Address addr(Ipv4::any(), static_cast<uint16_t>(port));
        Http::Endpoint endpoint(addr);
        endpoint.init(Http::Endpoint::options().flags(Tcp::Options::NoDelay | Tcp::Options::ReuseAddr));
        Rest::Router router;

        // Register services
        for (auto& service : services) {
            service->RegisterRoutes(router);
        }

        // Run server
        endpoint.setHandler(router.handler());
        endpoint.serve();
    }

    Service& GetService(const std::type_info& service_type) const {
        auto it = ranges::find_if(services,
                                  [&](const auto& service) -> bool {
                                        auto& the_service = *service;
                                        return (service_type == typeid(the_service));
                                  });
        if (it == services.end()) {
            throw std::runtime_error("Could not find service" + std::string { service_type.name() });
        }
        return **it;
    }
};

DebugServer::DebugServer() {
    impl = std::make_unique<DebugServerImpl>();
}

DebugServer::~DebugServer() = default;

void DebugServer::Run(unsigned port) {
    impl->Run(port);
}

Service& DebugServer::GetService(const std::type_info& service_type) const {
    return impl->GetService(service_type);
}

} // namespace Debugger
