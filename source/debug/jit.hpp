#pragma once

#include <debug_server.hpp>

#include <unordered_map>

namespace HLE::OS {
using ProcessId = uint32_t;
using ThreadId = uint32_t;
}

struct JitDebugInfo;

namespace Debugger {

class JitId {
    uint64_t value;

    JitId(uint64_t value) : value(value) { }

public:
    JitId(HLE::OS::ProcessId pid, HLE::OS::ThreadId tid) : value { (uint64_t { pid } << 32) | tid } {

    }

    static JitId FromRaw(uint64_t value) {
        return JitId { value };
    }

    std::pair<HLE::OS::ProcessId, HLE::OS::ThreadId> Decode() const {
        HLE::OS::ProcessId pid = value >> 32;
        HLE::OS::ThreadId tid = value & 0xffffffff;
        return std::make_pair(pid, tid);
    }

    struct Hasher {
        auto operator()(JitId self) const {
            return std::hash<uint64_t>{}(self.value);
        }
    };

    bool operator==(JitId oth) const {
        return value == oth.value;
    }
};

struct JitService : Service {
    std::unordered_map<JitId, JitDebugInfo*, JitId::Hasher> contexts;

    void RegisterContext(HLE::OS::ProcessId pid, HLE::OS::ThreadId tid, JitDebugInfo& jit_context);
    void UnregisterContext(HLE::OS::ProcessId pid, HLE::OS::ThreadId tid);

    void doOptions(const Pistache::Rest::Request&, Pistache::Http::ResponseWriter response);

    void doJitBlocks(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response);

    void RegisterRoutes(Pistache::Rest::Router& router) override;
};

} // namespace Debugger
