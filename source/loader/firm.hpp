#pragma once

#include <boost/endian/arithmetic.hpp>

#include <istream>

namespace FIRM {

using boost::endian::little_uint32_t;

struct SectionHeader {
    little_uint32_t file_offset;  // offset within file to section data
    little_uint32_t load_address; // physical memory address to load section data to
    little_uint32_t size;         // size in bytes (may be 0)
    little_uint32_t type;         // 0 = ARM9, 1 = ARM11
    uint8_t         hash[0x20];   // SHA-256 hash of the section data
};
static_assert(sizeof(SectionHeader) == 0x30, "Incorrect FIRM section header size");

struct Header {
    char magic[4]; // "FIRM"

    char reserved1[4];

    little_uint32_t entry_arm11; // physical memory address
    little_uint32_t entry_arm9;  // physical memory address

    char reserved2[0x30];

    SectionHeader sections[4];

    uint8_t signature[0x100]; // RSA-2048 signature of the header
};
static_assert(sizeof(Header) == 0x200, "Incorrect FIRM header size");

}  // namespace FIRM

namespace Loader {

/**
 * @note The stream read cursor is restored when this function returns
 */
bool IsFirm(std::istream& str);

}  // namespace Loader
