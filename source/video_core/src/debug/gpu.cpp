#include "gpu.hpp"
#include "../src/video_core/shader.hpp"
#include "../src/video_core/shader_microcode.hpp"
#include "../src/video_core/debug_utils/debug_utils.h"

#if ENABLE_PISTACHE == 1
#include <pistache/router.h>
#endif

#include <fmt/format.h>

#include <range/v3/algorithm/find_if.hpp>
#include <range/v3/view/cache1.hpp>
#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/join.hpp>
#include <range/v3/view/remove_if.hpp>
#include <range/v3/view/split.hpp>
#include <range/v3/view/transform.hpp>

namespace Debugger {

void GPUService::RegisterShader(const XXH64_hash_t& key,
                                std::vector<uint32_t> instructions,
                                std::vector<uint32_t> swizzle_data,
                                uint32_t entry_point,
                                uint16_t bool_uniforms,
                                const decltype(Pica::Regs::shader_output_semantics)& output_attributes) {
    std::lock_guard guard(access_mutex);

    if (shaders.count(key)) {
        throw std::runtime_error("A shader with this key is already registered");
    }

    auto& shader = shaders[key];
    shader.instructions = std::move(instructions);
    shader.swizzle_data = std::move(swizzle_data);
    shader.bool_uniforms = bool_uniforms;
    shader.entry_point = entry_point;

    using Attributes = Pica::Regs::VSOutputAttributes;
    auto decode_attributes = [](const Attributes& attr) -> std::array<Attributes::Semantic, 4> {
        return {{ attr.map_x, attr.map_y, attr.map_z, attr.map_w }};
    };
    ranges::copy(   output_attributes | ranges::views::transform(decode_attributes) | ranges::views::cache1 | ranges::views::join,
                    shader.output_semantics);
}

#if ENABLE_PISTACHE == 1
using namespace Pistache;

static void DoOptions(const Pistache::Rest::Request&, Pistache::Http::ResponseWriter response) {
    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");
    response.headers().add<Http::Header::AccessControlAllowHeaders>("user-agent");
    response.send(Http::Code::No_Content);
}

static void DoListShaders(GPUService& service, const Pistache::Rest::Request&, Pistache::Http::ResponseWriter response) {
    std::lock_guard guard(service.access_mutex);

    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");
    response.headers().add<Http::Header::ContentType>(Http::Mime::MediaType { Http::Mime::Type::Application, Http::Mime::Subtype::Json });

    std::string body = "[";
    std::size_t id = 0;
    for (auto& key_and_shader : service.shaders) {
        auto& key = key_and_shader.first;

        body += fmt::format(R"({{ "id": {}, "key": "{:#x}", "sources": "[{}]" }},)",
                            id++, static_cast<uint64_t>(key), "pica, cfg");
    }
    if (!service.shaders.empty()) {
        // Drop trailing comma
        body.pop_back();
    }

    response.send(Http::Code::Ok, body + "]");
}

static void DoPrintPicaDisassembly(GPUService& service, const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
    std::lock_guard guard(service.access_mutex);

    auto key = std::stoull(request.param(":key").as<std::string>(), 0, 16);

    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");
    response.headers().add<Http::Header::ContentType>(Http::Mime::MediaType { Http::Mime::Type::Application, Http::Mime::Subtype::Json });

    const auto& shader = service.shaders.at(key);
    std::string body = "[";

    auto format_instruction = [&](auto instr_and_offset) {
        return fmt::format( R"({{ "id": {}, "code": "{}" }})",
                            instr_and_offset.first,
                            Pica::VertexShader::DisassembleShader(instr_and_offset.second, shader.swizzle_data.data()));
    };
    namespace views = ranges::views;
    body += views::enumerate(shader.instructions) | views::transform(format_instruction) | views::cache1 | views::join(std::string_view(", ")) | ranges::to<std::string>;
    body += ']';

    response.send(Http::Code::Ok, body);
}

static void DoGetControlFlowGraph(GPUService& service, const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
    std::lock_guard guard(service.access_mutex);

    auto key = std::stoull(request.param(":key").as<std::string>(), 0, 16);

    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");
    response.headers().add<Http::Header::ContentType>(Http::Mime::MediaType { Http::Mime::Type::Application, Http::Mime::Subtype::Json });

    const auto& shader = service.shaders.at(key);
    const auto& analysis = Pica::VertexShader::AnalyzeShader(   shader.instructions.data(),
                                                                shader.instructions.size(),
                                                                shader.bool_uniforms,
                                                                shader.entry_point);

    if (!analysis.program) {
       response.send(Http::Code::Ok, "[]");
       return;
    }

    std::string body = "[";
    body += R"({ "id": 0, "code": ")";
    auto cfg = VisualizeShaderControlFlowGraph( *analysis.program,
                                                shader.instructions.data(),
                                                shader.instructions.size(),
                                                shader.swizzle_data.data(),
                                                shader.swizzle_data.size());

    // Manually escape line breaks and quotes
    for (auto c : cfg) {
        if (c == '\n') {
            body += "\\n";
        } else if (c == '"') {
            body += "\\\"";
        } else {
            body += c;
        }
    }
    body += "\"}]";

    response.send(Http::Code::Ok, body);
}

static void DoGetBytecode(GPUService& service, const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
    using Pica::VertexShader::MicroOpOffset;

    std::lock_guard guard(service.access_mutex);

    auto key = std::stoull(request.param(":key").as<std::string>(), 0, 16);

    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");
    response.headers().add<Http::Header::ContentType>(Http::Mime::MediaType { Http::Mime::Type::Application, Http::Mime::Subtype::Json });

    const auto& shader = service.shaders.at(key);
    // TODO: Don't redundantly analyze the shader all the time... instead, cache the result
    const auto& analysis = Pica::VertexShader::AnalyzeShader(   shader.instructions.data(),
                                                                shader.instructions.size(),
                                                                shader.bool_uniforms,
                                                                shader.entry_point);

    // TODO: Int uniforms!
    auto micro_code = Pica::VertexShader::RecompileToMicroCode(*analysis.program, shader.instructions.data(), shader.instructions.size(), shader.swizzle_data.data(), shader.swizzle_data.size(), shader.bool_uniforms, {});

    std::string body = "[";

    auto format_instruction = [&](const auto& instr_and_offset) {
        auto offset = instr_and_offset.first;
        auto pica_offset_it =
            ranges::find_if(micro_code.block_offsets,
                            [&](auto& v) { return MicroOpOffset { static_cast<uint32_t>(offset) } == v.second; });
        std::string pica_offset_str;
        bool is_entrypoint = false;
        if (pica_offset_it != micro_code.block_offsets.end()) {
            pica_offset_str = fmt::format(  R"(, "pica_offset": "{:#x}")",
                                            4 * pica_offset_it->first.value);
            is_entrypoint = (pica_offset_it->first.value == shader.entry_point);
        }
        return fmt::format( R"({{ "id": {}, "code": "{}{}" {}}})",
                            offset,
                            Pica::VertexShader::DisassembleMicroOp(instr_and_offset.second),
                            is_entrypoint ? " (ENTRY)" : "",
                            pica_offset_str);
    };
    namespace views = ranges::views;
    body += views::enumerate(micro_code.ops) | views::transform(format_instruction) | views::cache1 | views::join(std::string_view(", ")) | ranges::to<std::string>;
    body += ']';

    response.send(Http::Code::Ok, body);
}

static void DoGetGLSL(GPUService& service, const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
    std::lock_guard guard(service.access_mutex);

    auto key = std::stoull(request.param(":key").as<std::string>(), 0, 16);

    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");
    response.headers().add<Http::Header::ContentType>(Http::Mime::MediaType { Http::Mime::Type::Application, Http::Mime::Subtype::Json });

    response.send(Http::Code::Ok, "[]");
}

static void DoGetShbin(GPUService& service, const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
    std::lock_guard guard(service.access_mutex);

    auto key = std::stoull(request.param(":key").as<std::string>(), 0, 16);
    const auto& shader = service.shaders.at(key);

    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");
    response.headers().add<Http::Header::ContentType>(Http::Mime::MediaType { Http::Mime::Type::Application, Http::Mime::Subtype::OctetStream });

    auto shbin = Pica::DebugUtils::DumpShader(
            shader.instructions.data(), shader.instructions.size(),
            shader.swizzle_data.data(), shader.swizzle_data.size(),
            shader.entry_point, nullptr);

    std::string raw { shbin.begin(), shbin.end() };
    response.send(Http::Code::Ok, raw);
}

#endif

void GPUService::RegisterRoutes(Pistache::Rest::Router& router) {
#if ENABLE_PISTACHE == 1
    using namespace Rest;

    Routes::Get(router, "/gpu/shaders",
                [this](const auto& request, auto response) { DoListShaders(*this, request, std::move(response)); return Route::Result::Ok; });
    Routes::Get(router, "/gpu/shader/:key/pica",
                [this](const auto& request, auto response) { DoPrintPicaDisassembly(*this, request, std::move(response)); return Route::Result::Ok; });
    Routes::Get(router, "/gpu/shader/:key/cfg",
                [this](const auto& request, auto response) { DoGetControlFlowGraph(*this, request, std::move(response)); return Route::Result::Ok; });
    Routes::Get(router, "/gpu/shader/:key/bytecode",
                [this](const auto& request, auto response) { DoGetBytecode(*this, request, std::move(response)); return Route::Result::Ok; });
    Routes::Get(router, "/gpu/shader/:key/glsl",
                [this](const auto& request, auto response) { DoGetGLSL(*this, request, std::move(response)); return Route::Result::Ok; });
    Routes::Get(router, "/gpu/shader/:key/shbin",
                [this](const auto& request, auto response) { DoGetShbin(*this, request, std::move(response)); return Route::Result::Ok; });

    // TODO: Move those to DebugServer itself
    Routes::Options(router, "/*/*", Routes::bind(DoOptions));
    Routes::Options(router, "/*/*/*/*", Routes::bind(DoOptions));
#endif
}

template<>
std::unique_ptr<Service> CreateService<GPUService>() {
    return std::make_unique<GPUService>();
}

} // namespace Debugger
