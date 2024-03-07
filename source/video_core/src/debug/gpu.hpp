#pragma once

#include "../video_core/shader.hpp"

#include <platform/gpu/pica.hpp>

#include <debug_server.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

namespace Debugger {

namespace detail {
struct identity_hash {
    XXH64_hash_t operator()(const XXH64_hash_t& hash) const noexcept {
        return hash;
    }
};
}

// TODO: Rename to ShaderService
struct GPUService : Service {
    struct ShaderData {
        std::vector<uint32_t> instructions;
        std::vector<uint32_t> swizzle_data;
        uint16_t bool_uniforms;
        uint32_t entry_point;

        Pica::Regs::VSOutputAttributes::Semantic output_semantics[4 * std::tuple_size_v<decltype(Pica::Regs::shader_output_semantics)>];
    };

    std::mutex access_mutex;
    std::unordered_map<XXH64_hash_t, ShaderData, detail::identity_hash> shaders;

    void RegisterShader(const XXH64_hash_t& key,
                        std::vector<uint32_t> instructions,
                        std::vector<uint32_t> swizzle_data,
                        uint32_t entry_point,
                        uint16_t bool_uniforms,
                        const decltype(Pica::Regs::shader_output_semantics)&);

    void Shutdown();

    void RegisterRoutes(Pistache::Rest::Router&) override;
};

} // namespace Debugger
