#pragma once

#include <array>
#include <cstdint>
#include <optional>

struct KeyDatabase {
    // Each AES slot has three entries called KeyX, KeyY, and KeyN ("normal key")
    static constexpr int num_aes_slots = 0x40;

    using KeyType = std::array<uint8_t, 16>;

    struct {
        std::optional<KeyType> x;
        std::optional<KeyType> y;
        std::optional<KeyType> n;
    } aes_slots[num_aes_slots];

    // KeyYs used to decrypt title keys (required to decrypt CDN contents and update CIAs)
    std::array<std::optional<KeyType>, 6> common_y;
};
