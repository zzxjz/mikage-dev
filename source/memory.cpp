#ifndef __ANDROID__
#include <cryptopp/sha.h>
#endif
#include "display.hpp"
#include "input.hpp"
#include "memory.h"

#include "hardware/dsp.hpp"

#include "framework/bit_field_new.hpp"
#include "framework/exceptions.hpp"
#include "framework/logging.hpp"
#include "framework/meta_tools.hpp"

#include <spdlog/spdlog.h>

#include <boost/endian/arithmetic.hpp>

#include "pica.hpp"
#include "video_core/src/video_core/renderer.hpp"
#include "video_core/src/video_core/debug_utils/debug_utils.h"

#include <teakra/teakra.h>

#include <fstream>
#include <optional>

void TeakraAudioCallback(std::array<int16_t, 2> samples);

Teakra::Teakra* g_teakra = nullptr; // TODO: Remove
Memory::PhysicalMemory* g_mem = nullptr; // TODO: Remove
bool g_dsp_running = false; // TODO: Remove
bool g_dsp_just_reset = false; // TODO: Remove

namespace Pica {
struct Context;
}

namespace GPU {
template<typename T>
void Read(Pica::Context&, T&, uint32_t);
template<typename T>
void Write(Pica::Context&, uint32_t, T);
}

namespace HLE::OS {
class OS;
void SetupOSDSPCallbacks(OS&);
extern OS* g_os;
}

namespace Memory {

struct MPCorePrivate : MemoryAccessHandler {
    uint32_t Read32(uint32_t offset) {
        switch (offset) {
        case 0x4: // Configuration Register
            // Only return the (hardcoded) number of available CPUs (minus 1) for now
            return 0;

        default:
            return MemoryAccessHandler::Read32(offset);
        }
    }
};

/// Stubbed HID MMIO handler
struct HID : MemoryAccessHandler {
    InputSource* input = nullptr;

    int read = 0;

    std::shared_ptr<spdlog::logger> logger;

    HID(LogManager& log_manager) : logger(log_manager.RegisterLogger("IO_HID")) {

    }

    uint16_t Read16(uint32_t offset) {
        logger->info("Read from HID register {:#010x}", offset);
        if (offset == 0) {
            // This is a negative mask of button presses: 0xFFFF means that no buttons are pressed.
            logger->error("Got key HID: {:#x}", input->GetButtonState());
            return ~input->GetButtonState();
        } else if (offset == 0x100 || offset == 0x102) {
            // HACK: This register does not actually exist on 3DS. It's an internal extension to ease reading touch data for FakeHID
            // TODO: Move this to SPI instead. Also return raw data (before factoring in calibration parameters!) instead
            // TODO: We query the x and y components in separate GetTouchState calls, which will cause inconsistent results to be returned!
            auto state = input->GetTouchState();
            if (state.pressed) {
                if (offset == 0x100) {
                    return static_cast<uint16_t>(0.5f + state.x * 320.f);
                } else {
                    return static_cast<uint16_t>(0.5f + state.y * 240.f);
                }
            } else {
                // magic value used to indicate non-touches
                return 0xffff;
            }
        } else if (offset == 0x104 || offset == 0x106) {
            // HACK: This register does not actually exist on 3DS. It's an internal extension to ease reading touch data for FakeHID
            // TODO: Move this to SPI instead. Also return raw data (before factoring in calibration parameters!) instead
            // TODO: We query the x and y components in separate GetTouchState calls, which will cause inconsistent results to be returned!
            auto state = input->GetCirclePadState();
            // Rescale to positive integers: [0; 312]
            if (offset == 0x104) {
                return static_cast<uint16_t>(0.5f + (1.0f + state.x) * 0x9c);
            } else {
                return static_cast<uint16_t>(0.5f + (1.0f + state.y) * 0x9c);
            }
        } else if (offset == 0x108) {
            static bool latch = false;
            // Latch to ensure we don't send out multiple notifications... TODO: Should be moved to notifier!
            auto pressed = input->IsHomeButtonPressed();
            if (latch && !pressed) {
                latch = false;
            } else if (!latch && pressed) {
                latch = true;
            }
            // HACK: This register does not actually exist on 3DS. It's an internal extension to ease reading Home button state
            // TODO: Move this to I2C device 3 instead
            return /*input->IsHomeButtonPressed()*/ latch;
        }
        return MemoryAccessHandler::Read16(offset);
    }
};

/// Stubbed LCD MMIO handler
struct LCD : MemoryAccessHandler {
    std::shared_ptr<spdlog::logger> logger;

    LCD(LogManager& log_manager) : logger(log_manager.RegisterLogger("IO_LCD")) {

    }

    uint32_t Read32(uint32_t offset) {
        logger->info("Read from LCD register {:#010x}", offset);
        return 0;
    }

    void Write32(uint32_t offset, uint32_t value) {
        logger->info("Write to LCD register {:#010x} <- {:#010x}", offset, value);
    }
};

/// Stubbed AXI MMIO handler
struct AXIHandler : MemoryAccessHandler {
    std::shared_ptr<spdlog::logger> logger;

    AXIHandler(LogManager& log_manager) : logger(log_manager.RegisterLogger("IO_AXI")) {

    }

    uint32_t Read32(uint32_t offset) {
        logger->info("Read from AXI register {:#010x}", offset);
        return 0;
    }

    void Write32(uint32_t offset, uint32_t value) {
        logger->info("Write to AXI register {:#010x} <- {:#010x}", offset, value);
    }
};

/// GPU MMIO handler
struct GPU : MemoryAccessHandler {
    Pica::Context* context;

    std::shared_ptr<spdlog::logger> logger;

    GPU(LogManager& log_manager) : logger(log_manager.RegisterLogger("IO_GPU")) {

    }

    uint32_t Read32(uint32_t offset) {
        logger->info("Read from GPU register {:#010x}", offset);

        uint32_t ret;
        ::GPU::Read<uint32_t>(*context, ret, offset + 0x1EF00000);
        return ret;
    }

    void Write32(uint32_t offset, uint32_t value) {
        logger->info("Write to GPU register {:#010x} <- {:#010x}", offset, value);

// TODO: Extract docstrings and drop the rest
#if 0
        if (offset == 0x4) {
            // Unknown behavior. Treat as dummy sink, for now.
        } else if (offset == 0x30 || offset == 0x34) {
            // VTotal/VDisp. Treat as dummy sink, for now.
        } else if (offset == 0x45c) {
            fb_size = value;
        } else if (offset == 0x55c) {
            fb_bot_size = value;
        } else if (offset == 0x468) {
            fb_addr = value;
        } else if (offset == 0x46c) {
            fb_addr2 = value;
        } else if (offset == 0x474) {
            // Undocumented:
            // IRQ flags. bit 8 = hblank IRQ enable; bit9 = vblank IRQ enable; bit 10 = error IRQ enable; bit 16 = output enable
            // But actually it's a negative masks: bits that aren't set indicate enabled IRQs, while set bits disable them
            // See https://github.com/profi200/open_agb_firm/blob/c3b57bda2965fb400b98cc9ae7ac536271e66b9d/source/arm11/hardware/gfx.c#L402
            fb_enable_top = value & 1;
        } else if (offset == 0x478) {
            // NOTE: LLE GSP first writes to this register to acknowledge IRQs.
            //       bit 16 = hblank IRQ; bit9 = vblank IRQ; bit 10 = error IRQ
            // NOTE: LLE GSP writes values like 0x00070000 to this register, so only the lowest bit should be considered

            // https://github.com/profi200/open_agb_firm/blob/c3b57bda2965fb400b98cc9ae7ac536271e66b9d/source/arm11/hardware/gfx.c#L347

            fb_select_top = (value & 1);
        } else if (offset == 0x490) {
            fb_stride[0] = value;
        } else if (offset == 0x494) {
            fb_addr_right = value;
        } else if (offset == 0x498) {
            fb_addr2_right = value;
        } else if (offset == 0x578) {
            // NOTE: LLE GSP writes values like 0x00070000 to this register, so only the lowest bit should be considered
            fb_select_bottom = (value & 1);
        } else if (offset == 0x590) {
            fb_stride[1] = value;
        } else if (offset == 0x568) {
            fb_bot_addr = value;
        } else if (offset == 0x56c) {
            fb_bot_addr2 = value;
        } else if (offset == 0x470) {
            fb_format = value;
        } else if (offset == 0x570) {
            fb_bot_format = value;
        } else if (offset == 0x574) {
            fb_enable_bottom = (value & 1);
        } else {
#endif
        // Forward register write to video_core
        // TODO: Catch writes to registers unknown to video_core
        ::GPU::Write<uint32_t>(*context, offset, value);
    }
};

#ifndef __ANDROID__
// Child of CryptoPP::SHA256, the sole purpose of which is to publish some protected methods of SHA256
struct MySHA256 : CryptoPP::SHA256 {
    using Parent = IteratedHashWithStaticTransform<CryptoPP::word32, CryptoPP::BigEndian, 64, 32, CryptoPP::SHA256, 32, true>;

