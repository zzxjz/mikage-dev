#pragma once

#include <framework/formats.hpp>

#include <boost/hana/define_struct.hpp>

#include <array>
#include <cstdint>

namespace FileFormat::BCFNT {

struct Header {
    BOOST_HANA_DEFINE_STRUCT(Header,
        (std::array<uint8_t, 4>, magic),   // "CFNT" (on filesystem) or "CFNU" (decompressed in NS shared memory)
        (std::array<uint8_t, 2>, unknown),
        (uint16_t, header_size_bytes),
        (uint32_t, version),
        (uint32_t, file_size_bytes),
        (uint32_t, num_blocks)
    );
};

// Common structure shared between block headers defined below
struct BlockCommon {
    BOOST_HANA_DEFINE_STRUCT(BlockCommon,
        (std::array<uint8_t, 4>, magic),
        (uint32_t, num_bytes) // Number of bytes from block start to the next block; may include data following the headers below
    );
};

struct FINF {
    static constexpr std::array<uint8_t, 4> magic = { 'F', 'I', 'N', 'F' };

    BOOST_HANA_DEFINE_STRUCT(FINF,
        (std::array<uint8_t, 0x8>, unknown1),
        (uint32_t, tglp_offset),
        (uint32_t, cwdh_offset),
        (uint32_t, cmap_offset),
        (std::array<uint8_t, 0x4>, unknown2)
    );
};
static_assert(sizeof(FINF) == 24);

struct CMAP {
    static constexpr std::array<uint8_t, 4> magic = { 'C', 'M', 'A', 'P' };

    BOOST_HANA_DEFINE_STRUCT(CMAP,
        (std::array<uint8_t, 0x8>, unknown),
        (uint32_t, next_block_offset)
    );
};

struct CWDH {
    static constexpr std::array<uint8_t, 4> magic = { 'C', 'W', 'D', 'H' };

    BOOST_HANA_DEFINE_STRUCT(CWDH,
        (std::array<uint8_t, 0x4>, unknown),
        (uint32_t, next_block_offset)
    );
};

struct TGLP {
    static constexpr std::array<uint8_t, 4> magic = { 'T', 'G', 'L', 'P' };

    BOOST_HANA_DEFINE_STRUCT(TGLP,
        (std::array<uint8_t, 0x14>, unknown),
        (uint32_t, data_offset)
    );
};

} // namespace FileFormat::BCFNT
