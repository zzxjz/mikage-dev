#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace Pica {

template<unsigned ExponentBits, unsigned MantissaBits>
struct floatN {
    static constexpr unsigned bits = 1 + ExponentBits + MantissaBits;
    static constexpr uint32_t MantissaMask = (1 << MantissaBits) - 1;
    static constexpr uint32_t ExponentMask = ((1 << ExponentBits) - 1) << MantissaBits;
    static constexpr uint32_t SignMask = (1 << (ExponentBits + MantissaBits));
    static constexpr uint32_t FullMask = ((1 << (ExponentBits + MantissaBits + 1)) - 1);

    static constexpr unsigned ExponentBias = (1 << (ExponentBits - 1)) - 1;

    static floatN FromFloat32(float val) {
        floatN ret;
        ret.value = val;
        return ret;
    }

    static floatN FromRawFloat(uint32_t hex) {
        floatN ret;
        if ((hex & FullMask) == 0) {
            ret.value = 0;
        } else {
            uint32_t mantissa = hex & MantissaMask;
            uint32_t exponent = (hex & ExponentMask) >> MantissaBits;
            uint32_t sign = hex >> (ExponentBits + MantissaBits);
            if (exponent == ExponentMask) {
                throw /*std::runtime_error*/("Can't convert infinity/NaN float value");
            } else if (exponent == 0) {
                // TODO: Actually a denormal
                ret.value = 0;
            } else {
                constexpr unsigned MantissaBits32 = 23;
                constexpr unsigned ExponentBias32 = 127;
                mantissa <<= (MantissaBits32 - MantissaBits);
                exponent += (ExponentBias32 - ExponentBias);
                exponent <<= MantissaBits32;
                uint32_t raw_value = (sign << 31u) | exponent | mantissa;
                memcpy(&ret.value, &raw_value, sizeof(raw_value));
            }
        }

        return ret;
    }

    float ToFloat32() const {
        return value;
    }

    floatN operator / (const floatN& flt) const {
        return floatN::FromFloat32(ToFloat32() / flt.ToFloat32());
    }

    floatN operator + (const floatN& flt) const {
        return floatN::FromFloat32(ToFloat32() + flt.ToFloat32());
    }

    floatN operator - (const floatN& flt) const {
        return floatN::FromFloat32(ToFloat32() - flt.ToFloat32());
    }

    floatN operator - () const {
        return floatN::FromFloat32(-ToFloat32());
    }

    bool operator < (const floatN& flt) const {
        return ToFloat32() < flt.ToFloat32();
    }

    bool operator > (const floatN& flt) const {
        return ToFloat32() > flt.ToFloat32();
    }

    bool operator >= (const floatN& flt) const {
        return ToFloat32() >= flt.ToFloat32();
    }

    bool operator <= (const floatN& flt) const {
        return ToFloat32() <= flt.ToFloat32();
    }

    bool operator == (const floatN& flt) const {
        return ToFloat32() == flt.ToFloat32();
    }

    bool operator != (const floatN& flt) const {
        return ToFloat32() != flt.ToFloat32();
    }

private:
    // Stored as a regular float, merely for convenience
    // TODO: Perform proper arithmetic on this!
    float value;
};

using float16 = floatN<5, 10>;
using float24 = floatN<7, 16>;

const float24 zero_f24 = float24::FromFloat32(0.f);

inline float24 operator *(const float24& left, const float24& right) {
    // NOTE: IEEE 754 requires "inf * 0 == NaN", but the 3DS shader unit yields 0 in this case
    //       Effects of (not emulating) this are observed in missing UI elements in Zelda: Ocarina of Time 3D
    if (left == zero_f24 || right == zero_f24) {
        if (!std::isnan(left.ToFloat32()) && !std::isnan(right.ToFloat32())) {
            return zero_f24;
        }
    }
    return float24::FromFloat32(left.ToFloat32() * right.ToFloat32());
}

} // namespace Pica
