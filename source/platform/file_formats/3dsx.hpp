#pragma once

#include <framework/formats.hpp>

#include <boost/hana/define_struct.hpp>

#include <array>
#include <cstdint>

namespace FileFormat {

/**
 * 3DSX file format specification: 3DSX is a community-invented file format
 * used for running userland homebrew applications on the 3DS.
 */
namespace Dot3DSX {

/**
 * Main 3DSX header. This header is followed by:
 * - an optional secondary header (referred to as "3DSX extended header", not to be confused with NCCH extended headers)
 * - three RelocationHeaders (one each for the code, rodata, and data segments)
 * - the code and rodata segments
 * - the data segment (without bss TODO: Double check!)
 * - a sequence of RelocationDescriptors for each program segment
 */
struct Header {
    BOOST_HANA_DEFINE_STRUCT(Header,
        (std::array<uint8_t, 4>, magic), // "3DSX"
        (uint16_t, header_size),         // Size of this header and (if enabled) the extended header
        (uint16_t, reloc_header_size),   // Size of the RelocationHeader
        (uint32_t, version),             // Version (always 0 as of the time of writing)
        (uint32_t, flags),               // Not used for anything

        // Segment sizes (in bytes)
        (uint32_t, text_size),
        (uint32_t, ro_size),
        (uint32_t, data_bss_size),       // Data and bss section
        (uint32_t, bss_size)             // Bss section, only
    );

    uint32_t TextOffset() const {
        return header_size + 3 * reloc_header_size;
    }

    uint32_t RoOffset() const {
        return TextOffset() + text_size;
    }

    uint32_t DataOffset() const {
        return RoOffset() + ro_size;
    }

    uint32_t RelocationHeaderOffset(unsigned int index) const {
        return header_size + reloc_header_size * index;
    }

    uint32_t CodeRelocationInfoOffset() const {
        return DataOffset() + (data_bss_size - bss_size);
    }

    struct Tags : little_endian_tag, expected_size_tag<0x20> {};

// Supported in GCC 7.1, but not in older ones. Not exactly sure at which point specifically they added support
#if !defined(__GNUC__) || __GNUC__ >= 7
    // Identifying word at the file beginning
    static inline constexpr std::array<char, 4> expected_magic = {{ '3', 'D', 'S', 'X' }};
#endif
};

/**
 * 3DSX secondary header (optional)
 */
struct SecondaryHeader {
    BOOST_HANA_DEFINE_STRUCT(SecondaryHeader,
        (uint32_t, smdh_offset), // In bytes
        (uint32_t, smdh_size),   // In bytes
        (uint32_t, romfs_offset) // In bytes (All remaining data in the file is considered part of RomFS, hence implicitly defining its size)
    );
};

struct RelocationHeader {
    BOOST_HANA_DEFINE_STRUCT(RelocationHeader,
        (uint32_t, num_abs_relocs),
        (uint32_t, num_rel_relocs)
    );

    struct Tags : little_endian_tag, expected_size_tag<0x8> {};
};
static_assert(sizeof(RelocationHeader) == 8, "Structure has incorrect size");

struct RelocationInfo {
    BOOST_HANA_DEFINE_STRUCT(RelocationInfo,
        (uint16_t, words_to_skip),
        (uint16_t, words_to_patch)
    );

    struct Tags : little_endian_tag, expected_size_tag<0x4> {};
};

} // namespace Dot3DSX

} // namespace FileFormat
