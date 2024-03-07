#pragma once

#include <cstdint>

namespace Pica {

struct ZOrderOffset {
    static constexpr uint32_t block_width = 8;
    static constexpr uint32_t block_height = 8;

    uint32_t texel_within_tile;
    uint32_t coarse_x;
    uint32_t coarse_y;
};

inline ZOrderOffset ZOrderTileOffset(uint32_t x, uint32_t y) {
    // Images are split into 8x8 tiles. Each tile is composed of four 4x4 subtiles each
    // of which is composed of four 2x2 subtiles each of which is composed of four texels.
    // Each structure is embedded into the next-bigger one in a diagonal pattern, e.g.
    // texels are laid out in a 2x2 subtile like this:
    // 2 3
    // 0 1
    //
    // The full 8x8 tile has the texels arranged like this:
    //
    // 42 43 46 47 58 59 62 63
    // 40 41 44 45 56 57 60 61
    // 34 35 38 39 50 51 54 55
    // 32 33 36 37 48 49 52 53
    // 10 11 14 15 26 27 30 31
    // 08 09 12 13 24 25 28 29
    // 02 03 06 07 18 19 22 23
    // 00 01 04 05 16 17 20 21

    // TODO: More flexible encoding algorithms exist for this!
    uint32_t texel_index_within_tile = 0;
    for (int block_size_index = 0; block_size_index < 3; ++block_size_index) {
        int sub_tile_width = 1 << block_size_index;
        int sub_tile_height = 1 << block_size_index;

        int sub_tile_index = (x & sub_tile_width) << block_size_index;
        sub_tile_index += 2 * ((y & sub_tile_height) << block_size_index);
        texel_index_within_tile += sub_tile_index;
    }

    uint32_t coarse_x = (x / ZOrderOffset::block_width) * ZOrderOffset::block_width;
    uint32_t coarse_y = (y / ZOrderOffset::block_height) * ZOrderOffset::block_height;

    return { texel_index_within_tile, coarse_x, coarse_y };
}

} // namespace Pica