    /// Pointer to data to be hashed (only block-wise!)
    CryptoPP::word32* DataBuf() { return Parent::DataBuf(); }
    /// Pointer to running hash
    CryptoPP::word32* StateBuf() { return Parent::StateBuf(); }

    using IteratedHashBase = CryptoPP::IteratedHashBase<CryptoPP::SHA256::HashWordType, CryptoPP::HashTransformation>;
};

// TODO: Remove these globals
static uint8_t hash_data[0x40]; // data to be hashed
static uint8_t running_hash[0x20];
MySHA256 hash;
uint32_t hashed_data_size = 0; // size of data that went into running_hash so far

struct ByteCountHiMember {
    typedef MySHA256::HashWordType MySHA256::IteratedHashBase::*type;
    friend type get(ByteCountHiMember);
};

struct ByteCountLoMember {
    typedef MySHA256::HashWordType MySHA256::IteratedHashBase::*type;
    friend type get(ByteCountLoMember);
};

/**
 * Helper class used to access the private m_countHi and m_countLo data
 * members of CryptoPP::SHA256.
 * Yes, accessing private data members is possible.
 * And yes, this is indeed ridiculously ugly - but a necessary evil, unless
 * we want to require people to install a custom CryptoPP version.
 */
template<typename Tag, typename Tag::type M>
struct CryptoPPPrivateDataMembersWorkaround {
    friend typename Tag::type get(Tag) {
        return M;
    }
};

template struct CryptoPPPrivateDataMembersWorkaround<ByteCountHiMember, &MySHA256::IteratedHashBase::m_countHi>;
template struct CryptoPPPrivateDataMembersWorkaround<ByteCountLoMember, &MySHA256::IteratedHashBase::m_countLo>;
#endif

struct HASH : MemoryAccessHandler {
    std::shared_ptr<spdlog::logger> logger;
    uint32_t base;

    uint32_t hash_cnt = 0;

    HASH(LogManager& log_manager, const char* log_name, uint32_t base) : logger(log_manager.RegisterLogger(log_name)), base(base) {

    }

#ifndef __ANDROID__
    uint32_t Read32(uint32_t offset) {
//         static int iter = 0;

        uint32_t ret = 0;

//         if (iter >= 2 && iter < 5)
//             ret = 1;
//         iter++;

        if (offset >= 0x40 && offset < 0x60) {
            // TODO: This may actually also be used to read the running hash!
            // The FS module does this while hashing the loaded application's ExeFS. While doing so, it is doing the following:
            // * Copy over almost the entire ExeFS onto the HASH IO registers
            // * Read the running hash and copy it to an internal buffer
            // * Finalize the hash (writing 2 to 0x10101000)
            // * Reset the HASH engine (writing 0 to 0x10101000)
            // * Restore the copy of the *running* (non-final!) hash by writing it to the range starting at 0x10101040
            // * Write the amount of bytes already hashed to 0x10101004 (not sure if this is relevant)
            // * Write the remaining data to the HASH IO registers and finalize hashing as usual
            boost::endian::little_uint32_t hashpart;
            memcpy(&hashpart, &running_hash[offset - 0x40], sizeof(hashpart));
            ret = hashpart;
//             static int lol =0;
//             lol++;
//             ret = lol;

            // TODO: Assert this is only read when there is no pending (incomplete) block
        } else if (offset == 0x4) {
//             if ((hashed_data_size % 0x40) != 0)
//                 throw std::runtime_error("hashed_data size somehow got unaligned!");

            // NOTE: data size gets updated every 0x40 hashed bytes. It's unknown what happens if you write a non-0x40-byte-aligned value and try to read it back, though!
            ret = (hashed_data_size / 0x40) * 0x40;
//             ret = 0xdeadbeef;
//             ret = 0x20;
//            ret = hashed_data_size;

        } else if (offset == 0x0) {
            ret = hash_cnt;
        } else {
            throw std::runtime_error("Read from unknown HASH reg");
        }
        logger->warn("Read from HASH register {:#010x} {:#x}-> {:#010x}", base + offset, offset, ret);

        return ret;
    }

    void Write8(uint32_t offset, uint8_t value) {
        WriteImpl(offset, value);
    }

    void Write16(uint32_t offset, uint16_t value) {
        WriteImpl(offset, value);
    }

    void Write32(uint32_t offset, uint32_t value) {
        WriteImpl(offset, value);
    }

