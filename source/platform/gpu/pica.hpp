#pragma once

#include "float.hpp"

#include <array>
#include <cstddef>
#include <cmath>
#include <initializer_list>
#include <map>
#include <vector>

#include <framework/bit_field_new.hpp>
#include <framework/image_format.hpp>
#include <framework/meta_tools.hpp>
#include <video_core/src/support/common/bit_field.h>

namespace Pica {

// helper macro to properly align structure members.
// Calling INSERT_PADDING_WORDS will add a new member variable with a name like "pad121",
// depending on the current source line to make sure variable names are unique.
#define INSERT_PADDING_WORDS_HELPER1(x, y) x ## y
#define INSERT_PADDING_WORDS_HELPER2(x, y) INSERT_PADDING_WORDS_HELPER1(x, y)
#define INSERT_PADDING_WORDS(num_words) uint32_t INSERT_PADDING_WORDS_HELPER2(pad, __LINE__)[(num_words)]

enum class CullMode : uint32_t {
    // Select which polygons are considered to be "frontfacing".
    KeepAll              = 0,
    KeepClockWise        = 1,
    KeepCounterClockWise = 2,
    // TODO: What does the third value imply?
};

struct AlphaTest {
    enum class Function : uint32_t {
        Always             = 1,
        NotEqual           = 3,
        GreaterThan        = 6,
        GreaterThanOrEqual = 7,
    };

    uint32_t storage;

    auto enable() const { return BitField::v3::MakeFlagOn<0>(this); }
    auto function() const { return BitField::v3::MakeFieldOn<4, 3, Function>(this); }
    auto reference() const { return BitField::v3::MakeFieldOn<8, 8>(this); }
};

struct StencilTest {
    uint32_t raw1;
    uint32_t raw2;

    enum class CompareFunc {
        Never              = 0,
        Always             = 1,
        Equal              = 2,
        NotEqual           = 3,
        LessThan           = 4,
        LessThanOrEqual    = 5,
        GreaterThan        = 6,
        GreaterThanOrEqual = 7
    };

    enum class Op {
        Keep               = 0,
        Zero               = 1,
        Replace            = 2,
        IncrementAndClamp  = 3,
        DecrementAndClamp  = 4,
        Invert             = 5,
        IncrementAndWrap   = 6,
        DecrementAndWrap   = 7
    };

    auto enabled() const { return BitField::v3::MakeFlagOn<&StencilTest::raw1, 0>(this); }
    auto compare_function() const { return BitField::v3::MakeFieldOn<&StencilTest::raw1, 4, 3, CompareFunc>(this); }
    auto reference() const { return BitField::v3::MakeFieldOn<&StencilTest::raw1, 16, 8>(this); }

    // Masks for compare stencil op outputs and stencil function inputs, respectively
    auto mask_out() const { return BitField::v3::MakeFieldOn<&StencilTest::raw1, 8, 8>(this); }
    auto mask_in() const { return BitField::v3::MakeFieldOn<&StencilTest::raw1, 24, 8>(this); }

    auto op_fail_stencil() const { return BitField::v3::MakeFieldOn<&StencilTest::raw2, 0, 3, Op>(this); }
    auto op_pass_stencil_fail_depth() const { return BitField::v3::MakeFieldOn<&StencilTest::raw2, 4, 3, Op>(this); }
    auto op_pass_both() const { return BitField::v3::MakeFieldOn<&StencilTest::raw2, 8, 3, Op>(this); }
};

enum class DepthFunc : uint32_t {
    Never              = 0,
    Always             = 1,
    LessThan           = 4,
    LessThanOrEqual    = 5,
    GreaterThan        = 6,
    GreaterThanOrEqual = 7,
};

enum class AlphaBlendEquation : uint32_t {
    Add = 0,

    ReverseSubtract = 2,
};

enum class AlphaBlendFactor : uint32_t {
    Zero = 0,
    One = 1,
    SourceColor = 2,

    DestinationColor = 4,

    SourceAlpha = 6,
    OneMinusSourceAlpha = 7,
    DestinationAlpha = 8,
    OneMinusDestinationAlpha = 9,
    ConstantColor = 0xa,
    OneMinusConstantColor = 0xb,
    ConstantAlpha = 0xc,
    OneMinusConstantAlpha = 0xd,
    SourceAlphaSaturate = 0xe,
};

enum class LogicOp : uint32_t {
    Copy = 3,
    Set = 4,
    Noop = 6,
};

enum class TexFilter : uint32_t {
    Nearest = 0,
    Linear = 1
};

enum class TexWrapMode : uint32_t {
    ClampToEdge    = 0,
    ClampToBorder  = 1,
    Repeat         = 2,
    MirroredRepeat = 3,
};

enum class LightLutIndex : uint32_t {
    D0 = 0,
    D1 = 1,

    FR = 3, // Fresnel
    RB = 4, // Reflect Blue
    RG = 5, // Reflect Green
    RR = 6, // Reflect Red

    // Spot light
    SP0 = 8,
    // 9-15: SP1-SP7

    // Distance attenuation
    DA0 = 16,
    // 17-23: DA1-DA7
};

enum class LightLutInput : uint32_t {
    NH = 0,
    VH = 1,
    NV = 2,
    LN = 3,
    SP = 4, // Spot light
    CP = 5, // (cos φ)
};

enum class LightLutScale : uint32_t {
    x1    = 0,
    x2    = 1,
    x4    = 2,
    x8    = 3,

    x0_25 = 6,
    x0_5  = 7,
};

struct FramebufferColorFormat {
    uint32_t raw;

    static constexpr std::array<GenericImageFormat, 5> format_map = {{
        GenericImageFormat::RGBA8,
        GenericImageFormat::RGB8,
        GenericImageFormat::RGBA5551,
        GenericImageFormat::RGB565,
        GenericImageFormat::RGBA4
    }};
};

struct FramebufferDepthStencilFormat {
    uint32_t raw;

    static constexpr std::array<GenericImageFormat, 4> format_map = {{
        GenericImageFormat::D16,
        GenericImageFormat::Unknown,
        GenericImageFormat::D24,
        GenericImageFormat::D24S8
    }};
};

struct LightingColor {
    uint32_t storage;

    constexpr auto blue() const { return BitField::v3::MakeFieldOn<0, 10>(this); }
    constexpr auto green() const { return BitField::v3::MakeFieldOn<10, 10>(this); }
    constexpr auto red() const { return BitField::v3::MakeFieldOn<20, 10>(this); }
};

struct TextureConfig {
    struct {
        uint32_t storage;

        auto r() const { return BitField::v3::MakeFieldOn< 0, 8>(this); }
        auto g() const { return BitField::v3::MakeFieldOn< 8, 8>(this); }
        auto b() const { return BitField::v3::MakeFieldOn<16, 8>(this); }
        auto a() const { return BitField::v3::MakeFieldOn<24, 8>(this); }
    } border_color;

    union {
        BitFieldLegacy< 0, 16, uint32_t> height;
        BitFieldLegacy<16, 16, uint32_t> width;
    };

