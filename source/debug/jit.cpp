#include "jit.hpp"

#ifndef __ANDROID__
#include <pistache/router.h>
#endif

#include <range/v3/view/enumerate.hpp>

#include <fmt/format.h>

using namespace Pistache;

// TODO: Move to common header...
struct CompiledBlockInfo {
    uint32_t instrs_arm;
    uint32_t instrs_llvm;
};

// TODO: Move to common header...
struct JitDebugInfo {
    std::map<uint32_t, CompiledBlockInfo> compiled_blocks[2];
};

namespace Debugger {

void JitService::RegisterContext(HLE::OS::ProcessId pid, HLE::OS::ThreadId tid, JitDebugInfo& jit_context) {
    // TODO: Assert it's a new one
    contexts.emplace(JitId { pid, tid }, &jit_context);
}

void JitService::UnregisterContext(HLE::OS::ProcessId pid, HLE::OS::ThreadId tid) {
    contexts.erase(JitId { pid, tid });
}

#ifndef __ANDROID__
void JitService::doOptions(const Rest::Request&, Http::ResponseWriter response) {
    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");
    response.headers().add<Http::Header::AccessControlAllowHeaders>("user-agent");
    response.send(Http::Code::No_Content);
}

void JitService::doJitBlocks(const Rest::Request& request, Http::ResponseWriter response) {
    auto id = JitId::FromRaw(request.param(":id").as<uint64_t>());
    auto it = contexts.find(id);
    if (it == contexts.end()) {
        auto [pid, tid] = id.Decode();
        throw std::runtime_error(fmt::format("Tried to access invalid JIT with pid {} and tid {}", pid, tid));
    }

    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");

    // Iterate over compiled blocks, merging the ARM and Thumb blocks while sorting by their starting address
    std::string body = "[";
    std::size_t block_id = 0;
    auto block_it_arm = it->second->compiled_blocks[0].begin();
    auto block_it_thumb = it->second->compiled_blocks[1].begin();
    const auto block_it_arm_end = it->second->compiled_blocks[0].end();
    const auto block_it_thumb_end = it->second->compiled_blocks[1].end();
    while ( block_it_arm != block_it_arm_end ||
            block_it_thumb != block_it_thumb_end) {
        bool is_thumb;

        if (block_it_thumb == block_it_thumb_end) {
            is_thumb = false;
        } else if (block_it_arm == block_it_arm_end) {
            is_thumb = true;
        } else if (block_it_arm->first <= block_it_thumb->first) {
            is_thumb = false;
        } else {
            is_thumb = true;
        }

        auto block_it = (is_thumb ? block_it_thumb++ : block_it_arm++);

        uint32_t addr = block_it->first;
        auto& block = block_it->second;
        body += fmt::format("{{ \"id\": {}, \"addr\": {}, \"thumb\": {}, \"instrs_native\": {}, \"instrs_ir\": {}, \"instrs_compiled\": 0 }},",
                            ++block_id, addr, is_thumb ? "true" : "false", block.instrs_arm, block.instrs_llvm);
    }

    // Drop trailing comma
    if (body.size() > 1) {
        body.pop_back();
    }

    response.send(Http::Code::Ok, body + "]");
}
#endif

void JitService::RegisterRoutes(Rest::Router& router) {
#ifndef __ANDROID__
    using namespace Rest;

    Routes::Get(router, "/jit/:id/blocks", Routes::bind(&JitService::doJitBlocks, this));

    // TODO: Move those to DebugServer itself
    Routes::Options(router, "/jit/*/*", Routes::bind(&JitService::doOptions, this));
    Routes::Options(router, "/jit/*", Routes::bind(&JitService::doOptions, this));
    Routes::Options(router, "/jit", Routes::bind(&JitService::doOptions, this));
#endif
}

template<>
std::unique_ptr<Service> CreateService<JitService>() {
    return std::make_unique<JitService>();
}

} // namespace Debugger