    template<typename T>
    void WriteImpl(uint32_t offset, T value) {
        if (base == 0x10301000) {
            if (offset < 0x40) {
                std::conditional_t<Meta::is_same_v<T, uint32_t>,
//                                 boost::endian::big_uint32_t,
                                boost::endian::little_uint32_t,
                                std::conditional_t<Meta::is_same_v<T, uint16_t>,
//                                                     boost::endian::big_uint16_t,
                                                    boost::endian::little_uint16_t,
                                                    uint8_t>> be_value = value;
                memcpy(&hash_data[offset], &be_value, sizeof(be_value));

                // TODO: It seems that the hashed data size (as reported in MMIO reads) will only get updated in 0x40 chunks! Our current code needs the byte-precise information though, so we just keep updating this and return the cropped version when reading i back
                hashed_data_size += sizeof(T);

                if (offset == 0x40 - sizeof(T)) {
                    hash.Update(hash_data, sizeof(hash_data));
                    // TODOTODO: Use Transform instead!

                    memset(hash_data, 0, sizeof(hash_data));

//                     logger->warn("Written to final hash data register after {:#x} bytes, updating hash", hashed_data_size);

                    if (!hash.StateBuf())
                        throw std::runtime_error("bla2");

                    // TODO: Endianness? I think StateBuf is a uint32_t pointer...
                    memcpy(running_hash, hash.StateBuf(), sizeof(running_hash));
                }
            } else if (offset >= 0x40 && offset < 0x60) {
                throw std::runtime_error("bla");
            } else {
                throw std::runtime_error("Unknown HASH register");
            }
        } else if (offset == 4) {
            if (sizeof(T) != 4)
                throw std::runtime_error("Only 32-bit writes supported in this code path");

            // TODOTEST: When writing a non-0x40-byte aligned value, will the lower bits be chopped off on read back?
            hashed_data_size = value;

            // NOTE: This code is an ugly hack (see the definition of
            //       "CryptoPPPrivateDataMembersWorkaround" above) to access
            //       some data members of CryptoPP::SHA256 even though they
            //       are private. This is necessary to restore a given state
            //       as required to implement the HASH registers properly.
            hash.*get(ByteCountHiMember()) = 0;
            hash.*get(ByteCountLoMember()) = value;
            logger->warn("Resetting HASH byte count to {:#x}", value);
        } else {
            if (offset == 0) {
                // TODO: Assert that we are in big-endian hashing mode!

                if (sizeof(T) != 4)
                    throw std::runtime_error("Only 32-bit writes supported in this code path");

                if (value & 1) {
//                     static int lol = 0;
                    memset(hash_data, 0, sizeof(hash_data));
                    memset(running_hash, 0, sizeof(running_hash));
                    hash = decltype(hash){};
                    hashed_data_size = 0;

                    // TODO: Should we instead keep hash_data, running_hash, and hashed_data_size at their prior values?
                    logger->warn("Reset HASH engine");
                }

                if (value & 2) {
                    // Hash pending data, if any
                    if (hashed_data_size % 0x40) {
                        hash.Update(hash_data, hashed_data_size % 0x40);
                    }
                    logger->warn("Finalizing HASH engine after {:#x} bytes", hashed_data_size);
                    hash.Final(running_hash);
                }
                hash_cnt = value & 0xfffffffc;
            } else if (offset >= 0x40 && offset < 0x60) {
                if (sizeof(T) != 4)
                    throw std::runtime_error("Only 32-bit writes supported in this code path");

//                 boost::endian::big_uint32_t be_val = value;
                boost::endian::little_uint32_t be_val = value;
                memcpy(&running_hash[offset - 0x40], &be_val, sizeof(be_val));
                memcpy(reinterpret_cast<char*>(hash.StateBuf()) + (offset - 0x40), &be_val, sizeof(be_val));

                logger->warn("Write to HASH register {:#010x} {:#x}<- {:#010x}", base, offset, value);
            }
        }
        logger->warn("{}-bit write to HASH register {:#010x} {:#x}<- {:#010x}", sizeof(T) * 8, base, offset, value);
    }
#endif // ANDROID
};

struct GPIO : MemoryAccessHandler {
    std::shared_ptr<spdlog::logger> logger;
    uint32_t unknown0x20 = 0;
    uint32_t unknown0x24 = 0;

    GPIO(LogManager& log_manager) : logger(log_manager.RegisterLogger("IO_GPIO")) {

    }

    uint16_t Read16(uint32_t offset) {
        switch (offset) {
        // Used e.g. by HID
        case 0x0:
            logger->warn("16-bit read from stubbed GPIO register 0x0");
            return 0;

        case 0x14:
            logger->warn("16-bit read from stubbed GPIO register 0x14");
            return 0xFFFF;

        // Used e.g. by HID
        case 0x28:
            logger->warn("16-bit read from stubbed GPIO register 0x28");
            return 0;

        default:
            throw std::runtime_error(fmt::format("16-bit read from unknown GPIO register {:#x}", offset));
        }
    }

    uint32_t Read32(uint32_t offset) {
        switch (offset) {
        // Used by codec via gpio
        case 0x10:
            logger->warn("32-bit read from stubbed GPIO register 0x10");
            return 0;

        // Used e.g. by HID
        case 0x20:
            logger->warn("32-bit read from stubbed GPIO register 0x20");
            return unknown0x20;

        case 0x24:
            logger->warn("32-bit read from stubbed GPIO register 0x24");
            return unknown0x24;

        default:
            throw std::runtime_error(fmt::format("32-bit read from unknown GPIO register {:#x}", offset));
        }
    }

    void Write16(uint32_t offset, uint16_t value) {
        switch (offset) {
        case 0x14:
            logger->warn("16-bit write of value {:#x} to stubbed GPIO register 0x14", value);
            return;

        case 0x28:
            logger->warn("16-bit write of value {:#x} to stubbed GPIO register 0x28", value);
            return;

        default:
            throw std::runtime_error(fmt::format("16-bit write to unknown GPIO register {:#x}", offset));
        }
    }

    void Write32(uint32_t offset, uint32_t value) {
        switch (offset) {
        // Used by codec via gpio
        case 0x10:
            logger->warn("32-bit write of value {:#x} to stubbed GPIO register 0x10", value);
            break;

        // Used e.g. by HID
        case 0x20:
            logger->warn("32-bit write of value {:#x} to stubbed GPIO register 0x20", value);
            unknown0x20 = value;
            break;

        case 0x24:
            logger->warn("32-bit write of value {:#x} to stubbed GPIO register 0x24", value);
            unknown0x24 = value;
            break;

        default:
            throw std::runtime_error(fmt::format("32-bit write to unknown GPIO register {:#x}", offset));
        }
    }
};

struct SPI : MemoryAccessHandler {
    std::shared_ptr<spdlog::logger> logger;

    uint32_t unknown_cnt_0x800 = 0;

    // TODO: Change to bus number
    uint32_t base;

    bool read_user_offset = false;

    SPI(LogManager& log_manager, const char* log_name, uint32_t base_) : logger(log_manager.RegisterLogger(log_name)), base(base_) {

    }

    uint8_t Read8(uint32_t offset) {
        switch (offset) {
//         case 1:
//             logger->warn("Stubbed read from unknown SPI register 0x1");
//             return 0;

        default:
            throw std::runtime_error(fmt::format("8-bit read from unknown SPI register {:#x} @ {:#x}", offset, offset + base));
        }
    }

    uint32_t Read32(uint32_t offset) {
        switch (offset) {
        case 0x800:
            return unknown_cnt_0x800;

        case 0x80c:
            if (read_user_offset) {
                read_user_offset = false;
                return 0xacacaca;
            }

            if (base == 0x10160000) {
                // TODO: Device 1 (wifi) only. LLE CFG waits for bit 1 to be set in the return value
                return 2;
            }
            [[fallthrough]];
//        case 0x810:
//            return 2;

        default:
//             throw std::runtime_error(fmt::format("32-bit read from unknown SPI register {:#x}", offset));
            logger->warn("Stubbed 32-bit read from unknown SPI register {:#x} @ {:#x}", offset, offset + base);
            return 0;
        }
    }

    void Write32(uint32_t offset, uint32_t value) {
        switch (offset) {
        case 0x800:
        {
            // Force the "busy" flag to be clear
            unknown_cnt_0x800 = value & 0xffff7fff;
            logger->warn("Stubbed 32-bit write of value {:#x} to SPI control register {:#x} @ {:#x}", value, offset, offset + base);
            break;
        }

        case 0x80c:
            // TODO: Wifi device (bus 0, devid 1) only
            if (value == 0x20000003) {
                // read user offset next
                read_user_offset = true;
            }

            [[fallthrough]];

        default:
            logger->warn("Stubbed 32-bit write of value {:#x} to unknown SPI register {:#x} @ {:#x}", value, offset, offset + base);
//             throw std::runtime_error(fmt::format("32-bit write to unknown SPI register {:#x}", offset));
        }
    }
};

struct CONFIG11 : MemoryAccessHandler {
    std::shared_ptr<spdlog::logger> logger;

    // TODO: Is this still needed?
    Teakra::Teakra *teakra = new Teakra::Teakra(Teakra::CreateInterpreterEngine);

    uint16_t unknown0x1c0 = 0;

    CONFIG11(LogManager& log_manager) : logger(log_manager.RegisterLogger("IO_CONFIG11")) {
        g_teakra = teakra;
    }