    union {
        BitFieldLegacy< 1, 1, TexFilter> mag_filter;
        BitFieldLegacy< 2, 1, TexFilter> min_filter;

        BitFieldLegacy< 8, 2, TexWrapMode> wrap_t;
        BitFieldLegacy<12, 2, TexWrapMode> wrap_s;

        BitFieldLegacy<24, 1, TexFilter> mip_filter;
    };

    INSERT_PADDING_WORDS(0x1);

    uint32_t address_raw;

    uint32_t GetPhysicalAddress() const {
        return address_raw * 8;
    }

    // texture1 and texture2 store the texture format directly after the address
    // whereas texture0 inserts some additional flags inbetween.
    // Hence, we store the format separately so that all other parameters can be described
    // in a single structure.
};

struct TextureFormat {
    uint32_t raw;

    static constexpr std::array<GenericImageFormat, 14> format_map = {{
        GenericImageFormat::RGBA8,
        GenericImageFormat::RGB8,
        GenericImageFormat::RGBA5551,
        GenericImageFormat::RGB565,
        GenericImageFormat::RGBA4,
        GenericImageFormat::IA8,
        GenericImageFormat::RG8,
        GenericImageFormat::I8,
        GenericImageFormat::A8,
        GenericImageFormat::IA4,
        GenericImageFormat::I4,
        GenericImageFormat::A4,
        GenericImageFormat::ETC1,
        GenericImageFormat::ETC1A4,
    }};
};

struct FullTextureConfig {
    bool enabled;
    TextureConfig config;
    TextureFormat format;
};

// Returns index corresponding to the Regs member labeled by field_name
// TODO: Due to Visual studio bug 209229, offsetof does not return constant expressions
//       when used with array elements (e.g. PICA_REG_INDEX(vs_uniform_setup.set_value[1])).
//       For details cf. https://connect.microsoft.com/VisualStudio/feedback/details/209229/offsetof-does-not-produce-a-constant-expression-for-array-members
//       Hopefully, this will be fixed sometime in the future.
//       For lack of better alternatives, we currently hardcode the offsets when constant
//       expressions are needed via PICA_REG_INDEX_WORKAROUND (on sane compilers, static_asserts
//       will then make sure the offsets indeed match the automatically calculated ones).
#define PICA_REG_INDEX(field_name) (offsetof(Pica::Regs, field_name) / sizeof(uint32_t))
#if defined(_MSC_VER)
#define PICA_REG_INDEX_WORKAROUND(field_name, backup_workaround_index) (backup_workaround_index)
#else
// NOTE: Yeah, hacking in a static_assert here just to workaround the lacking MSVC compiler
//       really is this annoying. This macro just forwards its first argument to PICA_REG_INDEX
//       and then performs a (no-op) cast to size_t iff the second argument matches the expected
//       field offset. Otherwise, the compiler will fail to compile this code.
#define PICA_REG_INDEX_WORKAROUND(field_name, backup_workaround_index) \
    ((typename std::enable_if<backup_workaround_index == PICA_REG_INDEX(field_name), size_t>::type)PICA_REG_INDEX(field_name))
#endif // _MSC_VER

struct Regs {
    INSERT_PADDING_WORDS(0x10);

    uint32_t trigger_irq;

    INSERT_PADDING_WORDS(0x2f);

    union {
        BitFieldLegacy<0, 2, CullMode> cull_mode;
    };

    // NOTE: This is actually half the viewport width
    BitFieldLegacy<0, 24, uint32_t> viewport_size_x;

    INSERT_PADDING_WORDS(0x1);

    // NOTE: This is actually half the viewport height
    BitFieldLegacy<0, 24, uint32_t> viewport_size_y;

    INSERT_PADDING_WORDS(0x9);

    BitFieldLegacy<0, 24, uint32_t> viewport_depth_range; // float24
    BitFieldLegacy<0, 24, uint32_t> viewport_depth_far_plane; // float24

    /**
     * Total number of attributes output by the final shader stage and sent to
     * the post-shader-pipeline.
     *
     * Note that this is unrelated to the set of output registers accessible
     * by shaders, which is indicated by gs/vs_output_register_mask instead.
     * In particular, the shader output registers may have indexes higher
     * than the number of shader output attributes: Output attributes are
     * tightly packed, whereas there may be "gaps" of unused output registers.
     */
    BitFieldLegacy<0, 3, uint32_t> shader_num_output_attributes;

    union VSOutputAttributes {
        // Maps components of output vertex attributes to semantics
        enum Semantic : uint32_t
        {
            POSITION_X   =  0,
            POSITION_Y   =  1,
            POSITION_Z   =  2,
            POSITION_W   =  3,

            // This quaternion encodes a rotation from the orthonormal triad into the (normal, tangent, bitangent) coordinate system
            QUATERNION_X =  4,
            QUATERNION_Y =  5,
            QUATERNION_Z =  6,
            QUATERNION_W =  7,

            COLOR_R      =  8,
            COLOR_G      =  9,
            COLOR_B      = 10,
            COLOR_A      = 11,

            TEXCOORD0_U  = 12,
            TEXCOORD0_V  = 13,
            TEXCOORD1_U  = 14,
            TEXCOORD1_V  = 15,
            TEXCOORD0_W  = 16, // For projection texture (texture unit 0, only)
            TEXCOORD2_U  = 22,
            TEXCOORD2_V  = 23,

            VIEW_X       = 18,
            VIEW_Y       = 19,
            VIEW_Z       = 20,

            INVALID      = 31,
        };

        BitFieldLegacy< 0, 5, Semantic> map_x;
        BitFieldLegacy< 8, 5, Semantic> map_y;
        BitFieldLegacy<16, 5, Semantic> map_z;
        BitFieldLegacy<24, 5, Semantic> map_w;
    };

    std::array<VSOutputAttributes, 7> shader_output_semantics;

    INSERT_PADDING_WORDS(0xf);

    uint32_t scissor_pos;
    uint32_t scissor_size;

    union {
        uint32_t raw;

        BitFieldLegacy< 0, 10, int32_t> x;
        BitFieldLegacy<16, 10, int32_t> y;
    } viewport_corner;

    INSERT_PADDING_WORDS(0x17);

    union {
        BitFieldLegacy< 0, 1, uint32_t> texture0_enable;
        BitFieldLegacy< 1, 1, uint32_t> texture1_enable;
        BitFieldLegacy< 2, 1, uint32_t> texture2_enable;
    };
    TextureConfig texture0;
    INSERT_PADDING_WORDS(0x8);
    struct TextureFormatRegister {
        uint32_t storage;

        auto value() const { return BitField::v3::MakeFieldOn<0, 4, TextureFormat>(this); }

        operator TextureFormat() const {
            return value()();
        }
    };

    TextureFormatRegister texture0_format;
    INSERT_PADDING_WORDS(0x2);
    TextureConfig texture1;
    TextureFormatRegister texture1_format;
    INSERT_PADDING_WORDS(0x2);
    TextureConfig texture2;
    TextureFormatRegister texture2_format;
    INSERT_PADDING_WORDS(0x21);

