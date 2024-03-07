/**
 * DSP firmware; uses file extension "cdc"
 */

#pragma once

#include <framework/bit_field_new.hpp>
#include <framework/formats.hpp>

#include <boost/hana/define_struct.hpp>

#include <cstdint>

namespace FileFormat {

struct DSPFirmwareHeader {
    struct InitFlags {
        BOOST_HANA_DEFINE_STRUCT(InitFlags,
            (uint8_t, storage)
        );

        // If set, wait for DSP replies with data == 1 to be sent on each channel
        auto wait_for_dsp_reply() const { return BitField::v3::MakeFlagOn<0>(this); }

        // If set, read 3d filter data from config module
        auto load_3d_filters() const { return BitField::v3::MakeFlagOn<1>(this); }
    };

    struct SegmentInfo {
        BOOST_HANA_DEFINE_STRUCT(SegmentInfo,
            (uint32_t, data_offset),              // offset in bytes of segment contents from the start of this header
            (uint32_t, target_offset),            // target offset, 16-bit words
            (uint32_t, size_bytes),
            (std::array<uint8_t, 3>, unknown),
            (uint8_t, memory_type),               // memory type; 0/1: program memory, 2: data memory
            (std::array<uint8_t, 32>, hash)       // SHA256 hash over segment contents
        );
    };

    BOOST_HANA_DEFINE_STRUCT(DSPFirmwareHeader,
        (std::array<uint8_t, 0x100>, signature),  // RSA signature over the rest of this header
        (std::array<uint8_t, 4>, magic),          // always "DSP1"
        (uint32_t, size_bytes),                   // size including header
        (uint16_t, memory_regions_mask),          // mask enabling 32 kB regions in DSP memory
        (std::array<uint8_t, 3>, unknown),
        (uint8_t, memory_type_3d_filters),        // memory type for 3D filter segment
        (uint8_t, num_segments),                  // number of entries to use from the segments array
        (InitFlags, init_flags),                  // flags to apply during loading
        (uint32_t, start_offset_3d_filters),      // counted in 16-bit words. TODO: Is this from the start of DSP memory?
        (uint32_t, size_bytes_3d_filters),        // size of 3d filters segment
        (uint64_t, unknown2),
        (std::array<SegmentInfo, 10>, segments)
    );

    struct Tags : expected_size_tag<0x300> {};
};

} // namespace FileFormat
