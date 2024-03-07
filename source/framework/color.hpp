#pragma once

#include <cstdint>

namespace Color {

/// Convert a 1-bit color component to 8 bit
static inline uint8_t Convert1To8(uint8_t value) {
    return value * 255;
}

/// Convert a 4-bit color component to 8 bit
static inline uint8_t Convert4To8(uint8_t value) {
    return (value << 4) | value;
}

/// Convert a 5-bit color component to 8 bit
static inline uint8_t Convert5To8(uint8_t value) {
    return (value << 3) | (value >> 2);
}

/// Convert a 6-bit color component to 8 bit
static inline uint8_t Convert6To8(uint8_t value) {
    return (value << 2) | (value >> 4);
}

} // namespace