    const std::array<FullTextureConfig, 3> GetTextures() const {
        return {{
                   { texture0_enable.ToBool(), texture0, texture0_format },
                   { texture1_enable.ToBool(), texture1, texture1_format },
                   { texture2_enable.ToBool(), texture2, texture2_format }
               }};
    }

    // 0xc0-0xff: Texture Combiner (akin to glTexEnv)
    struct TevStageConfig {
        enum class Source : uint32_t {
            PrimaryColor           = 0x0,
            PrimaryFragmentColor   = 0x1,
            SecondaryFragmentColor = 0x2,
            Texture0               = 0x3,
            Texture1               = 0x4,
            Texture2               = 0x5,
            Texture3               = 0x6,
            // 0x7-0xc = primary color??
            CombinerBuffer         = 0xd,
            Constant               = 0xe,
            Previous               = 0xf,
        };

        enum class ColorModifier : uint32_t {
            SourceColor         = 0,
            OneMinusSourceColor = 1,
            SourceAlpha         = 2,
            OneMinusSourceAlpha = 3,

            // Non-standard extensions:
            SourceRed           = 4,
            OneMinusSourceRed   = 5,

            SourceGreen         = 8,
            OneMinusSourceGreen = 9,

            SourceBlue          = 12,
            OneMinusSourceBlue  = 13,
        };

        enum class AlphaModifier : uint32_t {
            SourceAlpha         = 0,
            OneMinusSourceAlpha = 1,

            // Non-standard extensions:
            SourceRed           = 2,
            OneMinusSourceRed   = 3,
            SourceGreen         = 4,
            OneMinusSourceGreen = 5,
            SourceBlue          = 6,
            OneMinusSourceBlue  = 7,
        };

        enum class Operation : uint32_t {
            Replace         = 0,
            Modulate        = 1,
            Add             = 2,
            AddSigned       = 3,
            Lerp            = 4,
            Subtract        = 5,
            Dot3RGB         = 6, // Dot product of first and second input, each offset by the median value (0.5/127.5)
            Dot3RGBA        = 7, // Like Dot3, but the scalar result is written to all components rather than just the RGB ones
            MultiplyThenAdd = 8,
            AddThenMultiply = 9,
        };

        uint32_t storage[5];

        auto color_source1() const { return BitField::v3::MakeFieldOn<0, 4, Source>(&storage[0]); }
        auto color_source2() const { return BitField::v3::MakeFieldOn<4, 4, Source>(&storage[0]); }
        auto color_source3() const { return BitField::v3::MakeFieldOn<8, 4, Source>(&storage[0]); }
        auto alpha_source1() const { return BitField::v3::MakeFieldOn<16, 4, Source>(&storage[0]); }
        auto alpha_source2() const { return BitField::v3::MakeFieldOn<20, 4, Source>(&storage[0]); }
        auto alpha_source3() const { return BitField::v3::MakeFieldOn<24, 4, Source>(&storage[0]); }

        auto color_modifier1() const { return BitField::v3::MakeFieldOn<0, 4, ColorModifier>(&storage[1]); }
        auto color_modifier2() const { return BitField::v3::MakeFieldOn<4, 4, ColorModifier>(&storage[1]); }
        auto color_modifier3() const { return BitField::v3::MakeFieldOn<8, 4, ColorModifier>(&storage[1]); }
        auto alpha_modifier1() const { return BitField::v3::MakeFieldOn<12, 3, AlphaModifier>(&storage[1]); }
        auto alpha_modifier2() const { return BitField::v3::MakeFieldOn<16, 3, AlphaModifier>(&storage[1]); }
        auto alpha_modifier3() const { return BitField::v3::MakeFieldOn<20, 3, AlphaModifier>(&storage[1]); }

        auto color_op() const { return BitField::v3::MakeFieldOn<0, 4, Operation>(&storage[2]); }
        auto alpha_op() const { return BitField::v3::MakeFieldOn<16, 4, Operation>(&storage[2]); }

        auto const_r() const { return BitField::v3::MakeFieldOn<0, 8, uint8_t>(&storage[3]); }
        auto const_g() const { return BitField::v3::MakeFieldOn<8, 8, uint8_t>(&storage[3]); }
        auto const_b() const { return BitField::v3::MakeFieldOn<16, 8, uint8_t>(&storage[3]); }
        auto const_a() const { return BitField::v3::MakeFieldOn<24, 8, uint8_t>(&storage[3]); }

        // Access these through the convenience getters below
        auto multiplier_exp_rgb() const { return BitField::v3::MakeFieldOn<0, 2>(&storage[4]); }
        auto multiplier_exp_a() const { return BitField::v3::MakeFieldOn<16, 2>(&storage[4]); }

        uint32_t GetMultiplierRGB() const {
            if (multiplier_exp_rgb() == 3) {
                throw std::runtime_error("Invalid RGB scaling exponent 3");
            }
            return (1 << multiplier_exp_rgb());
        }

        uint32_t GetMultiplierA() const {
            if (multiplier_exp_a() == 3) {
                throw std::runtime_error("Invalid alpha scaling exponent 3");
            }
            return (1 << multiplier_exp_a());
        }
    };

    TevStageConfig tev_stage0;
    INSERT_PADDING_WORDS(0x3);
    TevStageConfig tev_stage1;
    INSERT_PADDING_WORDS(0x3);
    TevStageConfig tev_stage2;
    INSERT_PADDING_WORDS(0x3);
    TevStageConfig tev_stage3;
    INSERT_PADDING_WORDS(0x3);

    struct {
        uint32_t storage;

        auto update_mask_rgb() const { return BitField::v3::MakeFieldOn<8, 4>(this); }
        auto update_mask_a() const { return BitField::v3::MakeFieldOn<12, 4>(this); }

        bool TevStageUpdatesRGB(std::size_t index) const {
            return 0 != ((update_mask_rgb() >> index) & 1);
        }

        bool TevStageUpdatesA(std::size_t index) const {
            return 0 != ((update_mask_a() >> index) & 1);
        }
    } combiner_buffer;

    INSERT_PADDING_WORDS(0xf);
    TevStageConfig tev_stage4;
    INSERT_PADDING_WORDS(0x3);
    TevStageConfig tev_stage5;

    // Value used to initialize the tev combiner buffer
    struct {
        uint32_t storage;

        auto r() const { return BitField::v3::MakeFieldOn< 0, 8>(this); }
        auto g() const { return BitField::v3::MakeFieldOn< 8, 8>(this); }
        auto b() const { return BitField::v3::MakeFieldOn<16, 8>(this); }
        auto a() const { return BitField::v3::MakeFieldOn<24, 8>(this); }
    } combiner_buffer_init;

    INSERT_PADDING_WORDS(0x2);

    const std::array<Regs::TevStageConfig,6> GetTevStages() const {
        return { tev_stage0, tev_stage1,
                 tev_stage2, tev_stage3,
                 tev_stage4, tev_stage5 };
    }

