#pragma once

#if ENABLE_PISTACHE
#define DEBUG_SERVER_AVAILABLE
#endif

#include <memory>
#include <string_view>

#ifdef DEBUG_SERVER_AVAILABLE
namespace Pistache {
namespace Rest {
class Request;
class Router;
}
namespace Http {
class ResponseWriter;
}
}
#endif

namespace Debugger {

#ifdef DEBUG_SERVER_AVAILABLE
struct Service {
    virtual ~Service() = default;

    virtual void RegisterRoutes(Pistache::Rest::Router& router) = 0;
};

/**
 * Factory function to create a service of the given type.
 * Must be specialized by service implementors
 */
template<typename ConcreteService>
std::unique_ptr<Service> CreateService();

class DebugServerImpl;
#endif

class DebugServer {
#ifdef DEBUG_SERVER_AVAILABLE
    std::unique_ptr<DebugServerImpl> impl;

    Service& GetService(const std::type_info& service_type) const;
#endif

public:
#ifdef DEBUG_SERVER_AVAILABLE
    DebugServer();
    ~DebugServer();

    void Run(unsigned port);

    template<typename ConcreteService>
    ConcreteService& GetService() const {
        return static_cast<ConcreteService&>(GetService(typeid(ConcreteService)));
    }
#endif
};

} // namespace Debugger