    uint8_t Read8(uint32_t offset) {
        switch (offset) {
        // Used by the dsp module
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x3:
        case 0x4:
        case 0x5:
        case 0x6:
        case 0x7:
            if (offset >= 0x80000) {
                throw Mikage::Exceptions::Invalid("Out-of-bounds read from DSP program memory");
            }
            logger->warn("8-bit read from DSP program memory offset {:#x}", offset);
            return 0; //teakra.ProgramRead(offset);

        case 0x8:
        case 0x9:
        case 0xa:
        case 0xb:
        case 0xc:
        case 0xd:
        case 0xe:
        case 0xf:
            if (offset >= 0x80000) {
                throw Mikage::Exceptions::Invalid("Out-of-bounds read from DSP data memory");
            }
            logger->warn("8-bit read from DSP data memory offset {:#x}", offset);
            return 0; //teakra.DataRead(offset);

        case 0x10c: // Read by NWM during boot
        case 0x180: // WIFICNT. Used by NWM during boot
            logger->warn("Stubbed 8-bit read from unknown CONFIG11 register {:#x}", offset);
            return 0;

//         case 1:
//             logger->warn("Stubbed 8-bit read from unknown CONFIG11 register 0x1");
//             return 0;

        // Used by codec (via pdn:i) during boot
        case 0x1220:
            logger->warn("Stubbed 8-bit read from unknown CONFIG11 register 0x1220");
            return 0;

        default:
            throw std::runtime_error(fmt::format("8-bit read from unknown CONFIG11 register at offset {:#x}", offset));
        }
    }

    uint32_t Read16(uint32_t offset) {
        switch (offset) {
        case 0x1c0:
            return unknown0x1c0;

        default:
            throw std::runtime_error(fmt::format("16-bit read from unknown CONFIG11 register at offset {:#x}", offset));
        }
    }

    uint32_t Read32(uint32_t offset) {
        switch (offset) {
        default:
            throw std::runtime_error(fmt::format("32-bit read from unknown CONFIG11 register at offset {:#x}", offset));
        }
    }

    void Write8(uint32_t offset, uint8_t value) {
        switch (offset) {
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x3:
        case 0x4:
        case 0x5:
        case 0x6:
        case 0x7:
            if (offset >= 0x80000) {
                throw Mikage::Exceptions::Invalid("Out-of-bounds write to DSP program memory");
            }
            logger->warn("8-bit write of value {:#x} to DSP program memory offset {:#x}", value, offset);
//            teakra.ProgramWrite(offset, value);
            break;

        case 0x8:
        case 0x9:
        case 0xa:
        case 0xb:
        case 0xc:
        case 0xd:
        case 0xe:
        case 0xf:
            if (offset >= 0x80000) {
                throw Mikage::Exceptions::Invalid("Out-of-bounds write to DSP data memory");
            }
            logger->warn("8-bit write of value {:#x} to DSP data memory offset {:#x}", value, offset);
//            teakra.DataWrite(offset, value);
            break;

        case 0x10c: // Used by NWM during boot
        case 0x180: // WIFICNT. Used by NWM during boot
            logger->warn("Stubbed 8-bit write of value {:#x} to unknown CONFIG11 register {:#x}", value, offset);
            break;

        // Used by codec (via pdn:i) during boot to set bit1
        case 0x1220:
            logger->warn("Stubbed 8-bit write of value {:#x} to unknown CONFIG11 register 0x1220", value);
            break;

        default:
            throw std::runtime_error(fmt::format("8-bit write to unknown CONFIG11 register at offset {:#x}", offset));
        }
    }

    void Write16(uint32_t offset, uint16_t value) {
        switch (offset) {
        case 0x1c0:
            unknown0x1c0 = value;
            return;

//         case 0x2:
//             logger->warn("Stubbed 16-bit write of value {:#x} to unknown CONFIG11 register 0x2", value);
//             break;
//
//         case 0x4:
//             logger->warn("Stubbed 16-bit write of value {:#x} to unknown CONFIG11 register 0x4", value);
//             break;

        default:
            throw std::runtime_error(fmt::format("16-bit write to unknown CONFIG11 register at offset {:#x}", offset));
        }
    }

    void Write32(uint32_t offset, uint32_t value) {
        switch (offset) {
        case 0x1200:
            // GPU related; used by gsp via pdn
            logger->warn("Stubbed 32-bit write of value {:#x} to unknown CONFIG11 register 0x1200", value);
            break;

        default:
            throw std::runtime_error(fmt::format("32-bit write to unknown CONFIG11 register at offset {:#x}", offset));
        }
    }
};

struct DSPMemory : MemoryAccessHandler {
    DSPMemory() {

    }

    uint8_t Read8(uint32_t offset) {
        return g_teakra->GetDspMemory()[offset];
    }

    uint16_t Read16(uint32_t offset) {
        return *(uint16_t*)&g_teakra->GetDspMemory()[offset];
    }

    uint32_t Read32(uint32_t offset) {
        return *(uint32_t*)&g_teakra->GetDspMemory()[offset];
    }

    void Write8(uint32_t offset, uint8_t value) {
        *(uint8_t*)&g_teakra->GetDspMemory()[offset] = value;
    }

    void Write16(uint32_t offset, uint16_t value) {
        *(uint16_t*)&g_teakra->GetDspMemory()[offset] = value;
    }

    void Write32(uint32_t offset, uint32_t value) {
        *(uint32_t*)&g_teakra->GetDspMemory()[offset] = value;
    }
};

struct DSPConfig {
    uint16_t raw;

    auto trigger_reset() const { return BitField::v3::MakeFlagOn<0>(&raw); }

    auto auto_inc_addr() const { return BitField::v3::MakeFlagOn<1>(&raw); }
    auto read_length_raw() const { return BitField::v3::MakeFieldOn<2, 2>(&raw); }

    auto trigger_read() const { return BitField::v3::MakeFlagOn<4>(&raw); }

    // 0 = Transfer from DSP memory, 1 = Transfer MMIO, 5 = Transfer program memory
    auto fifo_transfer_region() const { return BitField::v3::MakeFieldOn<12, 4>(&raw); }

};

struct DSPStatus {
    uint16_t raw;

    auto reading_from_dsp_memory() const { return BitField::v3::MakeFlagOn<0>(this); }
    auto writing_to_dsp_memory() const { return BitField::v3::MakeFlagOn<1>(this); }

    // Bit 2: Expected to be zero once reset complete

    auto read_fifo_full() const { return BitField::v3::MakeFlagOn<5>(this); }
    auto read_fifo_has_data() const { return BitField::v3::MakeFlagOn<6>(this); }
    auto write_fifo_full() const { return BitField::v3::MakeFlagOn<7>(this); }
    auto write_fifo_empty() const { return BitField::v3::MakeFlagOn<8>(this); }

    // 3dbrew erratum: Value 1 indicates the DSP has sent a reply
    auto reply_ready_mask() const { return BitField::v3::MakeFieldOn<10, 3>(this); }
    bool reply_ready(int channel) const {
        return reply_ready_mask() & (1 << channel);
    }

    // TODO: Rename. This is actually inverted, maybe?
    auto command_sent_mask() const { return BitField::v3::MakeFieldOn<13, 3>(this); }
};

static bool just_reset = false;

struct DSPMMIO : MemoryAccessHandler {
    std::shared_ptr<spdlog::logger> logger;

    DSPConfig config {};
    DSPStatus status { DSPStatus{}.write_fifo_empty()(true) };

    uint16_t semaphore = 0;
    uint16_t semaphore_mask = 0;

    // FIFO offset in 16-bit words
    uint16_t fifo_offset = 0;
    std::optional<uint32_t> read_fifo_remaining = { 0 }; // If empty, unlimited reading is enabled

    DSPMMIO(LogManager& log_manager, const char* log_name) : logger(log_manager.RegisterLogger(log_name)) {
    }

    uint8_t Read8(uint32_t offset) {
        switch (offset) {
        default:
            throw std::runtime_error(fmt::format("8-bit read from unknown DSP register {:#x}", offset));
        }
    }