    struct OutputMerger {
        union {
            // If false, logic blending is used
            BitFieldLegacy<8, 1, uint32_t> alphablend_enable;
        };

        union {
            uint32_t raw;

            BitFieldLegacy< 0, 8, AlphaBlendEquation> blend_equation_rgb;
            BitFieldLegacy< 8, 8, AlphaBlendEquation> blend_equation_a;

            BitFieldLegacy<16, 4, AlphaBlendFactor> factor_source_rgb;
            BitFieldLegacy<20, 4, AlphaBlendFactor> factor_dest_rgb;

            BitFieldLegacy<24, 4, AlphaBlendFactor> factor_source_a;
            BitFieldLegacy<28, 4, AlphaBlendFactor> factor_dest_a;
        } alpha_blending;

        union {
            BitFieldLegacy<0, 4, LogicOp> op;
        } logic_op;

        struct {
            uint32_t storage;

            auto r() const { return BitField::v3::MakeFieldOn< 0, 8>(this); }
            auto g() const { return BitField::v3::MakeFieldOn< 8, 8>(this); }
            auto b() const { return BitField::v3::MakeFieldOn<16, 8>(this); }
            auto a() const { return BitField::v3::MakeFieldOn<24, 8>(this); }
        } blend_constant;

        AlphaTest alpha_test;

        StencilTest stencil_test;

        union {
            BitFieldLegacy< 0, 1, uint32_t> depth_test_enable;
            BitFieldLegacy< 4, 3, DepthFunc> depth_test_func;
            BitFieldLegacy< 8, 1, uint32_t> color_write_enable_r;
            BitFieldLegacy< 9, 1, uint32_t> color_write_enable_g;
            BitFieldLegacy<10, 1, uint32_t> color_write_enable_b;
            BitFieldLegacy<11, 1, uint32_t> color_write_enable_a;
            BitFieldLegacy<12, 1, uint32_t> depth_write_enable;
        };

        INSERT_PADDING_WORDS(0x8);
    } output_merger;

    struct {
        INSERT_PADDING_WORDS(0x2);

        uint32_t raw_access_flags_color[2];
        uint32_t raw_access_flags_depth_stencil[2];

        // These fields look like bitmasks, but they control access to all components at once.
        // Disabling them (i.e. setting them to 0) takes priority over the output_merger flags.
        auto color_read_enabled() const { return BitField::v3::MakeFieldOn<0, 4>(&raw_access_flags_color[0]); }
        auto color_write_enabled() const { return BitField::v3::MakeFieldOn<0, 4>(&raw_access_flags_color[1]); }
        auto depth_stencil_read_enabled() const { return BitField::v3::MakeFieldOn<0, 2>(&raw_access_flags_depth_stencil[0]); }
        auto depth_stencil_write_enabled() const { return BitField::v3::MakeFieldOn<0, 2>(&raw_access_flags_depth_stencil[1]); }

        uint32_t depth_format;
        BitFieldLegacy<16, 3, uint32_t> color_format;

        FramebufferColorFormat GetColorFormat() const {
            return { color_format.Value() };
        }

        FramebufferDepthStencilFormat GetDepthStencilFormat() const {
            return { depth_format };
        }

        bool HasStencilBuffer() const {
            return (FramebufferDepthStencilFormat::format_map[depth_format] == GenericImageFormat::D24S8);
        }

        INSERT_PADDING_WORDS(0x4);

        uint32_t depth_buffer_address;
        uint32_t color_buffer_address;

        union {
            // Apparently, the framebuffer width is stored as expected,
            // while the height is stored as the actual height minus one.
            // Hence, don't access these fields directly but use the accessors
            // GetWidth() and GetHeight() instead.
            BitFieldLegacy< 0, 11, uint32_t> width;
            BitFieldLegacy<12, 10, uint32_t> height; // TODO: Does this indeed only have 10 bits?
        };

        INSERT_PADDING_WORDS(0x1);

        inline uint32_t GetColorBufferPhysicalAddress() const {
            return DecodeAddressRegister(color_buffer_address);
        }
        inline uint32_t GetDepthBufferPhysicalAddress() const {
            return DecodeAddressRegister(depth_buffer_address);
        }

        inline uint32_t GetWidth() const {
            return width;
        }

        inline uint32_t GetHeight() const {
            return height + 1;
        }
    } framebuffer;

    INSERT_PADDING_WORDS(0x20);

    struct {
        struct {
            // The 3DS fragment lighting pipeline doesn't distinguish between
            // materials and lights, so these properties are premultiplied
            std::array<LightingColor, 2> specular;
            LightingColor diffuse;
            LightingColor ambient;

            // NOTE: If is_directional() is false, these actually specify the position
            std::array<uint32_t, 2> raw_light_dir;
            constexpr auto raw_light_dir_x() const { return BitField::v3::MakeFieldOn< 0, 16>(&raw_light_dir[0]); }
            constexpr auto raw_light_dir_y() const { return BitField::v3::MakeFieldOn<16, 16>(&raw_light_dir[0]); }
            constexpr auto raw_light_dir_z() const { return BitField::v3::MakeFieldOn< 0, 16>(&raw_light_dir[1]); }
            float16 get_light_dir_x() const { return float16::FromRawFloat(raw_light_dir_x()); }
            float16 get_light_dir_y() const { return float16::FromRawFloat(raw_light_dir_y()); }
            float16 get_light_dir_z() const { return float16::FromRawFloat(raw_light_dir_z()); }

            // 13-bit signed fixed-point coordinates with 11 bits of precision
            std::array<uint32_t, 2> raw_spot_dir;
            constexpr auto spot_dir_x() const { return BitField::v3::MakeFieldOn< 0, 13, int32_t>(&raw_spot_dir[0]); }
            constexpr auto spot_dir_y() const { return BitField::v3::MakeFieldOn<16, 13, int32_t>(&raw_spot_dir[0]); }
            constexpr auto spot_dir_z() const { return BitField::v3::MakeFieldOn< 0, 13, int32_t>(&raw_spot_dir[1]); }


            INSERT_PADDING_WORDS(0x1);

            struct {
                uint32_t storage;

                constexpr auto is_directional() const { return BitField::v3::MakeFlagOn<0>(this); }

                // If true, the absolute value is used for dot products. This
                // unconditionally applies to the computation for diffuse
                // lighting, but it affects LUT indexing only if unsigned
                // indexes are used.
                constexpr auto abs_dot_products() const { return BitField::v3::MakeFlagOn<1>(this); }

                constexpr auto specular0_use_geometric_factor() const { return BitField::v3::MakeFlagOn<2>(this); }
                constexpr auto specular1_use_geometric_factor() const { return BitField::v3::MakeFlagOn<3>(this); }
            } config;
            // INSERT_PADDING_WORDS(0x1); // TODO: This ("two sided diffuse") takes the absolute of the dot product before indexing into the LUT. Not sure if it applies to all LUTs though
            INSERT_PADDING_WORDS(0x6);
        } lights[8]; // indexing must take light permutation into account

        LightingColor global_ambient;

        INSERT_PADDING_WORDS(0x1);

