#pragma once

#include <initializer_list>
#include <memory>

#include "../support/common/common_types.h"

#include <platform/gpu/pica.hpp>

#include <framework/math_vec.hpp>

namespace Settings {
enum class ShaderEngine;
}

namespace Pica {

struct Context;

namespace VertexShader {

struct ShaderUniforms {
    std::array<Math::Vec4<float24>, 96> f;

    std::array<bool,16> b;

    std::array<Math::Vec4<u8>,4> i;
};

struct InputVertex {
//    Math::Vec4<float24> attr[16];
    std::array<Math::Vec4<float24>, 16> attr;
};
static_assert(std::is_pod<InputVertex>::value, "Structure is not POD");

struct OutputVertex {
    OutputVertex() = default;

    // VS output attributes
    Math::Vec4<float24> pos;
    Math::Vec4<float24> dummy; // quaternions (not implemented, yet)
    Math::Vec4<float24> color;
    Math::Vec2<float24> tc0;
    Math::Vec2<float24> tc1;
    float24 pad[6];
    Math::Vec2<float24> tc2;

    // Padding for optimal alignment
    float24 pad2[4];

    // Attributes used to store intermediate results

    // position after perspective divide
    Math::Vec3<float24> screenpos;
    float24 pad3;

    // Linear interpolation
    // factor: 0=this, 1=vtx
    void Lerp(float24 factor, const OutputVertex& vtx) {
        pos = pos * factor + vtx.pos * (float24::FromFloat32(1) - factor);

        // TODO: Should perform perspective correct interpolation here...
        tc0 = tc0 * factor + vtx.tc0 * (float24::FromFloat32(1) - factor);
        tc1 = tc1 * factor + vtx.tc1 * (float24::FromFloat32(1) - factor);
        tc2 = tc2 * factor + vtx.tc2 * (float24::FromFloat32(1) - factor);

        screenpos = screenpos * factor + vtx.screenpos * (float24::FromFloat32(1) - factor);

        color = color * factor + vtx.color * (float24::FromFloat32(1) - factor);
    }

    // Linear interpolation
    // factor: 0=v0, 1=v1
    static OutputVertex Lerp(float24 factor, const OutputVertex& v0, const OutputVertex& v1) {
        OutputVertex ret = v0;
        ret.Lerp(factor, v1);
        return ret;
    }
};
static_assert(std::is_pod<OutputVertex>::value, "Structure is not POD");
static_assert(sizeof(OutputVertex) == 32 * sizeof(float), "OutputVertex has invalid size");

struct ShaderEngine {
    virtual ~ShaderEngine() = default;

    /**
     * Reconfigure the ShaderEngine for a changed set of external factors
     * NOTE: InputVertex must outlive any calls to RunShader before the next call to Reset()
     * TODO: Revisit the InputVertex lifetime issue
     */
    virtual void Reset(const Regs::VSInputRegisterMap&, InputVertex&, uint32_t num_attributes) = 0;

    virtual void UpdateUniforms(const std::array<Math::Vec4<float24>, 96>&) = 0;

    virtual OutputVertex Run(Context&, uint32_t entry_point) = 0;

    bool ProcessesInputVertexesOnGPU() const {
        return processes_input_vertexes_on_gpu;
    }

protected:
    ShaderEngine(bool processes_input_vertexes_on_gpu_)
        : processes_input_vertexes_on_gpu(processes_input_vertexes_on_gpu_) {

    }

    bool processes_input_vertexes_on_gpu;
};

struct EnginePoolImpl;
struct EnginePool {
    EnginePool();
    ~EnginePool();

    // Should be called when any of the parameters changed compared to the
    // previous call. This is used for internal optimization (to avoid
    // recomputing the internal hash table key)
    void SetDirty();

    ShaderEngine& GetOrCompile(Context& context, Settings::ShaderEngine type, uint32_t entry_point);

    std::unique_ptr<EnginePoolImpl> impl;
    bool dirty = true;
};

std::unique_ptr<ShaderEngine> CreateInterpreter();
std::unique_ptr<ShaderEngine> CreateMicroCodeRecompiler(Context& context);

std::string DisassembleShader(uint32_t instruction, const uint32_t* swizzle_data);

struct AnalyzedProgram;
struct AnalyzedProgramPimpl {
    AnalyzedProgramPimpl();
    explicit AnalyzedProgramPimpl(AnalyzedProgram&&);
    explicit AnalyzedProgramPimpl(AnalyzedProgramPimpl&&);
    AnalyzedProgramPimpl& operator =(AnalyzedProgramPimpl&&);
    ~AnalyzedProgramPimpl();

    std::unique_ptr<AnalyzedProgram> program;
};

AnalyzedProgramPimpl AnalyzeShader( const uint32_t* shader_instructions,
                                    uint32_t num_instructions,
                                    uint16_t bool_uniforms,
                                    uint32_t entry_point);

std::string VisualizeShaderControlFlowGraph(
        const AnalyzedProgram& program,
        const uint32_t* shader_instructions, uint32_t num_instructions,
        const uint32_t* swizzle_data, uint32_t num_swizzle_slots);

} // namespace

} // namespace