    void SyncStatusFromTeakra() {
        auto new_reply_ready_mask = g_teakra->RecvDataIsReady(0) | (g_teakra->RecvDataIsReady(1) << 1) | (g_teakra->RecvDataIsReady(2) << 2);
        auto new_command_sent_mask = (!g_teakra->SendDataIsEmpty(0)) | (!g_teakra->SendDataIsEmpty(1) << 1) | (!g_teakra->SendDataIsEmpty(2) << 2);
        status = status.reply_ready_mask()(new_reply_ready_mask).command_sent_mask()(new_command_sent_mask);
    }

    uint16_t Read16(uint32_t offset) {
        switch (offset) {
        case 0x0:
        {
            logger->warn("Stubbed 16-bit read from unknown DSP register {:#x}", offset);

            if (config.fifo_transfer_region() != 0) {
                throw Mikage::Exceptions::NotImplemented("FIFO read from region other than DSP memory not implemented");
            }

            if (read_fifo_remaining == 0) {
                throw Mikage::Exceptions::Invalid("Trying to read empty DSP FIFO");
            }

            if (g_teakra->DMAChan0GetDstHigh()) {
                throw Mikage::Exceptions::NotImplemented("Unsupported DSP read using upper 16 bits");
            }

            auto value = g_teakra->DataRead(fifo_offset, true);

            if (read_fifo_remaining) {
                if (--*read_fifo_remaining == 0) {
                    status = status.read_fifo_has_data()(false).reading_from_dsp_memory()(false);
                }
            }

            if (config.auto_inc_addr()) {
                ++fifo_offset;
                if (fifo_offset == 0) {
                    throw Mikage::Exceptions::NotImplemented("Overflow of read address for DSP FIFO");
                }
            }

            return value;
        }

        case 0x8:
            logger->warn("16-bit read from DSP config register {:#x} -> {:#x}", offset, config.raw);
            return config.raw;

        case 0xc:
        {
            // Query current status from Teakra before reporting to the application
            SyncStatusFromTeakra();
            logger->warn("16-bit read from DSP status register {:#x} -> {:#x}", offset, status.raw);
            return status.raw;
        }

        case 0x10:
        {
            logger->warn("16-bit read from DSP semaphore register: {:#x}", semaphore);
            semaphore = g_teakra->GetSemaphore(); // TODO: Needed for DSP LLE to boot, but actually this should be the semaphore for the other APBP direction...
            return semaphore;
        }

        case 0x14:
        {
            logger->warn("16-bit read from DSP semaphore mask register: {:#x}", semaphore_mask);
            return semaphore_mask;
        }

        case 0x1c:
        {
            auto sema = g_teakra->GetSemaphore();
            logger->warn("16-bit read from DSP semaphore register: {:#x}", sema);
            return sema;
        }

        case 0x24:
        case 0x2c:
        case 0x34:
        {
            auto channel = (offset - 0x24) / 8;
            auto ret = g_teakra->RecvData(channel);
            logger->warn("16-bit read from DSP command register {:#x} -> {:#x}", offset, ret);
            if (!just_reset && ret != 1) {
                // If we just reset, run DSP for a bit after the DSP module queried the pipe locations.
                // TODO: Drop this workaround. It's currently needed to avoid issues booting (e.g. in Home Menu or Super Mario 3D Land)
                g_teakra->Run(16384);
                g_teakra->Run(16384);
                g_teakra->Run(16384);
                g_teakra->Run(16384);
                g_teakra->Run(16384);
                g_teakra->Run(16384);
                g_teakra->Run(16384);
//                g_teakra->Run(4384);
                just_reset = true;
            }
            return ret;
        }

        default:
            throw std::runtime_error(fmt::format("16-bit read from unknown DSP register {:#x}", offset));
        }
    }

    uint32_t Read32(uint32_t offset) {
        switch (offset) {
        default:
            throw std::runtime_error(fmt::format("32-bit read from unknown DSP register {:#x}", offset));
        }
    }

    void Write16(uint32_t offset, uint16_t value) {
        switch (offset) {
        case 0x0:
        {
            logger->warn("Stubbed 16-bit write of value {:#x} unknown DSP register {:#x}", value, offset);

            if (config.fifo_transfer_region() != 0) {
                throw Mikage::Exceptions::NotImplemented("FIFO write to region other than DSP memory not implemented");
            }

            if (g_teakra->DMAChan0GetDstHigh()) {
                throw Mikage::Exceptions::NotImplemented("Unsupported DSP write using upper 16 bits");
            }

            g_teakra->DataWrite(fifo_offset, value, true);

            if (config.auto_inc_addr()) {
                ++fifo_offset;
                if (fifo_offset == 0) {
                    throw Mikage::Exceptions::NotImplemented("Overflow of write address for DSP FIFO");
                }
            }
            break;
        }

        case 0x4:
            logger->warn("16-bit write to DSP PADR register: {:#x}", value);
            fifo_offset = value;
            break;

        case 0x8:
        {
            logger->warn("16-bit write to DSP config register: {:#x}", value);
            auto prev_config = config;
            config.raw = value;

            if (config.trigger_reset() && !prev_config.trigger_reset()) {
                g_dsp_running = false;
                g_dsp_just_reset = false;
            }

            if (!config.trigger_reset() && prev_config.trigger_reset()) {
                std::vector<std::uint8_t> temp(0x80000);
                memcpy(temp.data(), g_teakra->GetDspMemory().data(), temp.size());
                delete g_teakra;
                g_teakra = new Teakra::Teakra(Teakra::CreateInterpreterEngine);
                g_teakra->Reset();
                memcpy(g_teakra->GetDspMemory().data(), temp.data(), temp.size());
                just_reset = false;

                HLE::OS::SetupOSDSPCallbacks(*HLE::OS::g_os);

                semaphore = 0;
                semaphore_mask = 0;
                read_fifo_remaining = 0;
                status = DSPStatus{}.write_fifo_empty()(true);

                static std::ofstream ofs("samples.raw", std::ios_base::binary);
                static std::ofstream ofs2("samples2.raw", std::ios_base::binary);
                static std::vector<uint16_t> data;
                static std::vector<uint16_t> data2;
                static auto audio_callback = [](std::array<int16_t, 2> samples) {
                    data.push_back(samples[0]);
                    data2.push_back(samples[1]);
                    if (data.size() == 0x10000) {
                        ofs.write((char*)data.data(), data.size() * 2);
                        data.clear();
                    }
                    if (data2.size() == 0x10000) {
                        ofs2.write((char*)data2.data(), data2.size() * 2);
                        data2.clear();
                    }

                    TeakraAudioCallback(samples);
                };
                g_teakra->SetAudioCallback(audio_callback);
//                g_teakra->SetAudioCallback(TeakraAudioCallback);

                // NOTE: These don't seem to be needed (only for unaligned memory accesses?)
                static Teakra::AHBMCallback ahbm;
                ahbm.read8 = [](uint32_t addr) -> uint8_t {
                    return ReadLegacy<uint8_t>(*g_mem, addr);
                };
                ahbm.read16 = [](uint32_t addr) -> uint16_t {
                    return ReadLegacy<uint16_t>(*g_mem, addr);
                };
                ahbm.read32 = [](uint32_t addr) -> uint32_t {
                    return ReadLegacy<uint32_t>(*g_mem, addr);
                };
                ahbm.write8 = [](uint32_t addr, uint8_t value) {
                    return WriteLegacy(*g_mem, addr, value);
                };
                ahbm.write16 = [](uint32_t addr, uint16_t value) {
                    return WriteLegacy(*g_mem, addr, value);
                };
                ahbm.write32 = [](uint32_t addr, uint32_t value) {
                    return WriteLegacy(*g_mem, addr, value);
                };
                g_teakra->SetAHBMCallback(ahbm);

                g_dsp_running = true; // TODO: Remove
                g_dsp_just_reset = true;
            }

            if (config.trigger_read()) {
                status = status.read_fifo_has_data()(true).reading_from_dsp_memory()(true);

                // TODO: Shouldn't change while trigger_read is true
                const uint32_t fifo_sizes[] = { 1, 8, 16 };
                if (config.read_length_raw() < 3) {
                    read_fifo_remaining = fifo_sizes[config.read_length_raw()];
                } else {
                    // Unlimited reading
                    read_fifo_remaining.reset();
                }
            } else {
                read_fifo_remaining = 0;
                status = status.read_fifo_has_data()(false).reading_from_dsp_memory()(false);
            }

            break;
        }

        case 0x10:
            // NOTE: Teakra internally applies the semaphore mask
            logger->warn("16-bit write to DSP semaphore set register: {:#x}", value);
            g_teakra->SetSemaphore(value);
            semaphore = value;
            break;

        case 0x14:
            logger->warn("16-bit write to DSP semaphore mask register: {:#x}", value);
            g_teakra->MaskSemaphore(value);
            semaphore_mask = value;
            break;

        case 0x18:
            // NOTE: Teakra internally applies the semaphore mask
            logger->warn("16-bit write to DSP semaphore clear register: {:#x}", value);
            g_teakra->ClearSemaphore(value);
            if (g_teakra->GetSemaphore() == 0) {
                status.raw &= ~(1 << 9);
            }
            break;

        case 0x20:
        case 0x28:
        case 0x30:
        {
            auto channel = (offset - 0x20) / 8;
            logger->warn("16-bit write to DSP command register {:#x} <- {:#x}", offset, value);
            g_teakra->SendData(channel, value);
            break;
        }

        default:
            throw std::runtime_error(fmt::format("16-bit write to unknown DSP register {:#x} (value {:#x})", offset, value));
        }
    }