        struct {
            uint32_t storage;
            constexpr auto value() const { return BitField::v3::MakeFieldOn<0, 3>(this); }
        } max_light_id;

        #pragma pack(4)
        struct {
            uint64_t storage;

            constexpr auto fresnel_to_primary_alpha() const { return BitField::v3::MakeFlagOn<2>(this); }
            constexpr auto fresnel_to_secondary_alpha() const { return BitField::v3::MakeFlagOn<3>(this); }

            constexpr auto lut_config() const { return BitField::v3::MakeFieldOn<4, 4>(this); }

            // If true, set the secondary fragment color to zero if the LN dot product is zero
            constexpr auto clamp_highlights() const { return BitField::v3::MakeFlagOn<27>(this); }

            // TODO: Do these control the LUTs themselves or the use of the corresponding terms in lighting calculations?
            constexpr auto spot_disabled() const { return BitField::v3::MakeFieldOn<40, 8>(this); }
            constexpr auto d0_disabled() const { return BitField::v3::MakeFlagOn<48>(this); }
            constexpr auto d1_disabled() const { return BitField::v3::MakeFlagOn<49>(this); }
            // Bit 50 doesn't seem to be used (always set 1)
            constexpr auto fr_disabled() const { return BitField::v3::MakeFlagOn<51>(this); }
            constexpr auto rb_disabled() const { return BitField::v3::MakeFlagOn<52>(this); }
            constexpr auto rg_disabled() const { return BitField::v3::MakeFlagOn<53>(this); }
            constexpr auto rr_disabled() const { return BitField::v3::MakeFlagOn<54>(this); }
            constexpr auto dist_disabled() const { return BitField::v3::MakeFieldOn<56, 8>(this); }

            uint32_t GetEnabledLUTMask() const {
                uint32_t mask = 0;
                mask |= spot_disabled()() << Meta::to_underlying(LightLutIndex::SP0);
                mask |= dist_disabled()() << Meta::to_underlying(LightLutIndex::DA0);

                mask |= d0_disabled() << Meta::to_underlying(LightLutIndex::D0);
                mask |= d1_disabled() << Meta::to_underlying(LightLutIndex::D1);
                // NOTE: LUT index 2 not used
                mask |= fr_disabled() << Meta::to_underlying(LightLutIndex::FR);
                mask |= rb_disabled() << Meta::to_underlying(LightLutIndex::RB);
                mask |= rg_disabled() << Meta::to_underlying(LightLutIndex::RG);
                mask |= rr_disabled() << Meta::to_underlying(LightLutIndex::RR);
                // NOTE: LUT index 7 not used

                return (~mask) & (0xffffff ^ 0b1000'0100);
            }

            bool LUTConfigConsistent() const {
                // TODO: Check that lut_config is consistent with the disablement bits
                const uint32_t enabled_masks[32] = {
                    (1 << Meta::to_underlying(LightLutIndex::D0)) | (1 << Meta::to_underlying(LightLutIndex::RR)) | (0xff << Meta::to_underlying(LightLutIndex::SP0)) | (0xff << Meta::to_underlying(LightLutIndex::D0)),
                    (1 << Meta::to_underlying(LightLutIndex::FR)) | (1 << Meta::to_underlying(LightLutIndex::RR)) | (0xff << Meta::to_underlying(LightLutIndex::SP0)) | (0xff << Meta::to_underlying(LightLutIndex::D0)),
                    (1 << Meta::to_underlying(LightLutIndex::D0)) | (1 << Meta::to_underlying(LightLutIndex::D1)) | (1 << Meta::to_underlying(LightLutIndex::RR)) | (0xff << Meta::to_underlying(LightLutIndex::D0)),
                    (1 << Meta::to_underlying(LightLutIndex::D0)) | (1 << Meta::to_underlying(LightLutIndex::D1)) | (1 << Meta::to_underlying(LightLutIndex::FR)) | (0xff << Meta::to_underlying(LightLutIndex::D0)),
                    0xffffff ^ (1 << Meta::to_underlying(LightLutIndex::FR)),
                    0xffffff ^ (1 << Meta::to_underlying(LightLutIndex::D1)),
                    0xffffff ^ (1 << Meta::to_underlying(LightLutIndex::RB)) ^ (1 << Meta::to_underlying(LightLutIndex::RG)),
                    0,
                    0xffffff,
                };
                auto enabled_luts_mask = enabled_masks[lut_config()];
                auto used_luts_mask = GetEnabledLUTMask();
                return !used_luts_mask || ((used_luts_mask & enabled_luts_mask) == used_luts_mask);
            }
        } config;
        #pragma pack()

        struct {
            uint32_t storage;

            constexpr auto entry_index() const { return BitField::v3::MakeFieldOn<0, 8>(this); }
            constexpr auto table_selector() const { return BitField::v3::MakeFieldOn<8, 5, LightLutIndex>(this); }
        } lut_write_index;

        uint32_t disabled_storage;
        constexpr auto disabled() const { return BitField::v3::MakeFlagOn<0>(&disabled_storage); }

        INSERT_PADDING_WORDS(0x1);

        // Sink register for writing LUT data to the location given by lut_write_index
        struct {
            uint32_t storage;

            // Unsigned 0.12-bit fixed-point
            constexpr auto value() const { return BitField::v3::MakeFieldOn<0, 12>(this); }

            // Signed 1.0.11-bit fixed-point
            constexpr auto delta() const { return BitField::v3::MakeFieldOn<12, 12>(this); }
        } set_lut_data[8];

        struct {
            uint32_t storage_index_signs; // TODO
            uint32_t storage_selectors;
            uint32_t storage_scales; // TODO

            // If set, the LUT index is computed by multiplying the dot product with 128 and clamping to [-128;127].
            // Otherwise, it is computed by multiplying with 256 and clamping to [0;255].
            // For the latter case, also consider the abs flag in the light config.
            constexpr auto index_signed_d0() const { return BitField::v3::MakeFlagOn< 1>(&storage_index_signs); }
            constexpr auto index_signed_d1() const { return BitField::v3::MakeFlagOn< 5>(&storage_index_signs); }
            constexpr auto index_signed_sp() const { return BitField::v3::MakeFlagOn< 9>(&storage_index_signs); }
            constexpr auto index_signed_fr() const { return BitField::v3::MakeFlagOn<13>(&storage_index_signs); }
            constexpr auto index_signed_rb() const { return BitField::v3::MakeFlagOn<17>(&storage_index_signs); }
            constexpr auto index_signed_rg() const { return BitField::v3::MakeFlagOn<21>(&storage_index_signs); }
            constexpr auto index_signed_rr() const { return BitField::v3::MakeFlagOn<25>(&storage_index_signs); }

