#include "context.h"
#include "shader.hpp"

#include <framework/exceptions.hpp>
#include <framework/settings.hpp>

#if ENABLE_PISTACHE
#include <debug/gpu.hpp>
#include <debug_server.hpp>
#endif

#include <framework/exceptions.hpp>

#include <range/v3/algorithm/copy.hpp>
#include <range/v3/view/indices.hpp>

#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>

#include <xxhash.h>

#include <unordered_map>

namespace Pica::VertexShader {

namespace detail {
struct identity_hash {
    XXH64_hash_t operator()(const XXH64_hash_t& hash) const noexcept {
        return hash;
    }
};
}

struct EnginePoolImpl {
    XXH64_hash_t shader_hash;
    std::unique_ptr<XXH64_state_t, XXH_errorcode(*)(XXH64_state_t*)> hash_state { XXH64_createState(), XXH64_freeState };

    using EngineMap = std::unordered_map<XXH64_hash_t, std::unique_ptr<ShaderEngine>, detail::identity_hash>;
    EngineMap engines;
};

EnginePool::EnginePool() : impl(new EnginePoolImpl) {

}

EnginePool::~EnginePool() = default;

void EnginePool::SetDirty() {
    dirty = true;
}

ShaderEngine& EnginePool::GetOrCompile(Context& context, Settings::ShaderEngine engine_type, uint32_t entry_point) {
    if (dirty) {
        TracyCZoneN(VertexShaderHash, "Hash", true);

        // TODO: Also hash swizzle patterns...
        XXH64_reset(impl->hash_state.get(), 0);
        XXH64_update(impl->hash_state.get(), context.shader_memory.data(), sizeof(context.shader_memory));
        // NOTE: Super Mario 3D Land (W1-2, during regular play through before the checkpoint) has a duplicate if the entry point is not included in the shader hash
        XXH64_update(impl->hash_state.get(), &entry_point, sizeof(entry_point));
        // TODO: Don't hash null-blocks at the end to increase entropy
        auto bool_uniforms = context.registers.vs_bool_uniforms.Value();
        XXH64_update(impl->hash_state.get(), &bool_uniforms, sizeof(bool_uniforms));

        impl->shader_hash = XXH64_digest(impl->hash_state.get());

        TracyCZoneEnd(VertexShaderHash);

        dirty = false;
    }

    auto engine_it = impl->engines.find(impl->shader_hash);
    if (engine_it == impl->engines.end()) {
#if ENABLE_PISTACHE
        // Register to debugger
        auto& debug_service = context.debug_server->GetService<Debugger::GPUService>();

        // TODO: Check if a debugger is actually connected
        std::vector<uint32_t> instructions(context.shader_memory.size());
        ranges::copy(context.shader_memory, instructions.data());
        std::vector<uint32_t> swizzle_data(context.swizzle_data.size());
        ranges::copy(context.swizzle_data, swizzle_data.data());
        debug_service.RegisterShader(   impl->shader_hash,
                                        std::move(instructions),
                                        std::move(swizzle_data),
                                        entry_point,
                                        context.registers.vs_bool_uniforms.Value(),
                                        context.registers.shader_output_semantics);
#endif

        std::unique_ptr<ShaderEngine> engine;
        if (engine_type == Settings::ShaderEngine::Interpreter) {
            engine = VertexShader::CreateInterpreter();
        } else if (engine_type == Settings::ShaderEngine::Bytecode) {
            engine = VertexShader::CreateMicroCodeRecompiler(context);
        } else {
            ValidateContract(false);
        }

        engine_it = impl->engines.insert({ impl->shader_hash, std::move(engine) }).first;
    }

    // TODO: If a debugger is actually connected, check if this shader's debug data is up to date

    return *engine_it->second;
}

} // namespace Pica::VertexShader