    void Write32(uint32_t offset, uint32_t value) {
        switch (offset) {
        default:
            throw std::runtime_error(fmt::format("32-bit write to unknown DSP register {:#x}", offset));
        }
    }
};

template<typename T, uint32_t PAddrStart, uint32_t Size>
uint8_t ProxyBus<T, PAddrStart, Size>::Read8(uint32_t address) {
    return handler->Read8(address - PAddrStart);
}

template<typename T, uint32_t PAddrStart, uint32_t Size>
uint16_t ProxyBus<T, PAddrStart, Size>::Read16(uint32_t address) {
    return handler->Read16(address - PAddrStart);
}

template<typename T, uint32_t PAddrStart, uint32_t Size>
uint32_t ProxyBus<T, PAddrStart, Size>::Read32(uint32_t address) {
    return handler->Read32(address - PAddrStart);
}

template<typename T, uint32_t PAddrStart, uint32_t Size>
void ProxyBus<T, PAddrStart, Size>::Write8(uint32_t address, uint8_t value) {
    handler->Write8(address - PAddrStart, value);
}

template<typename T, uint32_t PAddrStart, uint32_t Size>
void ProxyBus<T, PAddrStart, Size>::Write16(uint32_t address, uint16_t value) {
    handler->Write16(address - PAddrStart, value);
}

template<typename T, uint32_t PAddrStart, uint32_t Size>
void ProxyBus<T, PAddrStart, Size>::Write32(uint32_t address, uint32_t value) {
    handler->Write32(address - PAddrStart, value);
}

template<typename T, uint32_t PAddrStart, uint32_t Size>
ProxyBus<T, PAddrStart, Size>::ProxyBus(std::unique_ptr<T> handler) : handler(std::move(handler)) {
    static_assert(std::is_base_of<MemoryAccessHandler, T>::value, "Handler types must be child classes of MemoryAccessHandler");
}

template<typename T, uint32_t PAddrStart, uint32_t Size>
ProxyBus<T, PAddrStart, Size>::ProxyBus(ProxyBus&& bus) {
    handler = std::move(bus.handler);
}

template<typename T, uint32_t PAddrStart, uint32_t Size>
ProxyBus<T, PAddrStart, Size>::~ProxyBus() {
}

template<uint32_t PAddrStart, uint32_t Size>
uint8_t Bus<PAddrStart,Size>::Read8(uint32_t address) {
    throw std::runtime_error(fmt::format("Unimplemented read8 from address {:#010x} (bus {:#x}-{:#x})",
                                         address, PAddrStart, PAddrStart + Size));
}

template<uint32_t PAddrStart, uint32_t Size>
uint16_t Bus<PAddrStart,Size>::Read16(uint32_t address) {
    throw std::runtime_error(fmt::format("Unimplemented read16 from address {:#010x} (bus {:#x}-{:#x})",
                                         address, PAddrStart, PAddrStart + Size));
}

template<uint32_t PAddrStart, uint32_t Size>
uint32_t Bus<PAddrStart,Size>::Read32(uint32_t address) {
    throw std::runtime_error(fmt::format("Unimplemented read32 from address {:#010x} (bus {:#x}-{:#x})",
                                         address, PAddrStart, PAddrStart + Size));
}

template<uint32_t PAddrStart, uint32_t Size>
void Bus<PAddrStart,Size>::Write8(uint32_t address, uint8_t value) {
    throw std::runtime_error(fmt::format("Unimplemented 8-bit write of {:#04x} to address {:#010x} (bus {:#x}-{:#x})",
                                         value, address, PAddrStart, PAddrStart + Size));
}

template<uint32_t PAddrStart, uint32_t Size>
void Bus<PAddrStart,Size>::Write16(uint32_t address, uint16_t value) {
    throw std::runtime_error(fmt::format("Unimplemented 16-bit write of {:#06x} to address {:#010x} (bus {:#x}-{:#x})",
                                         value, address, PAddrStart, PAddrStart + Size));
}

template<uint32_t PAddrStart, uint32_t Size>
void Bus<PAddrStart,Size>::Write32(uint32_t address, uint32_t value) {
    throw std::runtime_error(fmt::format("Unimplemented 32-bit write of {:#10x} to address {:#010x} (bus {:#x}-{:#x})",
                                         value, address, PAddrStart, PAddrStart + Size));
}

uint8_t MemoryAccessHandler::Read8(uint32_t offset) {
    throw std::runtime_error(fmt::format("Unimplemented read8 from offset {:#010x}", offset));
}

uint16_t MemoryAccessHandler::Read16(uint32_t offset) {
    throw std::runtime_error(fmt::format("Unimplemented read16 from offset {:#010x}", offset));
}

uint32_t MemoryAccessHandler::Read32(uint32_t offset) {
    throw std::runtime_error(fmt::format("Unimplemented read32 from offset {:#010x}", offset));
}

void MemoryAccessHandler::Write8(uint32_t offset, uint8_t value) {
    throw std::runtime_error(fmt::format("Unimplemented 8-bit write of {:#04x} to offset {:#010x}", value, offset));
}

void MemoryAccessHandler::Write16(uint32_t offset, uint16_t value) {
    throw std::runtime_error(fmt::format("Unimplemented 16-bit write of {:#06x} to offset {:#010x}", value, offset));
}

void MemoryAccessHandler::Write32(uint32_t offset, uint32_t value) {
    throw std::runtime_error(fmt::format("Unimplemented 32-bit write of {:#10x} to offset {:#010x}", value, offset));
}

// TODO: Stop creating temporary FCRAM pointers and instead use just FCRAM{}
PhysicalMemory::PhysicalMemory(LogManager& log_manager)
    // Large memory blocks like FCRAM are explicitly heap-allocated here
    : memory(std::move(*std::make_unique<FCRAM>()), std::move(*std::make_unique<VRAM>()),
            DSP(std::make_unique<DSPMemory>()),
             std::move(*std::make_unique<AXI>()), IO_HID(std::make_unique<HID>(log_manager)),
             IO_LCD(std::make_unique<LCD>(log_manager)),
             IO_AXI(std::make_unique<AXIHandler>(log_manager)),
             IO_GPU(std::make_unique<GPU>(log_manager)),
             IO_HASH(std::make_unique<HASH>(log_manager, "IO_HASH_1", 0x10101000)),
             IO_CONFIG11(std::make_unique<CONFIG11>(log_manager)),
             IO_DSP1(std::make_unique<DSPMMIO>(log_manager, "IO_DSP_1")),
             IO_SPIBUS2(std::make_unique<SPI>(log_manager, "IO_SPI_2", 0x10142000)),
             IO_SPIBUS3(std::make_unique<SPI>(log_manager, "IO_SPI_3", 0x10143000)),
             IO_HASH2(std::make_unique<HASH>(log_manager, "IO_HASH_2", 0x10301000)),
             IO_GPIO(std::make_unique<GPIO>(log_manager)),
             IO_SPIBUS1(std::make_unique<SPI>(log_manager, "IO_SPI_1", 0x10160000)),
             IO_DSP2(std::make_unique<DSPMMIO>(log_manager, "IO_DSP_2")),
             MPCorePrivateBus(std::make_unique<MPCorePrivate>())) {
    g_mem = this;
}

void PhysicalMemory::InjectDependency(PicaContext& context) {
    std::get<IO_GPU>(memory).handler->context = context.context.get();
}

void PhysicalMemory::InjectDependency(InputSource& input) {
    std::get<IO_HID>(memory).handler->input = &input;
}

// Instantiate templates explicitly since the handler structures are only
// defined in here to reduce build times
template struct ProxyBus<DSPMemory,  0x1ff00000, 0x80000>;
template struct ProxyBus<HASH,       0x10101000, 0x1000>;
template struct ProxyBus<DSPMMIO,    0x10103000, 0x1000>;
template struct ProxyBus<CONFIG11,   0x10140000, 0x2000>;
template struct ProxyBus<SPI,        0x10142000, 0x1000>;
template struct ProxyBus<SPI,        0x10143000, 0x1000>;
template struct ProxyBus<HID,        0x10146000, 0x1000>;
template struct ProxyBus<GPIO,       0x10147000, 0x1000>;
template struct ProxyBus<SPI,        0x10160000, 0x1000>;
template struct ProxyBus<LCD,        0x10202000, 0x1000>;
template struct ProxyBus<DSPMMIO,    0x10203000, 0x1000>;
template struct ProxyBus<AXIHandler, 0x1020F000, 0x1000>;
template struct ProxyBus<HASH,       0x10301000, 0x1000>;
template struct ProxyBus<GPU,        0x10400000, 0x2000>;

template struct ProxyBus<MPCorePrivate, 0x17e00000, 0x00002000>;


template<HookKind Kind, typename Bus>
static auto& GetHookList(Bus& bus) {
    if constexpr (Kind == HookKind::Write) {
        return bus.write_hooks;
    } else {
        return bus.read_hooks;
    }
}

template<typename Bus>
static bool HasAnyHooks(Bus& bus, std::size_t page_index) {
    return (bus.write_hooks[page_index] || bus.read_hooks[page_index]);
}

template<HookKind Kind>
void SetHook(PhysicalMemory& mem, PAddr start, uint32_t num_bytes, HookHandler<Kind>& hook) {
    auto hook_kind_str = HasReadHook(Kind) ? HasWriteHook(Kind) ? "ReadWrite" : "Read" : "Write";

//fprintf(stderr, "HOOK: Setting %#x-%#x (%s)\n", start, start + num_bytes, hook_kind_str);

    ValidateContract(start < start + num_bytes);
    auto callback = [&](auto& bus) mutable {
        if (!IsInside{start}(bus)) {
            return false;
        }

        uint32_t page = (start - bus.start) >> 12;
        uint32_t end_page = (start - bus.start + 0x1000 + num_bytes - 1) >> 12;
        for (; page < end_page; ++page) {
            const bool already_unbacked = HasAnyHooks(bus, page);

            std::unique_ptr<HookBase>& bus_hook = GetHookList<Kind>(bus)[page];
            // Search the last bus hook before this range start and the first after
            HookBase* before = nullptr;
            HookBase* after = bus_hook.get();
            while (after && after->range.start < start) {
                before = after;
                after = after->next.get();
            }

            if (before && before->range.start + before->range.num_bytes > start) {
                throw std::runtime_error(fmt::format(   "Attempted to set {}Hook at {:#x}-{:#x} overlapping with existing hook at {:#x}-{:#x}",
                                                        hook_kind_str, start, start + num_bytes, before->range.start, before->range.start + before->range.num_bytes));
            }

            if (after && after->range.start < start + num_bytes) {
                throw std::runtime_error(fmt::format(   "Attempted to set {}Hook at {:#x}-{:#x} overlapping with existing hook at {:#x}-{:#x}",
                                                        hook_kind_str, start, start + num_bytes, after->range.start, after->range.start + after->range.num_bytes));
            }

            if (before && before->range.start + before->range.num_bytes == start) {
                // Extend existing entry
                before->range.num_bytes += num_bytes;

                // Check if all three ranges can be merged
                if (after && after->range.start == start + num_bytes) {
                    auto after_bytes = after->range.num_bytes;
                    before->next = std::move(after->next);
                    before->range.num_bytes += after_bytes;
                }
            } else if (after && after->range.start == start + num_bytes) {
                // Extend existing entry
                after->range.start -= num_bytes;
                after->range.num_bytes += num_bytes;
            } else {
                // Add new entry
                auto hook_start = std::max(start, page << 12);
                auto new_bus_hook = std::make_unique<Hook<Kind>>(MemoryRange { hook_start, std::min(bus.start + (page + 1) * 0x1000, start + num_bytes) - hook_start }, hook);
                if (before) {
                    new_bus_hook->next = std::move(before->next);
                    before->next = std::move(new_bus_hook);
                } else {
                    new_bus_hook->next = std::move(bus_hook);
                    bus_hook = std::move(new_bus_hook);
                }
            }

            // Unback memory page unless it already is unbacked due to setting a ReadHook nor WriteHook before
            if (!already_unbacked) {
                for (auto& subscriber : mem.subscribers) {
                    subscriber->OnUnbackedByHostMemory((page << 12) + bus.start);
                }
            }
        }

        return true;
    };
    detail::ForEachMemoryBus(mem.memory, callback);
}
template void SetHook<HookKind::Read>(PhysicalMemory&, PAddr, uint32_t, ReadHandler&);
template void SetHook<HookKind::Write>(PhysicalMemory&, PAddr, uint32_t, WriteHandler&);

static bool Overlaps(const MemoryRange& range, PAddr start, uint32_t num_bytes) {
    return !(range.start + range.num_bytes <= start || start + num_bytes <= range.start);
}

static bool Overlaps(const MemoryRange& range, const MemoryRange& other) {
    return Overlaps(range, other.start, other.num_bytes);
}

void ClearHook(PhysicalMemory& mem, HookKind kind, PAddr start, uint32_t num_bytes) {
//fprintf(stderr, "HOOK: Clearing %#x-%#x\n", start, start + num_bytes);
    auto callback = [&](auto& bus) mutable {
        if (!IsInside{start}(bus)) {
            return false;
        }

        uint32_t page = (start - bus.start) >> 12;
        uint32_t end_page = (start - bus.start + 0x1000 + num_bytes - 1) >> 12;
        for (; page < end_page; ++page) {
            auto clear = [&]<HookKind Kind>(std::unique_ptr<HookBase>* bus_hook, const char* name, Hook<Kind>* /* for template argument inference only */) {
                if (!*bus_hook) {
                    throw std::runtime_error(fmt::format("Attempted to clear {}Hook from a page that had none set", name));
                }

                while (*bus_hook) {
                    if (Overlaps(bus_hook->get()->range, MemoryRange { start, num_bytes })) {
                        // TODO: Assert they share the same handler

                        if (bus_hook->get()->range.start < start && bus_hook->get()->range.start + bus_hook->get()->range.num_bytes > start + num_bytes) {
                            // TODO: Need to split the hook into two separate ones
                            auto after_range = Memory::MemoryRange { start + num_bytes, bus_hook->get()->range.start + bus_hook->get()->range.num_bytes - (start + num_bytes) };
                            bus_hook->get()->range.num_bytes = start - bus_hook->get()->range.start;

                            auto new_bus_hook = std::make_unique<Hook<Kind>>(after_range, static_cast<Hook<Kind>*>(bus_hook->get())->handler);
                            new_bus_hook->next = std::move(bus_hook->get()->next);
                            bus_hook->get()->next = std::move(new_bus_hook);
                        } else if (bus_hook->get()->range.start < start) {
                            // Shorten the hook memory range
                            bus_hook->get()->range.num_bytes = start - bus_hook->get()->range.start;
                            ValidateContract(bus_hook->get()->range.start + bus_hook->get()->range.num_bytes == start);
                            bus_hook = &bus_hook->get()->next;
                        } else if (bus_hook->get()->range.start + bus_hook->get()->range.num_bytes > start + num_bytes) {
                            // Push the hook memory range back to the end of the cleared range
                            auto diff = start + num_bytes - bus_hook->get()->range.start;
                            bus_hook->get()->range.start += diff;
                            bus_hook->get()->range.num_bytes -= diff;
                            ValidateContract(bus_hook->get()->range.start == start + num_bytes);
                            bus_hook = &bus_hook->get()->next;
                        } else {
                            // Clear the entire hook
                            *bus_hook = std::move(bus_hook->get()->next);
                        }
                    } else {
                        bus_hook = &bus_hook->get()->next;
                    }
                }
            };

            if (HasReadHook(kind)) {
                clear(&GetHookList<HookKind::Read>(bus)[page], "Read", (Hook<HookKind::Read>*)nullptr);
            }

            if (HasWriteHook(kind)) {
                clear(&GetHookList<HookKind::Write>(bus)[page], "Write", (Hook<HookKind::Write>*)nullptr);
            }

            // Back memory page again if neither ReadHook nor WriteHook are set
            if (!HasAnyHooks(bus, page)) {
                for (auto& subscriber : mem.subscribers) {
                    subscriber->OnBackedByHostMemory(detail::GetMemoryBackedPageFor(bus, (page << 12) + bus.start), (page << 12) + bus.start);
                }
            }
        }

        return true;
    };
    detail::ForEachMemoryBus(mem.memory, callback);
}

// Deprecated since this doesn't check for or trigger any memory hooks
HostMemoryBackedPages LookupContiguousMemoryBackedPage(PhysicalMemory& mem, PAddr address, uint32_t num_bytes) {
    ValidateContract(num_bytes != 0);

    HostMemoryBackedPage out_page = { nullptr };
    auto callback = [&](auto& bus) {
        if (IsInside{address}(bus)) {
            if (address + num_bytes > bus.end) {
                throw std::runtime_error("Requested contiguous memory region that extends past available physical memory");
            }

            int32_t last_page_index = ((address & 0xfff) + num_bytes - 1) >> 12;
            int32_t page_index = 0;
            // Avoid testing pages that aren't fully covered, since they may have hooks for other memory
            // TODO: Find a cleaner way to handle this
            if ((address + num_bytes) & 0xfff) {
                --last_page_index;
            }
            if (address & 0xfff) {
                ++page_index;
            }

            for (; page_index <= last_page_index; ++page_index) {
                PAddr page_start = ((address >> 12) + page_index) << 12;

                if (bus.LookupReadHookFor(page_start) || bus.LookupWriteHookFor(page_start)) {
                    // Disabled for now as VertexLoader is hitting this path (e.g. when transitioning from title screen to menu in TLoZ: Ocarina of Time 3D)
//                    throw std::runtime_error(fmt::format(   "Requested contiguous memory region {:#x}-{:#x} that covers read/write hooks",
//                                                            address, address + num_bytes));
                }
            }

            out_page = detail::GetMemoryBackedPageFor(bus, address);
            return true;
        }

        return false;
    };

    detail::ForEachMemoryBus(mem.memory, callback);

    if (out_page.data == nullptr) {
        throw std::runtime_error(fmt::format(   "Requested invalid physical memory region {:#x}-{:#x}",
                                                address, address + num_bytes));
    }

    return { out_page.data, num_bytes };
}

template<HookKind AccessMode>
HostMemoryBackedPages LookupContiguousMemoryBackedPage(PhysicalMemory& mem, PAddr address, uint32_t num_bytes) {
    ValidateContract(num_bytes != 0);

//fprintf(stderr, "HOOK: Requesting contiguous memory region %#x-%#x\n", address, address + num_bytes);

    HostMemoryBackedPage out_page = { nullptr };
    auto callback = [&](auto& bus) {
        if (IsInside{address}(bus)) {
            if (address + num_bytes > bus.end) {
                throw std::runtime_error("Requested contiguous memory region that extends past available physical memory");
            }

            int32_t last_page_index = ((address & 0xfff) + num_bytes - 1) >> 12;
            int32_t page_index = 0;
            // Avoid testing pages that aren't fully covered, since they may have hooks for other memory
            // TODO: Find a cleaner way to handle this
            if ((address + num_bytes) & 0xfff) {
//                --last_page_index;
            }
            if (address & 0xfff) {
//                ++page_index;
            }

            for (; page_index <= last_page_index; ++page_index) {
                PAddr page_start = ((address >> 12) + page_index) << 12;

                if constexpr (HasReadHook(AccessMode)) {
                    for (HookBase* hook = bus.LookupReadHookFor(page_start); hook; hook = hook->next.get()) {
                        if (Overlaps(hook->range, MemoryRange { address, num_bytes })) {
                            throw std::runtime_error(fmt::format(   "Requested contiguous memory region {:#x}-{:#x} covers read hooks starting at {:#x}",
                                                                    address, address + num_bytes, page_start));
                        }
                    }
                }
                if constexpr (HasWriteHook(AccessMode)) {
                    for (HookBase* hook = bus.LookupWriteHookFor(page_start); hook; hook = hook->next.get()) {
                        if (Overlaps(hook->range, MemoryRange { address, num_bytes })) {
                            throw std::runtime_error(fmt::format(   "Requested contiguous memory region {:#x}-{:#x} covers write hooks starting at {:#x}",
                                                                    address, address + num_bytes, page_start));
                        }
                    }
                }
            }

            out_page = detail::GetMemoryBackedPageFor(bus, address);
            return true;
        }

        return false;
    };

    detail::ForEachMemoryBus(mem.memory, callback);

    if (out_page.data == nullptr) {
        throw std::runtime_error(fmt::format(   "Requested invalid physical memory region {:#x}-{:#x}",
                                                address, address + num_bytes));
    }

    return { out_page.data, num_bytes };
}

template HostMemoryBackedPages LookupContiguousMemoryBackedPage<HookKind::Read>(PhysicalMemory&, PAddr, uint32_t);
template HostMemoryBackedPages LookupContiguousMemoryBackedPage<HookKind::Write>(PhysicalMemory&, PAddr, uint32_t);
template HostMemoryBackedPages LookupContiguousMemoryBackedPage<HookKind::ReadWrite>(PhysicalMemory&, PAddr, uint32_t);

} // namespace Memory