            constexpr auto selector_d0() const { return BitField::v3::MakeFieldOn< 0, 3, LightLutInput>(&storage_selectors); }
            constexpr auto selector_d1() const { return BitField::v3::MakeFieldOn< 4, 3, LightLutInput>(&storage_selectors); }
            constexpr auto selector_sp() const { return BitField::v3::MakeFieldOn< 8, 3, LightLutInput>(&storage_selectors); }
            constexpr auto selector_fr() const { return BitField::v3::MakeFieldOn<12, 3, LightLutInput>(&storage_selectors); }
            constexpr auto selector_rb() const { return BitField::v3::MakeFieldOn<16, 3, LightLutInput>(&storage_selectors); }
            constexpr auto selector_rg() const { return BitField::v3::MakeFieldOn<20, 3, LightLutInput>(&storage_selectors); }
            constexpr auto selector_rr() const { return BitField::v3::MakeFieldOn<24, 3, LightLutInput>(&storage_selectors); }

            constexpr auto scale_d0() const { return BitField::v3::MakeFieldOn< 0, 3, LightLutScale>(&storage_scales); }
            constexpr auto scale_d1() const { return BitField::v3::MakeFieldOn< 4, 3, LightLutScale>(&storage_scales); }
            constexpr auto scale_sp() const { return BitField::v3::MakeFieldOn< 8, 3, LightLutScale>(&storage_scales); }
            constexpr auto scale_fr() const { return BitField::v3::MakeFieldOn<12, 3, LightLutScale>(&storage_scales); }
            constexpr auto scale_rb() const { return BitField::v3::MakeFieldOn<16, 3, LightLutScale>(&storage_scales); }
            constexpr auto scale_rg() const { return BitField::v3::MakeFieldOn<20, 3, LightLutScale>(&storage_scales); }
            constexpr auto scale_rr() const { return BitField::v3::MakeFieldOn<24, 3, LightLutScale>(&storage_scales); }
        } lut_config;

        INSERT_PADDING_WORDS(0x6);

        uint32_t storage_light_permutation;

        // Maps light IDs (0..max_light_id) to indexes into the light config array
        unsigned GetLightIndex(unsigned id) const {
            return (storage_light_permutation >> (4 * id)) & 0b111;
        }

        INSERT_PADDING_WORDS(0x26);
    } lighting;

    struct VertexAttributes {
        enum class Format : uint64_t {
            BYTE = 0,
            UBYTE = 1,
            SHORT = 2,
            FLOAT = 3,
        };

        BitFieldLegacy<0, 29, uint32_t> base_address;

        uint32_t GetPhysicalBaseAddress() const {
            return DecodeAddressRegister(base_address);
        }

        // Descriptor for internal vertex attributes
        union {
            BitFieldLegacy< 0,  2, Format> format0; // size of one element
            BitFieldLegacy< 2,  2, uint64_t> size0;      // number of elements minus 1
            BitFieldLegacy< 4,  2, Format> format1;
            BitFieldLegacy< 6,  2, uint64_t> size1;
            BitFieldLegacy< 8,  2, Format> format2;
            BitFieldLegacy<10,  2, uint64_t> size2;
            BitFieldLegacy<12,  2, Format> format3;
            BitFieldLegacy<14,  2, uint64_t> size3;
            BitFieldLegacy<16,  2, Format> format4;
            BitFieldLegacy<18,  2, uint64_t> size4;
            BitFieldLegacy<20,  2, Format> format5;
            BitFieldLegacy<22,  2, uint64_t> size5;
            BitFieldLegacy<24,  2, Format> format6;
            BitFieldLegacy<26,  2, uint64_t> size6;
            BitFieldLegacy<28,  2, Format> format7;
            BitFieldLegacy<30,  2, uint64_t> size7;
            BitFieldLegacy<32,  2, Format> format8;
            BitFieldLegacy<34,  2, uint64_t> size8;
            BitFieldLegacy<36,  2, Format> format9;
            BitFieldLegacy<38,  2, uint64_t> size9;
            BitFieldLegacy<40,  2, Format> format10;
            BitFieldLegacy<42,  2, uint64_t> size10;
            BitFieldLegacy<44,  2, Format> format11;
            BitFieldLegacy<46,  2, uint64_t> size11;

            // If bit N in this field is set, data written to fixed_vertex_attribute_sink
            // will be used as the default for uninitialized attributes.
            // (Note that vertex loaders take priority over this default value)
            BitFieldLegacy<48, 12, uint64_t> fixed_attribute_mask;

            // number of total attributes minus 1
            BitFieldLegacy<60,  4, uint64_t> num_extra_attributes;
        };

        inline Format GetFormat(int n) const {
            Format formats[] = {
                format0, format1, format2, format3,
                format4, format5, format6, format7,
                format8, format9, format10, format11
            };
            return formats[n];
        }

        inline int GetNumElements(int n) const {
            uint64_t sizes[] = {
                size0, size1, size2, size3,
                size4, size5, size6, size7,
                size8, size9, size10, size11
            };
            return (int)sizes[n]+1;
        }

        inline int GetElementSizeInBytes(int n) const {
            return (GetFormat(n) == Format::FLOAT) ? 4 :
                (GetFormat(n) == Format::SHORT) ? 2 : 1;
        }

        inline int GetStride(int n) const {
            return GetNumElements(n) * GetElementSizeInBytes(n);
        }

        inline uint32_t GetNumTotalAttributes() const {
            return static_cast<uint32_t>(num_extra_attributes + 1);
        }

        // Attribute loaders map the source vertex data to input attributes
        // This e.g. allows to load different attributes from different memory locations
        struct {
            // Source attribute data offset from the base address
            uint32_t data_offset;

            union {
                BitFieldLegacy< 0, 4, uint64_t> comp0;
                BitFieldLegacy< 4, 4, uint64_t> comp1;
                BitFieldLegacy< 8, 4, uint64_t> comp2;
                BitFieldLegacy<12, 4, uint64_t> comp3;
                BitFieldLegacy<16, 4, uint64_t> comp4;
                BitFieldLegacy<20, 4, uint64_t> comp5;
                BitFieldLegacy<24, 4, uint64_t> comp6;
                BitFieldLegacy<28, 4, uint64_t> comp7;
                BitFieldLegacy<32, 4, uint64_t> comp8;
                BitFieldLegacy<36, 4, uint64_t> comp9;
                BitFieldLegacy<40, 4, uint64_t> comp10;
                BitFieldLegacy<44, 4, uint64_t> comp11;

                // bytes for a single vertex in this loader
                BitFieldLegacy<48, 8, uint64_t> byte_count;

                BitFieldLegacy<60, 4, uint64_t> component_count;
            };

            inline int GetComponent(int n) const {
                uint64_t components[] = {
                    comp0, comp1, comp2, comp3,
                    comp4, comp5, comp6, comp7,
                    comp8, comp9, comp10, comp11
                };
                return (int)components[n];
            }
        } attribute_loaders[12];
    } vertex_attributes;

    struct {
        enum IndexFormat : uint32_t {
            BYTE = 0,
            SHORT = 1,
        };

        union {
            BitFieldLegacy<0, 31, uint32_t> offset; // relative to base attribute address
            BitFieldLegacy<31, 1, IndexFormat> format;
        };
    } index_array;

    // Number of vertices to render
    uint32_t num_vertices;

    INSERT_PADDING_WORDS(0x1);

    // Vertex offset to apply for non-indexed rendering
    uint32_t vertex_offset;

    INSERT_PADDING_WORDS(0x3);

    // These two trigger rendering of triangles
    uint32_t trigger_draw;
    uint32_t trigger_draw_indexed;

    INSERT_PADDING_WORDS(0x2);

    struct {
        bool IsImmediateSubmission() const {
            return (index == 0xf);
        }

        // Selects which attribute to set up the default attribute to.
        // Alternatively enables immediate mode vertex submission if set to 0xf.
        uint32_t index;

        uint32_t data[3];
    } fixed_vertex_attribute_sink;

    INSERT_PADDING_WORDS(2);

    struct  {
        // Register values for the two command processor engines, respectively

        uint32_t size[2];    // Number of byte octets
        uint32_t address[2]; // Encoded address (divided by 8)
        uint32_t trigger[2];
    } command_processor;

    INSERT_PADDING_WORDS(4);
    uint32_t immediate_rendering_max_input_attribute_index;
    INSERT_PADDING_WORDS(0x7);
    uint32_t vs_output_attributes_minus_1;
    INSERT_PADDING_WORDS(0x6);
    uint32_t vs_output_attributes_minus_1_copy;
    INSERT_PADDING_WORDS(0xc);

    enum class TriangleTopology : uint32_t {
        List        = 0,
        Strip       = 1,
        Fan         = 2,
        ListIndexed = 3, // TODO: No idea if this is correct
    };

    BitFieldLegacy<8, 2, TriangleTopology> triangle_topology;

    INSERT_PADDING_WORDS(0x51);

    BitFieldLegacy<0, 16, uint32_t> vs_bool_uniforms;
    union {
        BitFieldLegacy< 0, 8, uint32_t> x;
        BitFieldLegacy< 8, 8, uint32_t> y;
        BitFieldLegacy<16, 8, uint32_t> z;
        BitFieldLegacy<24, 8, uint32_t> w;
    } vs_int_uniforms[4];

    INSERT_PADDING_WORDS(0x4);
    uint32_t reg_0x2b9;
    auto max_shader_input_attribute_index() const {
        return v3::BitField::MakeFieldOn<0, 4>(&reg_0x2b9);
    }

    // Offset to shader program entry point (in words)
    BitFieldLegacy<0, 16, uint32_t> vs_main_offset;

    union VSInputRegisterMap {
        BitFieldLegacy< 0, 32, uint64_t> low;
        BitFieldLegacy<32, 32, uint64_t> high;

        BitFieldLegacy< 0, 4, uint64_t> attribute0_register;
        BitFieldLegacy< 4, 4, uint64_t> attribute1_register;
        BitFieldLegacy< 8, 4, uint64_t> attribute2_register;
        BitFieldLegacy<12, 4, uint64_t> attribute3_register;
        BitFieldLegacy<16, 4, uint64_t> attribute4_register;
        BitFieldLegacy<20, 4, uint64_t> attribute5_register;
        BitFieldLegacy<24, 4, uint64_t> attribute6_register;
        BitFieldLegacy<28, 4, uint64_t> attribute7_register;
        BitFieldLegacy<32, 4, uint64_t> attribute8_register;
        BitFieldLegacy<36, 4, uint64_t> attribute9_register;
        BitFieldLegacy<40, 4, uint64_t> attribute10_register;
        BitFieldLegacy<44, 4, uint64_t> attribute11_register;
        BitFieldLegacy<48, 4, uint64_t> attribute12_register;
        BitFieldLegacy<52, 4, uint64_t> attribute13_register;
        BitFieldLegacy<56, 4, uint64_t> attribute14_register;
        BitFieldLegacy<60, 4, uint64_t> attribute15_register;

        int GetRegisterForAttribute(int attribute_index) const {
            uint64_t fields[] = {
                attribute0_register,  attribute1_register,  attribute2_register,  attribute3_register,
                attribute4_register,  attribute5_register,  attribute6_register,  attribute7_register,
                attribute8_register,  attribute9_register,  attribute10_register, attribute11_register,
                attribute12_register, attribute13_register, attribute14_register, attribute15_register,
            };
            return (int)fields[attribute_index];
        }
    } vs_input_register_map;

    struct {
        uint32_t raw;

        /**
         * Mask of enabled shader output registers. This mask determines which
         * output registers are writeable how by shaders and how they map to
         * output attributes (as sent to the post-shader-pipeline).
         */
        auto mask() const { return BitField::v3::MakeFieldOn<0, 16>(this); }
    } vs_output_register_mask;

    INSERT_PADDING_WORDS(0x2);

    struct {
        enum Format : uint32_t
        {
            FLOAT24 = 0,
            FLOAT32 = 1
        };

        bool IsFloat32() const {
            return format == FLOAT32;
        }

        union {
            // Index of the next uniform to write to
            // TODO: ctrulib uses 8 bits for this, however that seems to yield lots of invalid indices
            BitFieldLegacy<0, 7, uint32_t> index;

            BitFieldLegacy<31, 1, Format> format;
        };

        // Writing to these registers sets the "current" uniform.
        // TODO: It's not clear how the hardware stores what the "current" uniform is.
        uint32_t set_value[8];

    } vs_uniform_setup;

    INSERT_PADDING_WORDS(0x2);

    struct {
        // Offset of the next instruction to write code to.
        // Incremented with each instruction write.
        uint32_t offset;

        // Writing to these registers sets the "current" word in the shader program.
        // TODO: It's not clear how the hardware stores what the "current" word is.
        uint32_t set_word[8];
    } vs_program;

    INSERT_PADDING_WORDS(0x1);

    // This register group is used to load an internal table of swizzling patterns,
    // which are indexed by each shader instruction to specify vector component swizzling.
    struct {
        // Offset of the next swizzle pattern to write code to.
        // Incremented with each instruction write.
        uint32_t offset;

        // Writing to these registers sets the "current" swizzle pattern in the table.
        // TODO: It's not clear how the hardware stores what the "current" swizzle pattern is.
        uint32_t set_word[8];
    } vs_swizzle_patterns;

    INSERT_PADDING_WORDS(0x22);

#undef INSERT_PADDING_WORDS_HELPER1
#undef INSERT_PADDING_WORDS_HELPER2
#undef INSERT_PADDING_WORDS

    // Map register indices to names readable by humans
    // Used for debugging purposes, so performance is not an issue here
    static std::string GetCommandName(int index) {
        std::map<uint32_t, std::string> map;

        #define ADD_FIELD(name)                                                                               \
            do {                                                                                              \
                map.insert({PICA_REG_INDEX(name), #name});                                                    \
                for (uint32_t i = PICA_REG_INDEX(name) + 1; i < PICA_REG_INDEX(name) + sizeof(Regs().name) / 4; ++i) \
                    map.insert({i, #name + std::string("+") + std::to_string(i-PICA_REG_INDEX(name))});       \
            } while(false)

        ADD_FIELD(trigger_irq);
        ADD_FIELD(cull_mode);
        ADD_FIELD(viewport_size_x);
        ADD_FIELD(viewport_size_y);
        ADD_FIELD(viewport_depth_range);
        ADD_FIELD(viewport_depth_far_plane);
        ADD_FIELD(shader_num_output_attributes);
        ADD_FIELD(viewport_corner);
        ADD_FIELD(texture0_enable);
        ADD_FIELD(texture0);
        ADD_FIELD(texture0_format);
        ADD_FIELD(texture1);
        ADD_FIELD(texture1_format);
        ADD_FIELD(texture2);
        ADD_FIELD(texture2_format);
        ADD_FIELD(tev_stage0);
        ADD_FIELD(tev_stage1);
        ADD_FIELD(tev_stage2);
        ADD_FIELD(tev_stage3);
        ADD_FIELD(combiner_buffer);
        ADD_FIELD(tev_stage4);
        ADD_FIELD(tev_stage5);
        ADD_FIELD(combiner_buffer_init);
        ADD_FIELD(output_merger);
        ADD_FIELD(framebuffer);
        ADD_FIELD(lighting);
        ADD_FIELD(vertex_attributes);
        ADD_FIELD(index_array);
        ADD_FIELD(num_vertices);
        ADD_FIELD(vertex_offset);
        ADD_FIELD(trigger_draw);
        ADD_FIELD(trigger_draw_indexed);
        ADD_FIELD(triangle_topology);
        ADD_FIELD(vs_bool_uniforms);
        ADD_FIELD(vs_int_uniforms);
        ADD_FIELD(reg_0x2b9);
        ADD_FIELD(vs_main_offset);
        ADD_FIELD(vs_input_register_map);
        ADD_FIELD(vs_output_register_mask);
        ADD_FIELD(vs_uniform_setup);
        ADD_FIELD(vs_program);
        ADD_FIELD(vs_swizzle_patterns);

        #undef ADD_FIELD

        // Return empty string if no match is found
        return map[index];
    }

    static inline size_t NumIds() {
        return sizeof(Regs) / sizeof(uint32_t);
    }

    uint32_t& operator [] (int index) const {
        uint32_t* content = (uint32_t*)this;
        return content[index];
    }

    uint32_t& operator [] (int index) {
        uint32_t* content = (uint32_t*)this;
        return content[index];
    }

private:
    /*
     * Most physical addresses which Pica registers refer to are 8-byte aligned.
     * This function should be used to get the address from a raw register value.
     */
    static inline uint32_t DecodeAddressRegister(uint32_t register_value) {
        return register_value * 8;
    }
};

// TODO: MSVC does not support using offsetof() on non-static data members even though this
//       is technically allowed since C++11. This macro should be enabled once MSVC adds
//       support for that.
#ifndef _MSC_VER
#define ASSERT_REG_POSITION(field_name, position) static_assert(offsetof(Regs, field_name) == position * 4, "Field "#field_name" has invalid position")

ASSERT_REG_POSITION(trigger_irq, 0x10);
ASSERT_REG_POSITION(cull_mode, 0x40);
ASSERT_REG_POSITION(viewport_size_x, 0x41);
ASSERT_REG_POSITION(viewport_size_y, 0x43);
ASSERT_REG_POSITION(viewport_depth_range, 0x4d);
ASSERT_REG_POSITION(viewport_depth_far_plane, 0x4e);
ASSERT_REG_POSITION(shader_num_output_attributes, 0x4f);
ASSERT_REG_POSITION(shader_output_semantics, 0x50);
ASSERT_REG_POSITION(viewport_corner, 0x68);
ASSERT_REG_POSITION(texture0_enable, 0x80);
ASSERT_REG_POSITION(texture0, 0x81);
ASSERT_REG_POSITION(texture0_format, 0x8e);
ASSERT_REG_POSITION(texture1, 0x91);
ASSERT_REG_POSITION(texture1_format, 0x96);
ASSERT_REG_POSITION(texture2, 0x99);
ASSERT_REG_POSITION(texture2_format, 0x9e);
ASSERT_REG_POSITION(tev_stage0, 0xc0);
ASSERT_REG_POSITION(tev_stage1, 0xc8);
ASSERT_REG_POSITION(tev_stage2, 0xd0);
ASSERT_REG_POSITION(tev_stage3, 0xd8);
ASSERT_REG_POSITION(combiner_buffer, 0xe0);
ASSERT_REG_POSITION(tev_stage4, 0xf0);
ASSERT_REG_POSITION(tev_stage5, 0xf8);
ASSERT_REG_POSITION(combiner_buffer_init, 0xfd);
ASSERT_REG_POSITION(output_merger, 0x100);
ASSERT_REG_POSITION(framebuffer, 0x110);
ASSERT_REG_POSITION(lighting, 0x140);
ASSERT_REG_POSITION(vertex_attributes, 0x200);
ASSERT_REG_POSITION(index_array, 0x227);
ASSERT_REG_POSITION(num_vertices, 0x228);
ASSERT_REG_POSITION(vertex_offset, 0x22a);
ASSERT_REG_POSITION(trigger_draw, 0x22e);
ASSERT_REG_POSITION(trigger_draw_indexed, 0x22f);
ASSERT_REG_POSITION(fixed_vertex_attribute_sink, 0x232);
ASSERT_REG_POSITION(command_processor, 0x238);
ASSERT_REG_POSITION(immediate_rendering_max_input_attribute_index, 0x242);
ASSERT_REG_POSITION(vs_output_attributes_minus_1, 0x24a);
ASSERT_REG_POSITION(vs_output_attributes_minus_1_copy, 0x251);
ASSERT_REG_POSITION(triangle_topology, 0x25e);
ASSERT_REG_POSITION(vs_bool_uniforms, 0x2b0);
ASSERT_REG_POSITION(vs_int_uniforms, 0x2b1);
ASSERT_REG_POSITION(reg_0x2b9, 0x2b9);
ASSERT_REG_POSITION(vs_main_offset, 0x2ba);
ASSERT_REG_POSITION(vs_input_register_map, 0x2bb);
ASSERT_REG_POSITION(vs_output_register_mask, 0x2bd);
ASSERT_REG_POSITION(vs_uniform_setup, 0x2c0);
ASSERT_REG_POSITION(vs_program, 0x2cb);
ASSERT_REG_POSITION(vs_swizzle_patterns, 0x2d5);

#undef ASSERT_REG_POSITION
#endif // !defined(_MSC_VER)

// The total number of registers is chosen arbitrarily, but let's make sure it's not some odd value anyway.
static_assert(sizeof(Regs) <= 0x300 * sizeof(uint32_t), "Register set structure larger than it should be");
static_assert(sizeof(Regs) >= 0x300 * sizeof(uint32_t), "Register set structure smaller than it should be");

union CommandHeader {
    uint32_t hex;

    BitFieldLegacy< 0, 16, uint32_t> cmd_id;
    BitFieldLegacy<16,  4, uint32_t> parameter_mask;
    BitFieldLegacy<20, 11, uint32_t> extra_data_length;
    BitFieldLegacy<31,  1, uint32_t> group_commands;
};

} // namespace
