#include <framework/formats.hpp>
#include <framework/meta_tools.hpp>
#include <platform/file_formats/3dsx.hpp>
#include <platform/file_formats/cia.hpp>
#include <platform/file_formats/ncch.hpp>

#include <cryptopp/osrng.h>
#include <cryptopp/rsa.h>

#include <range/v3/range_for.hpp>
#include <range/v3/algorithm/copy.hpp>
#include <range/v3/algorithm/equal.hpp>
#include <range/v3/algorithm/fill.hpp>
#include <range/v3/algorithm/transform.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/slice.hpp>
#include <range/v3/view/zip.hpp>

#include <boost/hana/functional/overload.hpp>

#include <boost/program_options.hpp>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <fstream>
#include <sstream>

// Helper used solely for implementing a custom boost::program_options validator that can read hex numbers
struct HexUint64 {
    uint64_t val;

    operator uint64_t () {
        return val;
    }
};

// Enable to have written data fields updated automatically where necessary (e.g. hashes, offsets, sizes)
#define UPDATE_FIELDS

#define PATCH_TITLEID
#ifdef PATCH_TITLEID
HexUint64 alt_titleid = { 0x000400000F80FE00 };
#endif

template<typename StreamOff>
static void ZeroUpTo(std::ostream& file, StreamOff off) {
    assert(off >= file.tellp());
    std::vector<char> zeroes(off - file.tellp(), 0);
    file.write(zeroes.data(), zeroes.size());
}

template<typename IntType>
static IntType RoundToNextMediaUnit(IntType value) {
    auto media_unit_mask = IntType{0x1ff};
    value += media_unit_mask;
    value -= value & media_unit_mask;
    return value;
}

/**
 * Fill the file with zeroes such that the difference between the given
 * base stream position and the new write position is a multiple of the
 * media unit size.
 */
template<typename StreamPos>
static void ZeroUpToNextMediaUnitFrom(std::ostream& file, StreamPos base) {
    // TODO: Narrowing cast..
    auto diff = file.tellp() - static_cast<decltype(file.tellp())>(base);
    diff = RoundToNextMediaUnit(diff);
    ZeroUpTo(file, base + diff);
}

template<typename T, typename SubType, typename Stream>
T ParseSignedData(Stream& stream) {
    auto sig = FileFormat::Signature { FileFormat::Load<boost::endian::big_uint32_t>(stream) };

    sig.data.resize(FileFormat::GetSignatureSize(sig.type));
    stream.Read(reinterpret_cast<char*>(sig.data.data()), sig.data.size() * sizeof(sig.data[0]));
    size_t pad_size;
    switch (sig.type) {
    case 0x10000:
    case 0x10001:
    case 0x10003:
    case 0x10004:
        pad_size = 0x3c;
        break;

    case 0x10002:
    case 0x10005:
        pad_size = 0x40;
        break;

    default:
        throw std::runtime_error("Unknown signature type " + std::to_string(sig.type));
    }
    std::vector<char> zeroes(pad_size, 0);
    stream.Read(zeroes.data(), zeroes.size());

    return T { sig, FileFormat::Load<SubType>(stream) };
}

template<typename T>
void SaveSignedData(FileFormat::Signature sig, const T& data, std::ostream& stream) {
    auto begin = (unsigned long long)stream.tellp();
    FileFormat::Save<boost::endian::big_uint32_t>(sig.type, stream);

    stream.write(reinterpret_cast<const char*>(sig.data.data()), sig.data.size() * sizeof(sig.data[0]));

    // TODO: Pad to 0x40 bytes for any type of signature, remove placeholder code
  //  auto padded_data_begin = begin + (unsigned long long){((unsigned long long)stream.tellp() - begin + 63) & ~UINT32_C(63)};

//    ZeroUpTo(stream, padded_data_begin);
    size_t pad_size;
    switch (sig.type) {
    case 0x10000:
    case 0x10001:
    case 0x10003:
    case 0x10004:
        pad_size = 0x3c;
        break;

    case 0x10002:
    case 0x10005:
        pad_size = 0x40;
        break;

    default:
        throw std::runtime_error("Unknown signature type " + std::to_string(sig.type));
    }
    std::vector<char> zeroes(pad_size, 0);
    stream.write(zeroes.data(), zeroes.size());

    FileFormat::Save(data, stream);
}

struct CIA {
    FileFormat::CIAHeader header;

    std::vector<uint8_t> content_mask = std::vector<uint8_t>(0x2000);

    std::array<FileFormat::Certificate, 3> certificates;
    FileFormat::Ticket ticket;
    FileFormat::TMD tmd;

    struct NCCH {
        FileFormat::NCCHHeader header;

        FileFormat::ExHeader exheader;

        struct PlainData {
            std::vector<uint8_t> data;
        } plain_data;

        struct Logo {
            // TODO: There is an actual structure to this.. define it!
            std::vector<uint8_t> data;
        } logo;

        struct ExeFS {
            FileFormat::ExeFSHeader header;

            struct Section {
                std::vector<uint8_t> data;
            } sections[Meta::tuple_size_v<decltype(FileFormat::ExeFSHeader::files)>];
        } exefs;

        struct RomFS {
            std::vector<uint8_t> data;
        } romfs;
    } ncch;

    FileFormat::CIAMeta meta;
};

CIA::NCCH ParseNCCH(std::ifstream& file) {
    CIA::NCCH ret;

    // TODO: Narrowing cast!
    auto ncch_begin = (unsigned long long)(file.tellg());

    auto strbuf_it = std::istreambuf_iterator<char>(file.rdbuf());
    auto stream = FileFormat::MakeStreamInFromContainer(strbuf_it, decltype(strbuf_it){});

    ret.header = FileFormat::Load<FileFormat::NCCHHeader>(stream);
    auto& ncch = ret.header;

    std::cout << "Header magic: " << (char)ncch.magic[0] << (char)ncch.magic[1] << (char)ncch.magic[2] << (char)ncch.magic[3] << std::endl;
    std::cout << "ExHeader size: 0x" << std::hex << ncch.exheader_size << " bytes" << std::endl;
    std::cout << "Plain data size: 0x" << std::hex << ncch.plain_data_size.ToBytes() << " bytes (@ 0x" << ncch.plain_data_offset.ToBytes() << ")" << std::endl;
    std::cout << "Logo size: 0x" << std::hex << ncch.logo_size.ToBytes() << " bytes (@ 0x" << ncch.logo_offset.ToBytes() << ")" << std::endl;
    std::cout << "ExeFS size: 0x" << std::hex
              << ncch.exefs_size.ToBytes() << " bytes (@ 0x"
              << ncch.exefs_offset.ToBytes() << "; 0x"
              << ncch.exefs_hash_message_size.ToBytes() << " bytes hashed)" << std::endl;
    std::cout << "RomFS size: 0x" << std::hex
              << ncch.romfs_size.ToBytes() << " bytes (@ 0x"
              << ncch.romfs_offset.ToBytes() << "; 0x"
              << ncch.romfs_hash_message_size.ToBytes() << " bytes hashed)" << std::endl;
    std::cout << "Partition id: 0x" << std::hex << ncch.partition_id << std::endl;
    std::cout << "Program id: 0x" << std::hex << ncch.program_id << std::endl;
    std::cout << "Encryption method: 0x" << std::hex << +ncch.crypto_method << std::endl;
    std::cout << "Platform: 0x" << std::hex << +ncch.platform << std::endl;
    std::cout << "Type mask: 0x" << std::hex << +ncch.type_mask << std::endl;
    std::cout << "Unit size: 0x" << std::hex << (0x200 * (UINT32_C(1) << ncch.unit_size_log2)) << " (0x200 * (1 << 0x" << +ncch.unit_size_log2 << "))" << std::endl;
    std::cout << "Flags: 0x" << std::hex << +ncch.flags << std::endl;

    for (size_t index = 0; index < ncch.unknown.size(); ++index) {
        if (ncch.unknown[index]) {
            std::cout << "Unknown @ " << std::dec << index << ": 0x" << std::hex << +ncch.unknown[index] << std::endl;
        }
    }
    for (size_t index = 0; index < ncch.unknown2a.size(); ++index) {
        if (ncch.unknown2a[index]) {
            std::cout << "Unknown2 @ " << std::dec << index << ": 0x" << std::hex << +ncch.unknown2a[index] << std::endl;
        }
    }
    for (size_t index = 0; index < ncch.unknown2b.size(); ++index) {
        if (ncch.unknown2b[index]) {
            std::cout << "Unknown2 @ " << std::dec << index << ": 0x" << std::hex << +ncch.unknown2b[index] << std::endl;
        }
    }
    for (size_t index = 0; index < ncch.unknown3.size(); ++index) {
        if (ncch.unknown3[index]) {
            std::cout << "Unknown3 @ " << std::dec << index << ": 0x" << std::hex << +ncch.unknown3[index] << std::endl;
        }
    }
    for (size_t index = 0; index < ncch.unknown4.size(); ++index) {
        if (ncch.unknown4[index]) {
            std::cout << "Unknown4 @ " << std::dec << index << ": 0x" << std::hex << +ncch.unknown4[index] << std::endl;
        }
    }
    for (size_t index = 0; index < ncch.unknown5.size(); ++index) {
        if (ncch.unknown5[index]) {
            std::cout << "Unknown5 @ " << std::dec << index << ": 0x" << std::hex << +ncch.unknown5[index] << std::endl;
        }
    }

    // TODO: Check NCCH header flags before reading the exheader
    auto exheader_begin = file.tellg();
    const auto exheader_data = Meta::invoke([&] {
        // NOTE: The given header_size only refers to the hashed exheader region rather than the full exheader size
        std::vector<uint8_t> data(FileFormat::ExHeader::Tags::expected_serialized_size);
        assert(data.size() >= ret.header.exheader_size);
        stream.Read(reinterpret_cast<char*>(data.data()), data.size());
        return data;
    });
    auto exheader_data_stream = FileFormat::MakeStreamInFromContainer(exheader_data.begin(), exheader_data.end());
    ret.exheader = FileFormat::Load<FileFormat::ExHeader>(exheader_data_stream);
    auto& exheader = ret.exheader;

    std::cout << "\nExtended Header\n";
    {
    std::cout << "Reference hash: ";
    for (auto c : ncch.exheader_sha256)
        std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
    std::cout << std::endl;

    CryptoPP::byte exheader_hash[CryptoPP::SHA256::DIGESTSIZE];
    CryptoPP::SHA256().CalculateDigest(exheader_hash,
                                       reinterpret_cast<const CryptoPP::byte*>(exheader_data.data()), ncch.exheader_size);
    std::cout << "Computed hash:  ";
    for (auto c : exheader_hash)
        std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
    std::cout << std::endl;
    }

    std::cout << "Application title: \"";
    for (auto c : exheader.application_title) {
        if (c == 0)
            break;
        std::cout << c;
    }
    std::cout << "\"" << std::endl;

    std::cout << "Remaster version: 0x" << std::hex << exheader.remaster_version << std::endl;
    auto print_codesetinfo = [](const FileFormat::ExHeader::CodeSetInfo& codeset) {
        std::cout << "Address: 0x" << std::hex << codeset.address << '\n';
        std::cout << "Size (pages): 0x" << std::hex << codeset.size_pages << '\n';
        std::cout << "Size (bytes): 0x" << std::hex << codeset.size_bytes << std::endl;
    };
    std::cout << "Text section:" << '\n';
    print_codesetinfo(exheader.section_text);
    std::cout << "Ro section:" << '\n';
    print_codesetinfo(exheader.section_ro);
    std::cout << "Data section (without bss):" << '\n';
    print_codesetinfo(exheader.section_data);
    std::cout << "Stack size: 0x" << std::hex << exheader.stack_size << std::endl;
    std::cout << "bss size: 0x" << std::hex << exheader.bss_size << std::endl;
    std::cout << "Save data size: 0x" << std::hex << exheader.save_data_size << std::endl;
    std::cout << "Jump id: 0x" << std::hex << exheader.jump_id << std::endl;

    for (auto& title_id : exheader.dependencies) {
        if (!title_id)
            continue;

        std::cout << "Dependency: " << std::hex << std::setw(16) << std::setfill('0') << title_id << std::endl;
    }

    for (size_t index = 0; index < exheader.unknown.size(); ++index) {
        if (exheader.unknown[index]) {
            std::cout << "Unknown @ " << std::dec << index << ": 0x" << std::hex << +exheader.unknown[index] << std::endl;
        }
    }
    for (size_t index = 0; index < exheader.unknown3.size(); ++index) {
        if (exheader.unknown3[index]) {
            std::cout << "Unknown3 @ " << std::dec << index << ": 0x" << std::hex << +exheader.unknown3[index] << std::endl;
        }
    }
    for (size_t index = 0; index < exheader.unknown4.size(); ++index) {
        if (exheader.unknown4[index]) {
            std::cout << "Unknown4 @ " << std::dec << index << ": 0x" << std::hex << +exheader.unknown4[index] << std::endl;
        }
    }

    std::cout << "Access Control Info:" << std::endl;
    std::cout << "Program id: 0x" << std::hex << exheader.aci.program_id << " (limit: 0x" << exheader.aci_limits.program_id << ")" << std::endl;
    std::cout << "ACI version: 0x" << std::hex << exheader.aci.version << " (limit: 0x" << exheader.aci_limits.version << ")" << std::endl;
//     std::cout << "Flag0: 0x" << std::hex << exheader.aci.flag0 << " (limit: 0x" << exheader.aci_limits.flag0 << ")" << std::endl;
//     std::cout << "Flag1: 0x" << std::hex << exheader.aci.flag1 << " (limit: 0x" << exheader.aci_limits.flag1 << ")" << std::endl;
//     std::cout << "Flag2: 0x" << std::hex << exheader.aci.flag2 << " (limit: 0x" << exheader.aci_limits.flag2 << ")" << std::endl;
//     std::cout << "Default priority: 0x" << std::hex << exheader.aci.priority << " (limit: 0x" << exheader.aci_limits.priority << ")" << std::endl;
    std::cout << "Flag0: 0x" << std::hex << exheader.aci.flags.flag0()() << " (limit: 0x" << exheader.aci_limits.flags.flag0()() << ")" << std::endl;
    std::cout << "Flag1: 0x" << std::hex << exheader.aci.flags.flag1()() << " (limit: 0x" << exheader.aci_limits.flags.flag1()() << ")" << std::endl;
    std::cout << "Flag2: 0x" << std::hex << exheader.aci.flags.flag2()() << " (limit: 0x" << exheader.aci_limits.flags.flag2()() << ")" << std::endl;
    std::cout << "Default priority: 0x" << std::hex << exheader.aci.flags.priority()() << " (limit: 0x" << exheader.aci_limits.flags.priority()() << ")" << std::endl;
    for (size_t index = 0; index < exheader.aci.unknown5.size(); ++index) {
        if (exheader.aci.unknown5[index] || exheader.aci_limits.unknown5[index]) {
            std::cout << "Unknown5 @ " << std::dec << index << ": 0x" << std::hex << +exheader.aci.unknown5[index]
                      << " (limit: 0x" << +exheader.aci_limits.unknown5[index] << ")" << std::endl;
        }
    }
    std::cout << "ARM11 kernel capabilities:" << std::endl;
    for (auto cap : exheader.aci.arm11_kernel_capabilities) {
        cap.visit(boost::hana::overload(
            [](uint32_t unknown) { if (unknown != 0xffffffff) std::cout << "Unknown value: 0x" << std::hex << unknown << std::endl; },
            [](FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::SVCMaskTableEntry svc_mask) {
                std::cout << "SVC access " << std::dec << svc_mask.entry_index()() << ": 0x" << std::hex << svc_mask.mask()() << std::endl;
            },
            [](FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::HandleTableInfo handle_table) {
                std::cout << "Handle table size: 0x" << std::hex << handle_table.num_handles()() << " handles" << std::endl;
            },
            [](FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::MappedMemoryPage mapped_page) {
                std::cout << "Mapped page at virtual address: 0x"
                          << std::hex << std::setw(8) << std::setfill('0') << (uint64_t{mapped_page.page_index()()} << 12) << std::endl;
            },
            [](FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::MappedMemoryRange mapped_range_limit) {
                std::cout << "Mapped memory limit at virtual address: 0x"
                          << std::hex << std::setw(8) << std::setfill('0') << (uint64_t{mapped_range_limit.page_index()()} << 12)
                          << " (R" << (mapped_range_limit.read_only()() ? ")" : "W)") << std::endl;
            },
            [](FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::KernelFlags flags) {
                const char* NoYes[] = { "No", "Yes" };
                std::cout << "Kernel flags:" << std::endl;
                std::cout << "\tAllow debug: " << NoYes[flags.allow_debug()()] << std::endl;
                std::cout << "\tForce debug: " << NoYes[flags.force_debug()()] << std::endl;
                std::cout << "\tUnknown2: " << NoYes[flags.unknown2()()] << std::endl;
                std::cout << "\tShared page writeable: " << NoYes[flags.shared_page_writeable()()] << std::endl;
                std::cout << "\tUnknown4: " << NoYes[flags.unknown4()()] << std::endl;
                std::cout << "\tUnknown5: " << NoYes[flags.unknown5()()] << std::endl;
                std::cout << "\tUnknown6: " << NoYes[flags.unknown6()()] << std::endl;
                std::cout << "\tUnknown7: " << NoYes[flags.unknown7()()] << std::endl;
                std::cout << "\tMemory type: 0x" << flags.memory_type()() << std::endl;
                std::cout << "\tNonstandard code address: " << NoYes[flags.nonstandard_code_address()()] << std::endl;
                std::cout << "\tEnable second CPU core: " << NoYes[flags.enable_second_cpu_core()()] << std::endl;
            }
        ));
    }

    for (size_t index = 0; index < exheader.aci.unknown6.size(); ++index) {
        if (exheader.aci.unknown6[index] || exheader.aci_limits.unknown6[index]) {
            std::cout << "Unknown6 @ " << std::dec << index << ": 0x" << std::hex << +exheader.aci.unknown6[index]
                      << " (limit: 0x" << +exheader.aci_limits.unknown6[index] << ")" << std::endl;
        }
    }
    for (size_t index = 0; index < exheader.aci.unknown7.size(); ++index) {
        if (exheader.aci.unknown7[index] || exheader.aci_limits.unknown7[index]) {
            std::cout << "Unknown7 @ " << std::dec << index << ": 0x" << std::hex << +exheader.aci.unknown7[index]
                      << " (limit: 0x" << +exheader.aci_limits.unknown7[index] << ")" << std::endl;
        }
    }

    std::cout << "Service access:" << std::endl;
    for (auto service_pair : ranges::view::zip(exheader.aci.service_access_list, exheader.aci_limits.service_access_list)) {
        auto&& service = std::get<0>(service_pair);
        auto&& service_limit = std::get<1>(service_pair);
        if (service[0] == 0 && service[1] == 0)
            continue;

        std::cout << "\"";
        for (auto c : service) {
            if (c == 0)
                break;
            std::cout << c;
        }
        std::cout << "\" (limit: \"";
        for (auto c : service_limit) {
            if (c == 0)
                break;
            std::cout << c;
        }
        std::cout << "\")";
        std::cout << std::endl;
    }
//    for (auto&& kernel_caps_pair : ranges::view::zip(exheader.aci.arm11_kernel_capabilities, exheader.aci_limits.arm11_kernel_capabilities)) {
//        std::cout << "Kernel capabilities: 0x" << std::hex << std::get<0>(kernel_caps_pair)
//                  << " (limit: 0x" << std::get<1>(kernel_caps_pair) << ")" << std::endl;
//    }


    auto plain_data_begin = ncch_begin + (unsigned long long){ncch.plain_data_offset.ToBytes()};
    file.seekg(plain_data_begin);
    auto& plain_data = ret.plain_data.data;
    plain_data.resize(ncch.plain_data_size.ToBytes());
    file.read(reinterpret_cast<char*>(plain_data.data()), plain_data.size());

    auto logo_begin = ncch_begin + (unsigned long long){ncch.logo_offset.ToBytes()};
    file.seekg(logo_begin);
    auto& logo = ret.logo.data;
    logo.resize(ncch.logo_size.ToBytes());
    file.read(reinterpret_cast<char*>(logo.data()), logo.size());

    {
    std::cout << "Reference logo hash: ";
    for (auto c : ncch.logo_sha256)
        std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
    std::cout << std::endl;

    CryptoPP::byte logo_hash[CryptoPP::SHA256::DIGESTSIZE];
    CryptoPP::SHA256().CalculateDigest(logo_hash,
                                       reinterpret_cast<const CryptoPP::byte*>(logo.data()), logo.size());
    std::cout << "Computed logo hash:  ";
    for (auto c : logo_hash)
        std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
    std::cout << std::endl;
    }


    auto exefs_begin = ncch_begin + (unsigned long long){ncch.exefs_offset.ToBytes()};
    file.seekg(exefs_begin);

    CryptoPP::SHA256 exefs_hash;
    CryptoPP::byte digest[CryptoPP::SHA256::DIGESTSIZE];
    std::vector<unsigned char> data;
    data.resize(ncch.exefs_hash_message_size.ToBytes());
    file.read(reinterpret_cast<char*>(&data[0]), data.size());
    exefs_hash.Update(data.data(), data.size());
    exefs_hash.Final(digest);
    std::cout << "ExeFS Hash, 0x" << std::hex << ncch.exefs_hash_message_size.ToBytes() << " bytes:" << std::endl;
    for (auto c : digest)
        std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
    std::cout << std::endl;
    for (auto c : ncch.exefs_sha256)
        std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
    std::cout << std::endl;
    file.seekg(exefs_begin);

    strbuf_it = std::istreambuf_iterator<char>(file.rdbuf());
    stream = FileFormat::MakeStreamInFromContainer(strbuf_it, decltype(strbuf_it){});

    ret.exefs.header = FileFormat::Load<FileFormat::ExeFSHeader>(stream);
    // TODO: Narrowing cast!
    auto exefs_end = (unsigned long long)(file.tellg());
    auto& exefs = ret.exefs.header;
    for (size_t section_index = 0; section_index < exefs.files.size(); ++section_index) {
        const auto& exefs_section = exefs.files[section_index];

        std::cout << "ExeFS section " << std::dec << section_index << ": \"";
        for (auto c : exefs_section.name) {
            if (c == 0)
                break;
            std::cout << c;
        }
        std::cout << "\"" << std::endl;

        // TODO: Early abort if size is zero!
        auto section_offset = exefs_end + (unsigned long long){exefs_section.offset};
        file.seekg(section_offset);

        // TODO: Cleanup
        ret.exefs.sections[section_index].data.resize(exefs_section.size_bytes);
        file.read(reinterpret_cast<char*>(ret.exefs.sections[section_index].data.data()), exefs_section.size_bytes);

        // Update section hash
        //auto& section_hash = *(exefs.header.hashes.rbegin() + section_index);
        //CryptoPP::SHA256().CalculateDigest(reinterpret_cast<CryptoPP::byte*>(section_hash.data()),
        //                                   reinterpret_cast<const CryptoPP::byte*>(section_data.data()), section_data.size());


#ifdef TODOSECTIONHASH // d0k3 suggested this hash is not actually valid for p9?
        CryptoPP::SHA256 exefs_hash;
        CryptoPP::byte digest[CryptoPP::SHA256::DIGESTSIZE];
        std::vector<unsigned char> data;
        data.resize(ncch.exefs_hash_message_size.ToBytes());
        file.read(reinterpret_cast<char*>(&data[0]), data.size());
        exefs_hash.Update(data.data(), data.size());
        exefs_hash.Final(digest);
        std::cout << "ExeFS Hash, 0x" << std::hex << ncch.exefs_hash_message_size.ToBytes() << " bytes:" << std::endl;
        for (auto c : digest)
            std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
#endif
    }

    auto romfs_begin = ncch_begin + (unsigned long long){ncch.romfs_offset.ToBytes()};
    file.seekg(romfs_begin);

    auto& romfs_data = ret.romfs.data;
    romfs_data.resize(ncch.romfs_size.ToBytes());
    file.read(reinterpret_cast<char*>(romfs_data.data()), romfs_data.size());

    return ret;
}

CIA ParseCIA(std::ifstream& file) {
    CIA ret;

    auto cia_begin = file.tellg();

    auto strbuf_it = std::istreambuf_iterator<char>(file.rdbuf());
    auto stream = FileFormat::MakeStreamInFromContainer(strbuf_it, decltype(strbuf_it){});

    ret.header = FileFormat::Load<FileFormat::CIAHeader>(stream);
    auto& cia = ret.header;

    std::cout << std::hex << "CIA header size: 0x" << cia.header_size << std::endl;
    std::cout << "Type: 0x" << std::hex << cia.type << std::endl;
    std::cout << "Version: 0x" << std::hex << cia.version << std::endl;
    std::cout << "Certificate chain size: 0x" << std::hex << cia.certificate_chain_size << std::endl;
    std::cout << "Ticket size: 0x" << std::hex << cia.ticket_size << std::endl;
    std::cout << "TMD size: 0x" << std::hex << cia.tmd_size << std::endl;
    std::cout << "Meta size: 0x" << std::hex << cia.meta_size << std::endl;
    std::cout << "Content size: 0x" << std::hex << cia.content_size << std::endl;
    std::cout << std::endl;

    // TODO: Handle overflow cases for large content_size values..
    auto cert_begin = static_cast<unsigned long long>(cia_begin) + (unsigned long long){(cia.header_size + 63) & ~UINT32_C(63)};
    auto ticket_begin = cert_begin + (unsigned long long){(cia.certificate_chain_size + 63) & ~UINT32_C(63)};
    auto tmd_begin = ticket_begin + (unsigned long long){(cia.ticket_size + 63) & ~UINT32_C(63)};
    auto content_begin = tmd_begin + (unsigned long long){(cia.tmd_size + 63) & ~UINT32_C(63)};
    auto meta_begin = content_begin + (unsigned long long){(cia.content_size + 63) & ~UINT32_C(63)};

    file.seekg(cert_begin);
    strbuf_it = std::istreambuf_iterator<char>(file.rdbuf());
    stream = FileFormat::MakeStreamInFromContainer(strbuf_it, decltype(strbuf_it){});

    auto& certificates = ret.certificates;
    for (auto cert_index : { 0, 1, 2}) {
        auto& certificate = certificates[cert_index];
        certificate = ParseSignedData<FileFormat::Certificate, FileFormat::Certificate::Data>(stream);
        std::cout << "Certificate " << std::dec << cert_index << ":" << std::endl;
        std::cout << "Type: " << std::hex << certificate.sig.type << std::endl;
        std::cout << "Public key type: " << std::hex << certificate.cert.key_type << std::endl;

        std::cout << "Issuer: ";
        std::copy(certificate.cert.issuer.begin(), certificate.cert.issuer.end(), std::ostream_iterator<char>(std::cout));
        std::cout << std::endl;

        std::cout << "Name: ";
        std::copy(certificate.cert.name.begin(), certificate.cert.name.end(), std::ostream_iterator<char>(std::cout));
        std::cout << std::endl;

        if (certificate.cert.key_type == 0) {
            certificate.pubkey.rsa4096 = FileFormat::Load<FileFormat::CertificatePublicKey::RSAKey<4096/8>>(stream);
        } else if (certificate.cert.key_type == 1) {
            certificate.pubkey.rsa2048 = FileFormat::Load<FileFormat::CertificatePublicKey::RSAKey<2048/8>>(stream);
        } else if (certificate.cert.key_type == 1) {
            certificate.pubkey.ecc = FileFormat::Load<FileFormat::CertificatePublicKey::ECCKey>(stream);
        } else {
            throw std::runtime_error("Unknown certificate public key type");
        }

        std::cout << std::endl;
    }

    // We should usually be at this point now anyway
//    assert(file.tellg() == ticket_begin);

    file.seekg(ticket_begin);
    std::cout << "Ticket: " << std::endl;

    strbuf_it = std::istreambuf_iterator<char>(file.rdbuf());
    stream = FileFormat::MakeStreamInFromContainer(strbuf_it, decltype(strbuf_it){});

    ret.ticket = ParseSignedData<FileFormat::Ticket, FileFormat::Ticket::Data>(stream);
    auto& ticket = ret.ticket;
    std::cout << "Signature type: 0x" << std::hex << ticket.sig.type << std::endl;

    std::cout << "Issuer: ";
    std::copy(ticket.data.issuer.begin(), ticket.data.issuer.end(), std::ostream_iterator<char>(std::cout));
    std::cout << std::endl;

    std::cout << "Format version: 0x" << std::hex << +ticket.data.version << std::endl;
    std::cout << "Ticket ID: 0x" << ticket.data.ticket_id << std::endl;
    std::cout << "Console ID: 0x" << ticket.data.console_id << std::endl;
    std::cout << "Title ID: 0x" << ticket.data.title_id << std::endl;
    std::cout << "Sys Access: 0x" << ticket.data.sys_access << std::endl;
    std::cout << "Title version: 0x" << ticket.data.title_version << std::endl;
    std::cout << "Permit mask: 0x" << ticket.data.permit_mask << std::endl;
    std::cout << "Title export: 0x" << +ticket.data.title_export << std::endl;
    std::cout << "KeyY index: 0x" << +ticket.data.key_y_index << std::endl;
    for (auto i = 0; i < ticket.data.time_limit.size(); ++i) {
        if (ticket.data.time_limit[i].enable) {
            std::cout << "Time limit " << std::dec << i << ": " << ticket.data.time_limit[i].limit_in_seconds << " seconds" << std::endl;
        }
    }
    std::cout << "Unknown @ 0: 0x" << std::hex << +ticket.data.unknown[0] << std::endl;
    std::cout << "Unknown @ 1: 0x" << std::hex << +ticket.data.unknown[1] << std::endl;
    std::cout << "Unknown: 0x" << std::hex << +ticket.data.unknown2 << std::endl;
    for (size_t i = 0; i < ticket.data.unknown3.size(); ++i) {
        if (ticket.data.unknown3[i]) {
            std::cout << "Unknown3 @ "<< std::dec << i << ": 0x" << std::hex << +ticket.data.unknown3[i] << std::endl;
        }
    }
    for (size_t i = 0; i < ticket.data.unknown4.size(); ++i) {
        if (ticket.data.unknown4[i]) {
            std::cout << "Unknown4 @ "<< std::dec << i << ": 0x" << std::hex << +ticket.data.unknown4[i] << std::endl;
        }
    }
    std::cout << std::endl;

    // TMD
    std::cout << "TMD: " << std::endl;

    std::vector<char> data_buffer;
    file.seekg(tmd_begin);
    data_buffer.resize(cia.tmd_size);
    file.read(data_buffer.data(), cia.tmd_size);
    auto stream2 = FileFormat::MakeStreamInFromContainer(data_buffer.begin(), data_buffer.end());

    ret.tmd = ParseSignedData<FileFormat::TMD, FileFormat::TMD::Data>(stream2);
    auto& tmd = ret.tmd;

    std::cout << "Signature type: 0x" << std::hex << tmd.sig.type << std::endl;

    std::cout << "Issuer: ";
    std::copy(tmd.data.issuer.begin(), tmd.data.issuer.end(), std::ostream_iterator<char>(std::cout));
    std::cout << std::endl;

    std::cout << "Version: 0x" << std::hex << +tmd.data.version << std::endl;
    std::cout << "CA revocation list version: 0x" << std::hex << +tmd.data.ca_crl_version << std::endl;
    std::cout << "Signer CLR version: 0x" << std::hex << +tmd.data.ca_crl_version << std::endl;
    std::cout << "System version: 0x" << std::hex << +tmd.data.system_version << std::endl;
    std::cout << "Title id: 0x" << std::hex << +tmd.data.title_id << std::endl;
    std::cout << "Title type: 0x" << std::hex << +tmd.data.title_type << std::endl;
    std::cout << "Group id: 0x" << std::hex << +tmd.data.group_id << std::endl;
    std::cout << "Save data size: 0x" << std::hex << +tmd.data.save_data_size << std::endl;
    std::cout << "Private save data size for SRL: 0x" << std::hex << +tmd.data.srl_private_data_size << std::endl;
    std::cout << "SRL flag: 0x" << std::hex << +tmd.data.srl_flag << std::endl;
    std::cout << "Access rights: 0x" << std::hex << +tmd.data.access_rights << std::endl;
    std::cout << "Title version: 0x" << std::hex << +tmd.data.title_version << std::endl;
    std::cout << "Content count: 0x" << std::hex << +tmd.data.content_count << std::endl;
    std::cout << "Main content: 0x" << std::hex << +tmd.data.main_content << std::endl;

    std::cout << "Unknown: 0x" << std::hex << +tmd.data.unknown << std::endl;
    std::cout << "Unknown2: 0x" << std::hex << +tmd.data.unknown2 << std::endl;
    for (size_t i = 0; i < tmd.data.unknown3.size(); ++i) {
        if (tmd.data.unknown3[i]) {
            std::cout << "Unknown3 @ "<< std::dec << i << ": 0x" << std::hex << +tmd.data.unknown3[i] << std::endl;
        }
    }
    std::cout << "Unknown4: 0x" << std::hex << +tmd.data.unknown4 << std::endl;

    // Read raw content infos into memory for hashing first
    // TODO: Support more than one content!
    // TODO: Clean this up.. we are just copying data from one memory buffer into another here,
    //       when instead we could just store a pointer to the current data stream offset...
    auto content_info_hash_data = Meta::invoke([&] {
        std::vector<uint8_t> data(FileFormat::TMD::ContentInfoHash::Tags::expected_serialized_size * Meta::tuple_size_v<decltype(ret.tmd.content_info_hashes)>);
        stream2.Read(reinterpret_cast<char*>(data.data()), data.size());
        return data;
    });

    // Get content info hash ... hash
    CryptoPP::byte contentinfohash_hash[CryptoPP::SHA256::DIGESTSIZE];
    CryptoPP::SHA256().CalculateDigest(contentinfohash_hash,
                                       reinterpret_cast<const CryptoPP::byte*>(content_info_hash_data.data()), content_info_hash_data.size());

    std::cout << "Reference hash: ";
    for (auto c : ret.tmd.data.content_info_records_sha256)
        std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
    std::cout << std::endl;

    std::cout << "Computed hash:  ";
    for (auto c : contentinfohash_hash)
        std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
    std::cout << std::endl;

    auto content_info_hash_info_stream = FileFormat::MakeStreamInFromContainer(content_info_hash_data.begin(), content_info_hash_data.end());
    for (auto& content_info_hash_info : ret.tmd.content_info_hashes)
        content_info_hash_info = FileFormat::Load<FileFormat::TMD::ContentInfoHash>(content_info_hash_info_stream);

/*bool verify_sigs = false;

if (verify_sigs) {
    // TODO: Following is for sigtype==0x10004 only
    using namespace CryptoPP;
    auto&& sig_key = certificates[2].pubkey.rsa2048;
    auto n = Integer(reinterpret_cast<CryptoPP::byte*>(sig_key.modulus.data()), sig_key.modulus.size());
    auto e = Integer(reinterpret_cast<CryptoPP::byte*>(sig_key.exponent.data()), sig_key.exponent.size());
AutoSeededRandomPool rng;

//InvertibleRSAFunction params;
//params.GenerateRandomWithKeySize(rng, 2048);
RSA::PublicKey pubkey;
pubkey.Initialize(n,e);
for (auto a : sig_key.modulus)
    std::cout << +a;
std::cout << "Yo: " << n << std::endl;
std::cout << "Yo: " << e << std::endl;
std::cout << "Yo: " << pubkey.GetModulus() << std::endl;
//std::cout << "Yo: " << pubkey.GetExp() << std::endl;
    auto sig_verifier = RSASSA_PKCS1v15_SHA_Verifier(pubkey);
//    sig_verifier.AccessKey().Initialize(n, e);
//    CryptoPP::RandomNumberGenerator rng;
//auto res = pubkey.Validate(rng, 0);
    auto res = sig_verifier.AccessKey().Validate(rng, 0);
    if (!res)
        throw std::runtime_error("bla");
    auto sig_valid = sig_verifier.VerifyMessage(reinterpret_cast<CryptoPP::byte*>(data_buffer.data()) + 0x140, data_buffer.size() - 0x140, tmd.sig.data.data(), tmd.sig.data.size());
    std::cout << "Signature type: " << std::hex << tmd.sig.type << (sig_valid ? "valid" : "INVALID") << std::endl;
}*/

    // Read raw content infos into memory for hashing first
    // TODO: Support more than one content!
    // TODO: Clean this up.. we are just copying data from one memory buffer into another here,
    //       when instead we could just store a pointer to the current data stream offset...
    tmd.content_infos.resize(tmd.data.content_count);
    assert(tmd.content_infos.size() == 1);
    auto content_info_data = Meta::invoke([&] {
        std::vector<uint8_t> data(FileFormat::TMD::ContentInfo::Tags::expected_serialized_size * tmd.content_infos.size());
        stream2.Read(reinterpret_cast<char*>(data.data()), data.size());
        return data;
    });

    auto content_info_stream = FileFormat::MakeStreamInFromContainer(content_info_data.begin(), content_info_data.end());
    for (auto& content_info : tmd.content_infos) {
        content_info = FileFormat::Load<FileFormat::TMD::ContentInfo>(content_info_stream);
    }
    std::cout << std::endl;
    for (size_t index = 0; index < tmd.content_info_hashes.size(); ++index) {
        auto&& content_info_info = tmd.content_info_hashes[index];
        if (content_info_info.chunk_count == 0)
            continue;

        assert(index == 0); // TODO: Support more than one metainfo block

        std::cout << "Content metainfo " << std::dec << index << " index offset: " << std::hex << content_info_info.index_offset << std::endl;
        std::cout << "Content metainfo " << std::dec << index << " chunk count: " << std::hex << content_info_info.chunk_count << std::endl;

        // Get content info hash
        CryptoPP::SHA256 running_hash;
        CryptoPP::byte chunk_hash[running_hash.DIGESTSIZE];
        for (size_t chunk_index = 0; chunk_index < content_info_info.chunk_count; ++chunk_index) {
            auto size = FileFormat::TMD::ContentInfo::Tags::expected_serialized_size;
            auto* content_info_buffer = content_info_data.data() + chunk_index * size;
            running_hash.Update(reinterpret_cast<const CryptoPP::byte*>(content_info_buffer), size);
        }
        running_hash.Final(chunk_hash);

        std::cout << "Reference hash: ";
        for (auto c : content_info_info.sha256)
            std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
        std::cout << std::endl;

        std::cout << "Computed hash:  ";
        for (auto c : chunk_hash)
            std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
        std::cout << std::endl;
    }
    std::cout << std::endl;
    for (auto& content_info : tmd.content_infos) {
        assert(tmd.content_infos.size() == 1);

        std::cout << "Content index: " << std::dec << content_info.index << std::endl;
        std::cout << "Content id: " << std::dec << content_info.id << std::endl;
        std::cout << "Content size: 0x" << std::hex << content_info.size << std::endl;

        file.seekg(content_begin);
        std::vector<char> content_data(content_info.size);
        file.read(content_data.data(), content_data.size());

        // Get content hash
        CryptoPP::byte content_hash[CryptoPP::SHA256::DIGESTSIZE];
        CryptoPP::SHA256().CalculateDigest(content_hash, reinterpret_cast<const CryptoPP::byte*>(content_data.data()), content_data.size());

        std::cout << "Reference hash: ";
        for (auto c : content_info.sha256)
            std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
        std::cout << std::endl;

        std::cout << "Computed hash:  ";
        for (auto c : content_hash)
            std::cout << std::setw(2) << std::setfill('0') << std::hex << +c;
        std::cout << std::endl;
    }

    file.seekg(content_begin);
    std::cout << std::endl << "Content: " << std::endl;
    strbuf_it = std::istreambuf_iterator<char>(file.rdbuf());
    stream = FileFormat::MakeStreamInFromContainer(strbuf_it, decltype(strbuf_it){});
    ret.ncch = ParseNCCH(file);

    file.seekg(meta_begin);
    strbuf_it = std::istreambuf_iterator<char>(file.rdbuf());
    stream = FileFormat::MakeStreamInFromContainer(strbuf_it, decltype(strbuf_it){});
    ret.meta = FileFormat::Load<FileFormat::CIAMeta>(stream);
    for (auto& title_id : ret.meta.dependencies) {
        if (!title_id)
            continue;

        std::cout << "Dependency: " << std::hex << std::setw(16) << std::setfill('0') << title_id << std::endl;
    }

    std::cout << "Finished successfully!" << std::endl;

    return ret;
}

enum class WriteNCCHFlags {
    RecomputeOffsetsAndSizes = 0x1
};

bool HasFlag(WriteNCCHFlags flags, WriteNCCHFlags flag) {
    using IntType = std::underlying_type_t<WriteNCCHFlags>;
    return IntType{0} != (static_cast<IntType>(flags) & static_cast<IntType>(flag));
}
WriteNCCHFlags operator| (WriteNCCHFlags flags, WriteNCCHFlags mask) {
    using IntType = std::underlying_type_t<WriteNCCHFlags>;
    return static_cast<WriteNCCHFlags>(static_cast<IntType>(flags) | static_cast<IntType>(mask));
}

template<typename StreamPos>
unsigned long long NarrowCastStreamPos(StreamPos streampos) {
    return (unsigned long long)streampos;
}

static void WriteExeFS(CIA::NCCH::ExeFS& exefs, std::ostream& stream) {
    auto header_begin = stream.tellp();

    auto header_end = NarrowCastStreamPos(header_begin) + FileFormat::ExeFSHeader::Tags::expected_serialized_size;

    ZeroUpTo(stream, header_end);

    for (size_t section_index = 0; section_index < exefs.header.files.size(); ++section_index) {
        auto& exefs_section = exefs.header.files[section_index];
        auto& section_data = exefs.sections[section_index].data;

#ifdef UPDATE_FIELDS
        exefs_section.size_bytes = section_data.size();
#endif

        // TODO: Do we need to hash empty sections?
        if (exefs_section.size_bytes == 0)
            continue;

        exefs_section.offset = NarrowCastStreamPos(stream.tellp()) - header_end;
        auto section_offset = header_end + (unsigned long long){exefs_section.offset};
        assert(section_offset >= stream.tellp());
        ZeroUpTo(stream, section_offset);

        stream.write(reinterpret_cast<const char*>(section_data.data()), section_data.size());

#ifdef UPDATE_FIELDS
        // Update section hash
        auto& section_hash = *(exefs.header.hashes.rbegin() + section_index);
        CryptoPP::SHA256().CalculateDigest(reinterpret_cast<CryptoPP::byte*>(section_hash.data()),
                                           reinterpret_cast<const CryptoPP::byte*>(section_data.data()), section_data.size());
#endif
    }

    auto exefs_end = stream.tellp();

    stream.seekp(header_begin);
    FileFormat::Save<FileFormat::ExeFSHeader>(exefs.header, stream);

    stream.seekp(exefs_end);
}

static void WriteRomFS(const CIA::NCCH::RomFS& romfs, std::ostream& ostream) {
    ostream.write(reinterpret_cast<const char*>(romfs.data.data()), romfs.data.size());
}

void WriteNCCH(CIA::NCCH& ncch, std::ostream& file, WriteNCCHFlags flags) {
    auto ncch_begin = NarrowCastStreamPos(file.tellp());

    // NCCH header and extended header are written in the end when we know all offsets

    auto& header = ncch.header;

    // TODO: Check NCCH header flags before saving the exheader

    // TODO: Don't hardcode the NCCH header size here!
    auto exheader_begin = ncch_begin + FileFormat::NCCHHeader::Tags::expected_serialized_size;

#ifdef UPDATE_FIELDS
    // TODO: This isn't the full exheader size, but only part of it.
//     header.exheader_size = FileFormat::ExHeader::Tags::expected_serialized_size;
#endif

    auto exheader_end = exheader_begin + FileFormat::ExHeader::Tags::expected_serialized_size;
    ZeroUpTo(file, exheader_end);

    auto& plain_data = ncch.plain_data.data;
    if (plain_data.size()) {
#ifdef UPDATE_FIELDS
        header.plain_data_offset = FileFormat::MediaUnit32::FromBytes(RoundToNextMediaUnit(NarrowCastStreamPos(file.tellp()) - ncch_begin));
        header.plain_data_size = FileFormat::MediaUnit32::FromBytes(plain_data.size());
#endif
        auto plain_data_begin = ncch_begin + (unsigned long long){header.plain_data_offset.ToBytes()};
        ZeroUpTo(file, plain_data_begin);
        file.write(reinterpret_cast<const char*>(plain_data.data()), plain_data.size());
        ZeroUpToNextMediaUnitFrom(file, ncch_begin);
    }
    auto& logo_data = ncch.logo.data;
    if (logo_data.size()) {
#ifdef UPDATE_FIELDS
        header.logo_offset = FileFormat::MediaUnit32::FromBytes(RoundToNextMediaUnit(NarrowCastStreamPos(file.tellp()) - ncch_begin));
        header.logo_size = FileFormat::MediaUnit32::FromBytes(logo_data.size());
#endif
        auto logo_begin = ncch_begin + (unsigned long long){header.logo_offset.ToBytes()};
        ZeroUpTo(file, logo_begin);
        file.write(reinterpret_cast<const char*>(logo_data.data()), logo_data.size());
        ZeroUpToNextMediaUnitFrom(file, ncch_begin);
    }
    // TODO: May the logo be zero-sized? Do we get the correct hash for that case?
    CryptoPP::SHA256().CalculateDigest(header.logo_sha256.data(),
                                       reinterpret_cast<const CryptoPP::byte*>(logo_data.data()), logo_data.size());


    // ExeFS
#ifdef UPDATE_FIELDS
    header.exefs_offset = FileFormat::MediaUnit32::FromBytes(NarrowCastStreamPos(file.tellp()) - ncch_begin);
#endif
    auto exefs_begin = ncch_begin + (unsigned long long){header.exefs_offset.ToBytes()};

    ZeroUpTo(file, exefs_begin);
    header.exefs_offset = FileFormat::MediaUnit32::FromBytes(RoundToNextMediaUnit(NarrowCastStreamPos(file.tellp()) - ncch_begin));

    // ExeFS
    {
        auto exefs_blob = Meta::invoke([&] {
            std::ostringstream stream;
            WriteExeFS(ncch.exefs, stream);
            return stream.str();
        });
        file.write(reinterpret_cast<const char*>(exefs_blob.data()), exefs_blob.size());

#ifdef UPDATE_FIELDS
        // Update header hash
        CryptoPP::byte exefs_hash[CryptoPP::SHA256::DIGESTSIZE];
        ncch.header.exefs_hash_message_size = FileFormat::MediaUnit32::FromBytes(0x200);
        CryptoPP::SHA256().CalculateDigest(exefs_hash, reinterpret_cast<const CryptoPP::byte*>(exefs_blob.data()), ncch.header.exefs_hash_message_size.ToBytes());
        memcpy(ncch.header.exefs_sha256.data(), exefs_hash, sizeof(exefs_hash));
#endif
    }
    header.exefs_size = FileFormat::MediaUnit32::FromBytes(ncch.exefs.header.GetExeFSSize());

    // RomFS
    auto& romfs_data = ncch.romfs.data;
    header.romfs_size = FileFormat::MediaUnit32::FromBytes(RoundToNextMediaUnit(romfs_data.size()));
    if (romfs_data.size()) {
#ifdef UPDATE_FIELDS
        // TODO: Remove the extra padding; seemed necessary to get the output matching to the reference CIA, but shouldn't be necessary!
        header.romfs_offset = FileFormat::MediaUnit32::FromBytes(0x400 + RoundToNextMediaUnit(NarrowCastStreamPos(file.tellp()) - ncch_begin));
#endif

        auto romfs_begin = ncch_begin + NarrowCastStreamPos(ncch.header.romfs_offset.ToBytes());
        ZeroUpTo(file, romfs_begin);

        auto romfs_blob = Meta::invoke([&] {
            std::ostringstream stream;
            WriteRomFS(ncch.romfs, stream);
            return stream.str();
        });
        file.write(reinterpret_cast<const char*>(romfs_data.data()), romfs_data.size());

#ifdef UPDATE_FIELDS
        // Update header hash
        CryptoPP::byte romfs_hash[CryptoPP::SHA256::DIGESTSIZE];
        ncch.header.romfs_hash_message_size = FileFormat::MediaUnit32::FromBytes(0x200);
        CryptoPP::SHA256().CalculateDigest(romfs_hash, reinterpret_cast<const CryptoPP::byte*>(romfs_blob.data()), ncch.header.romfs_hash_message_size.ToBytes());
        memcpy(ncch.header.romfs_sha256.data(), romfs_hash, sizeof(romfs_hash));
#endif
    }

    auto ncch_end = file.tellp();

    std::string exheader_blob = Meta::invoke([&] {
        std::ostringstream stream;
        FileFormat::Save<FileFormat::ExHeader>(ncch.exheader, stream);
        return stream.str();
    });
    assert(exheader_blob.size() == 0x800); // TODO: removeme...

#ifdef UPDATE_FIELDS
    ncch.header.content_size = FileFormat::MediaUnit32::FromBytes(RoundToNextMediaUnit(NarrowCastStreamPos(ncch_end) - ncch_begin));

    // Update exheader hash
    CryptoPP::byte exheader_hash[CryptoPP::SHA256::DIGESTSIZE];
    // NOTE: It seems ncch.header.exheader_size need not be the same as the actual exheader blob size!
    assert(exheader_blob.size() >= ncch.header.exheader_size);
    CryptoPP::SHA256().CalculateDigest(exheader_hash, reinterpret_cast<const CryptoPP::byte*>(exheader_blob.data()), ncch.header.exheader_size);
    memcpy(ncch.header.exheader_sha256.data(), exheader_hash, sizeof(exheader_hash));
#endif

    // Write header now that we know all offsets
#ifdef PATCH_TITLEID
    ncch.header.partition_id = alt_titleid;
    ncch.header.program_id = alt_titleid;
#endif
    file.seekp(ncch_begin);
    FileFormat::Save<FileFormat::NCCHHeader>(ncch.header, file);

    file.seekp(exheader_begin);
    file.write(exheader_blob.data(), exheader_blob.size());

    file.seekp(ncch_end);
}

void WriteCIA(CIA& cia, std::ofstream& file, WriteNCCHFlags flags) {
    auto cia_begin = NarrowCastStreamPos(file.tellp());

    auto content_mask_begin = cia_begin + cia.content_mask.size();

    auto cert_begin = NarrowCastStreamPos(cia_begin) + (unsigned long long){(content_mask_begin + 63) & ~UINT32_C(63)};
    ZeroUpTo(file, cert_begin);
    for (auto cert_index = 0; cert_index < cia.certificates.size(); ++cert_index) {
        auto& certificate = cia.certificates[cert_index];
        SaveSignedData(certificate.sig, certificate.cert, file);

        if (certificate.cert.key_type == 0) {
            FileFormat::Save(certificate.pubkey.rsa4096, file);
        } else if (certificate.cert.key_type == 1) {
            FileFormat::Save(certificate.pubkey.rsa2048, file);
        } else if (certificate.cert.key_type == 1) {
            FileFormat::Save(certificate.pubkey.ecc, file);
        } else {
            throw std::runtime_error("Unknown certificate public key type");
        }
    }
    cia.header.certificate_chain_size = NarrowCastStreamPos(file.tellp()) - cert_begin;

#ifdef PATCH_TITLEID
    cia.ticket.data.ticket_id = 0x0004988F4CA451C8; // ???
    cia.ticket.data.title_id = alt_titleid;
#endif
    auto ticket_begin = cert_begin + (unsigned long long){(cia.header.certificate_chain_size + 63) & ~UINT32_C(63)};
    ZeroUpTo(file, ticket_begin);
    SaveSignedData(cia.ticket.sig, cia.ticket.data, file);
    cia.header.ticket_size = NarrowCastStreamPos(file.tellp()) - ticket_begin;

    // Serialize content in memory so that we can compute its hash (to be stored in the TMD)
    auto serialized_content = Meta::invoke([&] {
        std::ostringstream stream;
        WriteNCCH(cia.ncch, stream, flags);
        return stream.str();
    });

    auto tmd_begin = ticket_begin + (unsigned long long){(cia.header.ticket_size + 63) & ~UINT32_C(63)};
    ZeroUpTo(file, tmd_begin);
    assert(cia.tmd.content_infos.size() == 1); // TODO: Support more than one content
#ifdef UPDATE_FIELDS
    cia.tmd.data.content_count = cia.tmd.content_infos.size();
    cia.tmd.content_infos[0].size = cia.ncch.header.content_size.ToBytes();
#endif
#ifdef PATCH_TITLEID
    cia.tmd.data.title_id = alt_titleid;
#endif

    // Iterate over all TMD content infos, and set one bit in the CIA content mask for each set content
    cia.header.content_size = 0;
    for (size_t content = 0; content < cia.tmd.data.content_count; ++content) {
        auto&& content_info = cia.tmd.content_infos[content];
        auto index = content_info.index;
        cia.header.content_size += content_info.size;
        assert(index / 8 < cia.content_mask.size());
        cia.content_mask[index / 8] |= 0x80 >> (index % 8);
    }

    // Update content info hashes and serialize them into a buffer that we can hash for the meta content info
    std::string serialized_content_infos = Meta::invoke([&] {
        std::ostringstream stream;
        for (auto& content_info : cia.tmd.content_infos) {
#ifdef UPDATE_FIELDS
            content_info.size = serialized_content.size();
#endif
            assert(content_info.size == serialized_content.size());
            CryptoPP::SHA256().CalculateDigest(content_info.sha256.data(), reinterpret_cast<const CryptoPP::byte*>(serialized_content.data()), serialized_content.size());
            FileFormat::Save(content_info, stream);
        }
        return stream.str();
    });

    // Update contentmetainfo hashes and serialize them into a buffer that we can hash for the TMD header
    auto serialized_metacontentinfos = Meta::invoke([&] {
        std::ostringstream stream;

        for (auto& metacontentinfo : cia.tmd.content_info_hashes) {
            if (metacontentinfo.chunk_count) {
                assert(metacontentinfo.index_offset == 0);
                assert(metacontentinfo.chunk_count == 1);
                // Get content info hash
                CryptoPP::SHA256 running_hash;
                for (size_t chunk_index = 0; chunk_index < metacontentinfo.chunk_count; ++chunk_index) {
                    auto size = FileFormat::TMD::ContentInfo::Tags::expected_serialized_size;
                    auto* content_info_buffer = serialized_content_infos.data() + chunk_index * size;
                    running_hash.Update(reinterpret_cast<const CryptoPP::byte*>(content_info_buffer), size);
                }
                running_hash.Final(metacontentinfo.sha256.data());
            }
            FileFormat::Save(metacontentinfo, stream);
        }
        return stream.str();
    });
    CryptoPP::SHA256().CalculateDigest(cia.tmd.data.content_info_records_sha256.data(),
                                       reinterpret_cast<const CryptoPP::byte*>(serialized_metacontentinfos.data()), serialized_metacontentinfos.size());

    // Write final data to file
    SaveSignedData(cia.tmd.sig, cia.tmd.data, file);
    file.write(serialized_metacontentinfos.data(), serialized_metacontentinfos.size());
    file.write(serialized_content_infos.data(), serialized_content_infos.size());
    cia.header.tmd_size = NarrowCastStreamPos(file.tellp()) - tmd_begin;

    // TODO: Support multiple contents
    // TODO: Assert for just a single content being here
    auto content_begin = tmd_begin + (unsigned long long){(cia.header.tmd_size + 63) & ~UINT32_C(63)};
    ZeroUpTo(file, content_begin);
    file.write(serialized_content.data(), serialized_content.size());
#ifdef UPDATE_FIELDS
    cia.header.content_size = NarrowCastStreamPos(file.tellp()) - content_begin;
#endif

    auto meta_begin = content_begin + (unsigned long long){(cia.header.content_size + 63) & ~UINT32_C(63)};
    ZeroUpTo(file, meta_begin);
    FileFormat::Save(cia.meta, file);
    cia.header.meta_size = NarrowCastStreamPos(file.tellp()) - meta_begin;

    auto cia_end = file.tellp();

    // Seek back to the beginning to write the header with any updated offsets
    file.seekp(cia_begin);
    FileFormat::Save(cia.header, file);

    file.seekp(content_mask_begin);
    file.write((const char*)cia.content_mask.data(), cia.content_mask.size());

    file.seekp(cia_end);
}

CIA InjectCIA(const std::string& in_filename, const std::string& injected_filename) {

    auto cia = Meta::invoke([&]() {
                                std::ifstream file(in_filename, std::ios_base::binary);
                                file.exceptions(std::ifstream::badbit | std::ifstream::failbit | std::ifstream::eofbit);
                                return ParseCIA(file);
                            });
return cia;
    std::cout << std::endl << std::endl << "Reading NCCH \"" << injected_filename << "\" to inject now... " << std::endl;
    const auto ncch_to_inject = Meta::invoke([&]() {
                                                std::ifstream file(injected_filename, std::ios_base::binary);
                                                file.exceptions(std::ifstream::badbit | std::ifstream::failbit | std::ifstream::eofbit);
                                                return ParseNCCH(file);
                                            });

    cia.ncch = ncch_to_inject;

    // TODO: Merge extended headers (romfs access, etc)

    return cia;
}

struct Dot3DSX {
    FileFormat::Dot3DSX::Header header;

    std::array<FileFormat::Dot3DSX::RelocationHeader, 3> relocation_headers;

    std::vector<FileFormat::Dot3DSX::RelocationInfo> relocations;
};

void Inject3DSXFrom(std::istream& stream, CIA::NCCH& inject_target) {
    Dot3DSX ret;

    ret.header = FileFormat::Load<FileFormat::Dot3DSX::Header>(stream);
    auto& header = ret.header;

    // Validate input
    assert(ranges::equal(header.magic, header.expected_magic));

    assert(ret.header.version == 0);
    assert(ret.header.flags == 0);

    assert(ret.header.data_bss_size >= ret.header.bss_size);

    // NOTE: The header size may actually be different, in which case there may
    //       be an extended header for ROMFS/SMDH information
    assert(ret.header.header_size >= FileFormat::Dot3DSX::Header::Tags::expected_serialized_size);
    if (ret.header.header_size != FileFormat::Dot3DSX::Header::Tags::expected_serialized_size) {
        std::cout << "WARNING: Unrecognized header size; skipping unknown part" << std::endl;
        stream.seekg(ret.header.header_size - FileFormat::Dot3DSX::Header::Tags::expected_serialized_size, std::ios_base::cur);
    }

    for (auto& reloc_header : ret.relocation_headers)
        reloc_header = FileFormat::Load<FileFormat::Dot3DSX::RelocationHeader>(stream);

    std::cout << "3DSX:" << std::endl;
    std::cout << "Text segment offset: 0x" << std::hex << std::setw(8) << std::setfill('0') << header.TextOffset() << " (0x" << header.text_size << " bytes)" << std::endl;
    std::cout << "Ro segment offset: 0x" << std::hex << std::setw(8) << std::setfill('0') << header.RoOffset() << " (0x" << header.ro_size << " bytes)" << std::endl;
    std::cout << "Data segment offset: 0x" << std::hex << std::setw(8) << std::setfill('0') << header.DataOffset()
              << " (0x" << header.data_bss_size << " bytes total, 0x" << header.bss_size << " for bss)" << std::endl;

    // Section sizes aligned to pages
    uint32_t text_numpages = ((header.text_size +0xfff) >> 12);
    uint32_t ro_numpages = ((header.ro_size+0xfff) >> 12);
    uint32_t data_numpages = (((header.data_bss_size - header.bss_size) + 0xfff) >> 12);

    uint32_t text_vaddr = 0x00100000;
    uint32_t ro_vaddr = text_vaddr + (text_numpages << 12);
    uint32_t data_vaddr = ro_vaddr + (ro_numpages << 12);

    inject_target.exheader.flags = inject_target.exheader.flags.compress_exefs_code().Set(0);
    inject_target.exheader.section_text = FileFormat::ExHeader::CodeSetInfo{text_vaddr, text_numpages, header.text_size };
    inject_target.exheader.section_ro = FileFormat::ExHeader::CodeSetInfo{ro_vaddr, ro_numpages, header.ro_size };
    inject_target.exheader.section_data = FileFormat::ExHeader::CodeSetInfo{data_vaddr, data_numpages, header.data_bss_size - header.bss_size };
    inject_target.exheader.bss_size = header.bss_size;
//     inject_target.exheader.stack_size = ; // TODO: Should we adjust this?
    inject_target.exheader.stack_size = 0x10000; // TODO: Should we adjust this?
    // TODO: Override program_id and jump_id!!

    // Read program sections
    uint32_t total_size = (text_numpages + ro_numpages + data_numpages) << 12;
    std::vector<uint32_t> program_data(total_size / 4, 0);
    const std::array<uint32_t*, 3> segment_ptrs = {{ program_data.data(),
                                                     program_data.data() + (ro_vaddr - text_vaddr) / 4,
                                                     program_data.data() + (data_vaddr - text_vaddr) / 4 }};

    // TODO: Endianness!
    stream.read(reinterpret_cast<char*>(segment_ptrs[0]), header.text_size);
    stream.read(reinterpret_cast<char*>(segment_ptrs[1]), header.ro_size);
    stream.read(reinterpret_cast<char*>(segment_ptrs[2]), header.data_bss_size - header.bss_size);

    // Patch in relocations
    for (unsigned segment = 0; segment < 3; ++segment) {
        auto& reloc_header = ret.relocation_headers[segment];

        // patch_kind = 0: absolute relocation; patch_kind = 1: relative relocation
        for (unsigned patch_kind = 0; patch_kind < 2; ++patch_kind) {
            unsigned num_relocations = (patch_kind==0) ? reloc_header.num_abs_relocs : reloc_header.num_rel_relocs;

            // Pointer to the word currently being patched
            uint32_t* patch_ptr = segment_ptrs[segment];

            // TODO: iostream'ify!
            printf("Seg %d, patch kind %d: %d relocations\n", segment, patch_kind, num_relocations);
            for (unsigned relocation = 0; relocation < num_relocations; ++relocation) {
                auto reloc_info = FileFormat::Load<FileFormat::Dot3DSX::RelocationInfo>(stream);

                // TODO: iostream'ify!
                printf("Relocation %d: %d/%d words to skip/patch\n", relocation, reloc_info.words_to_skip, reloc_info.words_to_patch);

                patch_ptr += reloc_info.words_to_skip;
                for (unsigned word_index = 0; word_index < reloc_info.words_to_patch; ++word_index) {
                    // TODO: Make sure patch_ptr is still within the current segment!

                    uint32_t unpatched_word = *patch_ptr;
                    uint32_t virtual_address = (unpatched_word & 0x0FFFFFFF) + text_vaddr; // TODO: This might be oversimplified compared to TranslateAddress
                    uint32_t sub_type = unpatched_word >> 28;

                    if (patch_kind == 0) {
                        assert(sub_type == 0);
                        *patch_ptr = virtual_address;
                    } else {
                        assert(sub_type < 2);
                        // TODO: need to actually respect the sub type...
                        uint32_t offset = patch_ptr - segment_ptrs[0];
                        *patch_ptr = virtual_address - offset;
                    }

                    patch_ptr++;
                }
            }
        }
    }

    assert(ranges::equal(inject_target.exefs.header.files[0].name, FileFormat::ExeFSHeader::code_section_name));

    auto& exefs_code_section = inject_target.exefs.sections[0];
    std::cout << "Changing ExeFS size from 0x" << std::hex << exefs_code_section.data.size() << " to 0x" << program_data.size() * sizeof(program_data[0]) << std::endl;
    // TODO: Endianness!
    exefs_code_section.data.clear();
    exefs_code_section.data.resize(program_data.size() * sizeof(program_data[0]));
    memcpy(exefs_code_section.data.data(), program_data.data(), exefs_code_section.data.size());
}

/// TODO: Get rid of the NCCH argument, which we currently need as a template
void Generate3DSX(std::istream& stream, CIA::NCCH& inject_target) {
    Dot3DSX ret;

    ret.header = FileFormat::Load<FileFormat::Dot3DSX::Header>(stream);
    auto& header = ret.header;

    // Validate input
    assert(ranges::equal(header.magic, header.expected_magic));

    assert(ret.header.version == 0);
    assert(ret.header.flags == 0);

    assert(ret.header.data_bss_size >= ret.header.bss_size);

    // NOTE: The header size may actually be different, in which case there may
    //       be an extended header for ROMFS/SMDH information
    assert(ret.header.header_size >= FileFormat::Dot3DSX::Header::Tags::expected_serialized_size);
    if (ret.header.header_size != FileFormat::Dot3DSX::Header::Tags::expected_serialized_size) {
        std::cout << "WARNING: Unrecognized header size; skipping unknown part" << std::endl;
        stream.seekg(ret.header.header_size - FileFormat::Dot3DSX::Header::Tags::expected_serialized_size, std::ios_base::cur);
    }

    for (auto& reloc_header : ret.relocation_headers)
        reloc_header = FileFormat::Load<FileFormat::Dot3DSX::RelocationHeader>(stream);

    std::cout << "3DSX:" << std::endl;
    std::cout << "Text segment offset: 0x" << std::hex << std::setw(8) << std::setfill('0') << header.TextOffset() << " (0x" << header.text_size << " bytes)" << std::endl;
    std::cout << "Ro segment offset: 0x" << std::hex << std::setw(8) << std::setfill('0') << header.RoOffset() << " (0x" << header.ro_size << " bytes)" << std::endl;
    std::cout << "Data segment offset: 0x" << std::hex << std::setw(8) << std::setfill('0') << header.DataOffset()
              << " (0x" << header.data_bss_size << " bytes total, 0x" << header.bss_size << " for bss)" << std::endl;

    // Section sizes aligned to pages
    uint32_t text_numpages = ((header.text_size +0xfff) >> 12);
    uint32_t ro_numpages = ((header.ro_size+0xfff) >> 12);
    uint32_t data_numpages = (((header.data_bss_size - header.bss_size) + 0xfff) >> 12);

    uint32_t text_vaddr = 0x00100000;
    uint32_t ro_vaddr = text_vaddr + (text_numpages << 12);
    uint32_t data_vaddr = ro_vaddr + (ro_numpages << 12);

    inject_target.exheader.application_title = std::array<unsigned char, 8>{};
    inject_target.exheader.unknown = {};
    inject_target.exheader.flags = {};//inject_target.exheader.flags.compress_exefs_code().Set(0);
    inject_target.exheader.remaster_version = {};
    inject_target.exheader.section_text = FileFormat::ExHeader::CodeSetInfo{text_vaddr, text_numpages, header.text_size };
    inject_target.exheader.section_ro = FileFormat::ExHeader::CodeSetInfo{ro_vaddr, ro_numpages, header.ro_size };
    inject_target.exheader.section_data = FileFormat::ExHeader::CodeSetInfo{data_vaddr, data_numpages, header.data_bss_size - header.bss_size };
    inject_target.exheader.bss_size = header.bss_size;
    inject_target.exheader.stack_size = 0x1000; // TODO: Should we adjust this to some other value?
    inject_target.exheader.unknown3 = {};
    inject_target.exheader.unknown4 = {};

    // Read program sections
    uint32_t total_size = (text_numpages + ro_numpages + data_numpages) << 12;
    std::vector<uint32_t> program_data(total_size / 4, 0);
    const std::array<uint32_t*, 3> segment_ptrs = {{ program_data.data(),
                                                     program_data.data() + (ro_vaddr - text_vaddr) / 4,
                                                     program_data.data() + (data_vaddr - text_vaddr) / 4 }};

    // TODO: Endianness!
    stream.read(reinterpret_cast<char*>(segment_ptrs[0]), header.text_size);
    stream.read(reinterpret_cast<char*>(segment_ptrs[1]), header.ro_size);
    stream.read(reinterpret_cast<char*>(segment_ptrs[2]), header.data_bss_size - header.bss_size);

    // Patch in relocations
    for (unsigned segment = 0; segment < 3; ++segment) {
        auto& reloc_header = ret.relocation_headers[segment];

        // patch_kind = 0: absolute relocation; patch_kind = 1: relative relocation
        for (unsigned patch_kind = 0; patch_kind < 2; ++patch_kind) {
            unsigned num_relocations = (patch_kind==0) ? reloc_header.num_abs_relocs : reloc_header.num_rel_relocs;

            // Pointer to the word currently being patched
            uint32_t* patch_ptr = segment_ptrs[segment];

            // TODO: iostream'ify!
            printf("Seg %d, patch kind %d: %d relocations\n", segment, patch_kind, num_relocations);
            for (unsigned relocation = 0; relocation < num_relocations; ++relocation) {
                auto reloc_info = FileFormat::Load<FileFormat::Dot3DSX::RelocationInfo>(stream);

                // TODO: iostream'ify!
                printf("Relocation %d: %d/%d words to skip/patch\n", relocation, reloc_info.words_to_skip, reloc_info.words_to_patch);

                patch_ptr += reloc_info.words_to_skip;
                for (unsigned word_index = 0; word_index < reloc_info.words_to_patch; ++word_index) {
                    // TODO: Make sure patch_ptr is still within the current segment!

                    uint32_t unpatched_word = *patch_ptr;
                    uint32_t virtual_address = (unpatched_word & 0x0FFFFFFF) + text_vaddr; // TODO: This might be oversimplified compared to TranslateAddress
                    uint32_t sub_type = unpatched_word >> 28;

                    if (patch_kind == 0) {
                        assert(sub_type == 0);
                        *patch_ptr = virtual_address;
                    } else {
                        assert(sub_type < 2);
                        // TODO: need to actually respect the sub type...
                        uint32_t offset = patch_ptr - segment_ptrs[0];
                        *patch_ptr = virtual_address - offset;
                    }

                    patch_ptr++;
                }
            }
        }
    }

    // TODO: We clear the exefs below anyway. Why bother with this assert?
//     assert(ranges::equal(inject_target.exefs.header.files[0].name, FileFormat::ExeFSHeader::code_section_name));

    // Clear ExeFS and write new one
    // TODO: Write SMDH if there is any
    inject_target.exefs = CIA::NCCH::ExeFS{};
    auto& exefs_code_section_header = inject_target.exefs.header.files[0];
    std::memcpy(exefs_code_section_header.name.data(), ".code", 5);

    auto& exefs_code_section = inject_target.exefs.sections[0];
    std::cout << "Changing ExeFS size from 0x" << std::hex << exefs_code_section.data.size() << " to 0x" << program_data.size() * sizeof(program_data[0]) << std::endl;
    // TODO: Endianness!
    exefs_code_section.data.clear();
    exefs_code_section.data.resize(program_data.size() * sizeof(program_data[0]));
    memcpy(exefs_code_section.data.data(), program_data.data(), exefs_code_section.data.size());
}

// Custom program_options validator. The sole purpose of this is to do hexadecimal number parsing
void validate(boost::any& v,
              const std::vector<std::string>& xs,
              HexUint64*, long)
{
    using namespace boost::program_options;

    validators::check_first_occurrence(v);
    std::string s(validators::get_single_string(xs));

    if (s.size() < 2 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
         boost::throw_exception(invalid_option_value(s));

    try {
        size_t pos;
        auto result = HexUint64{std::stoull(s, &pos, 16)};
        if (pos == s.size())
            v = boost::any(HexUint64{std::stoull(s, 0, 16)});
        else
            boost::throw_exception(invalid_option_value(s));
    } catch (...) {
         boost::throw_exception(invalid_option_value(s));
    }
}

int main(int argc, char* argv[]) {
    std::string in_filename;
    std::string in_3dsx_filename;

    // Use a set of default dependencies when the caller does not specify any
    std::vector<HexUint64> exheader_dependencies = {
        {0x0004013000008002 /* ns */},
        {0x0004013000001c02 /* gsp */},
        {0x0004013000001d02 /* hid */},
        {0x0004013000001802 /* codec */},
        {0x0004013000003102 /* ps */},
        {0x0004013000001b02 /* gpio */}
    };

    bool generate_ncch;
    bool generate_cia;

    {
        namespace bpo = boost::program_options;

        // NOTE: Legacy syntax is argv[1] == template_cia, argv[2] == input

        bpo::options_description desc;
        desc.add_options()
            ("help,h", "Print this help")
            ("template-cia", bpo::value<std::string>(&in_filename), "Path to CIA file to use to get certificate signatures")
            ("input,i", bpo::value<std::string>(&in_3dsx_filename)->required(), "Path to input 3DSX file")
            ("title-id", bpo::value<HexUint64>(&alt_titleid)->required(), "Title ID to use for the output file")
            ("dep", bpo::value<std::vector<HexUint64>>(&exheader_dependencies)->composing(), "Add ExHeader dependency")
            ("gen-ncch", bpo::bool_switch(&generate_ncch)->default_value(false), "Generate NCCH")
            ("gen-cia", bpo::bool_switch(&generate_cia)->default_value(false), "Generate CIA")
            ;

        try {
            auto positional_arguments = bpo::positional_options_description{}.add("input", -1);
            bpo::variables_map var_map;
            bpo::store(bpo::command_line_parser(argc, argv).options(desc).positional(positional_arguments).run(),
                       var_map);
            if (var_map.count("help") ) {
                std::cout << desc << std::endl;
                return 0;
            }

            if (generate_cia && var_map.count("template-cia") == 0) {
                std::cerr << "Must specify --template-cia when generating a CIA" << std::endl;
                return 1;
            }

            bpo::notify(var_map);
        } catch (bpo::error error) {
            std::cout << desc << std::endl;
            return 1;
        }
    }

    CIA::NCCH ncch;

    // Generate NCCH - needed by both NCCH and CIA generator
    {
        // Generate fake exheader with maximum permissions
        ncch.exheader = {};
        ncch.exheader.application_title = {};
        ncch.exheader.unknown = {};
        ncch.exheader.flags = {};//ncch.exheader.flags.compress_exefs_code().Set(0);
        ncch.exheader.remaster_version = {};
        // Filled out by Generate3DSX
//         ncch.exheader.section_text = FileFormat::ExHeader::CodeSetInfo{text_vaddr, text_numpages, header.text_size};
//         ncch.exheader.section_ro = FileFormat::ExHeader::CodeSetInfo{ro_vaddr, ro_numpages, header.ro_size};
//         ncch.exheader.section_data = FileFormat::ExHeader::CodeSetInfo{data_vaddr, data_numpages, header.data_bss_size - header.bss_size};
//         ncch.exheader.bss_size = header.bss_size;
        ncch.exheader.stack_size = 0x4000; // TODO: Should we adjust this to some other value?
        ncch.exheader.unknown3 = {};
        ncch.exheader.unknown4 = {};

        ncch.exheader.jump_id = {}; // TODO: Consider passing in alt_titleid here
        ncch.exheader.save_data_size = {};
        ncch.exheader.dependencies = {};
        ranges::copy(exheader_dependencies, ncch.exheader.dependencies.begin());

        ncch.exheader.subsignature = {};
        ncch.exheader.ncch_sig_pub_key = {};


        ncch.exheader.aci.program_id = alt_titleid;
        ncch.exheader.aci.version = 2;
        ncch.exheader.aci.flags = FileFormat::ExHeader::ACIFlags{}.flag0()(4).flag1()(3).flag2()(2).priority()(0x30);
        ncch.exheader.aci.unknown5 = {};

        ranges::fill(ncch.exheader.aci.arm11_kernel_capabilities, FileFormat::ExHeader::ARM11KernelCapabilityDescriptor{0xffffffff});
        using SVCMaskTableEntry = FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::SVCMaskTableEntry;
        using HandleTableInfo = FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::HandleTableInfo;
        using MappedMemoryRange = FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::MappedMemoryRange;
        using KernelFlags = FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::KernelFlags;

        ncch.exheader.aci.arm11_kernel_capabilities[0].storage = SVCMaskTableEntry::Make().entry_index()(0).mask()(0xfffffe).storage;
        ncch.exheader.aci.arm11_kernel_capabilities[1].storage = SVCMaskTableEntry::Make().entry_index()(1).mask()(0xffffff).storage;
        ncch.exheader.aci.arm11_kernel_capabilities[2].storage = SVCMaskTableEntry::Make().entry_index()(2).mask()(0x807fff).storage;
        ncch.exheader.aci.arm11_kernel_capabilities[3].storage = SVCMaskTableEntry::Make().entry_index()(3).mask()(0x1ffff).storage;
        ncch.exheader.aci.arm11_kernel_capabilities[4].storage = SVCMaskTableEntry::Make().entry_index()(4).mask()(0xef3fff).storage;
        ncch.exheader.aci.arm11_kernel_capabilities[5].storage = SVCMaskTableEntry::Make().entry_index()(5).mask()(0x3f).storage;
        ncch.exheader.aci.arm11_kernel_capabilities[6].storage = HandleTableInfo::Make().num_handles()(0x200).storage; // TODO: 0x200 handles is somewhat overkill...
        ncch.exheader.aci.arm11_kernel_capabilities[7].storage = MappedMemoryRange::Make().page_index()(0x1ff00).storage;
        ncch.exheader.aci.arm11_kernel_capabilities[8].storage = MappedMemoryRange::Make().page_index()(0x1ff80).storage;
        ncch.exheader.aci.arm11_kernel_capabilities[9].storage = MappedMemoryRange::Make().page_index()(0x1f000).read_only()(1).storage;
        ncch.exheader.aci.arm11_kernel_capabilities[10].storage = MappedMemoryRange::Make().page_index()(0x1f600).read_only()(1).storage;
        ncch.exheader.aci.arm11_kernel_capabilities[11].storage = KernelFlags::Make().memory_type()(1).storage;

        // Map all IO memory
        ncch.exheader.aci.arm11_kernel_capabilities[12].storage = MappedMemoryRange::Make().page_index()(0x1ec00).storage;
        ncch.exheader.aci.arm11_kernel_capabilities[13].storage = MappedMemoryRange::Make().page_index()(0x1f000).storage;

        ncch.exheader.aci.service_access_list = {};
        const char* default_services[] = {
            "APT:U", "ac:u", "am:net", "boss:U", "cam:u",
            "cecd:u", "cfg:nor", "cfg:u", "csnd:SND",
            "dsp::DSP", "frd:u", "fs:USER", "gsp::Gpu",
            "gsp::Lcd", "hid:USER", "http:C", "ir:rst",
            "ir:u", "ir:USER", "mic:u", "ndm:u",
            "news:s", "nwm::EXT", "nwm::UDS", "ptm:sysm",
            "ptm:u", "pxi:dev", "soc:U", // "ssl:C",
            "y2r:u", "pm:app", "ns:s"
        };
        static_assert(sizeof(default_services) / sizeof(default_services[0]) < ncch.exheader.aci.service_access_list.size(),
                      "Maximum number of services exhausted");
        ranges::transform(default_services, ncch.exheader.aci.service_access_list.begin(),
                          [](auto& service) {
                              std::array<uint8_t, 8> ret{};
                              ranges::copy(service, service + strlen(service), ret.begin());
                              return ret;
                          });

        ncch.exheader.aci.unknown6 = {};
        ncch.exheader.aci.unknown7 = {};
        // TODO: Not sure what these are about.
        ncch.exheader.aci.unknown7[0x10] = 0xff;
        ncch.exheader.aci.unknown7[0x11] = 0x3;
        ncch.exheader.aci.unknown7[0x1f] = 0x2;
        ncch.exheader.aci_limits = ncch.exheader.aci;

        // Ideal processor in primary ACI must be smaller or equal to the one in the secondary (TODO: Verify!)
        // Priority in primary ACI must be larger or equal to the one in the secondary (TODO: Verify!)
        ncch.exheader.aci_limits.flags = ncch.exheader.aci_limits.flags.ideal_processor()(1).priority()(0x18);

        {
            std::ifstream ifile(in_3dsx_filename, std::ios_base::binary);
            ifile.exceptions(std::ofstream::badbit | std::ofstream::failbit | std::ofstream::eofbit);
            Generate3DSX(ifile, ncch);
        }

        ncch.header = FileFormat::NCCHHeader{};
        ncch.header.magic = std::array<uint8_t, 4>{{'N', 'C', 'C', 'H'}};
        // content_size filled in by WriteNCCH
        ncch.header.partition_id = alt_titleid;
        ncch.header.program_id = alt_titleid;
        ncch.header.crypto_method = 0;
        ncch.header.platform = 1; // Old3DS
        ncch.header.type_mask = 0; // 3 ???
        ncch.header.unit_size_log2 = 0;
        ncch.header.flags = 0x1 | 0x2 | 0x4; // don't mount RomFS; don't encrypt contents
        ncch.header.exefs_hash_message_size = FileFormat::MediaUnit32{1};
        ncch.header.romfs_hash_message_size = FileFormat::MediaUnit32{1};

        ncch.header.exheader_size = 0x400;
    }

    if (generate_ncch) {
        std::ofstream ofile(std::string(in_3dsx_filename) + ".ncch", std::ios_base::binary);
        ofile.exceptions(std::ofstream::badbit | std::ofstream::failbit | std::ofstream::eofbit);
        WriteNCCH(ncch, ofile, static_cast<WriteNCCHFlags>(0));
    }

    if (generate_cia) {
        std::ifstream file(in_filename, std::ios_base::binary);
        file.exceptions(std::ifstream::badbit | std::ifstream::failbit | std::ifstream::eofbit);
        auto cia = ParseCIA(file); // TODO: Remove

        // Move out the certificate information, since common 3DS firmware patches
        // apparently don't patch out the certificate signature checks
        auto imported_certificates = std::move(cia.certificates);

        // Reset CIA because other than the certificate signatures, we don't need any other data
        // TODO: Well.. we still need parts of it, apparently
//         cia = {};

        std::string cert_issuers[] = {
            "Root", "Root-CA00000003", "Root-CA00000003"
        };
        std::string cert_names[] = {
            "CA00000003", "XS0000000c", "CP0000000b"
        };
        uint32_t cert_signature_types[] = {
            0x10003, 0x10004, 0x10004
        };

        for (int i : {0, 1, 2}) {
            ranges::copy(cert_issuers[i], cia.certificates[i].cert.issuer.begin());
            ranges::copy(cert_names[i], cia.certificates[i].cert.name.begin());
            cia.certificates[i].sig.type = cert_signature_types[i];
            cia.certificates[i].sig.data = std::move(imported_certificates[i].sig.data);
            cia.certificates[i].cert.key_type = 1;
        }

        auto ticket_issuer = cert_issuers[1] + '-' + cert_names[1];
        ranges::copy(ticket_issuer, cia.ticket.data.issuer.begin());
        cia.ticket.sig.type = 0x10004;
        cia.ticket.sig.data.resize(FileFormat::GetSignatureSize(cia.ticket.sig.type));

        auto tmd_issuer = cert_issuers[2] + '-' + cert_names[2];
        ranges::copy(tmd_issuer, cia.tmd.data.issuer.begin());
        cia.tmd.sig.type = 0x10004;
        cia.tmd.sig.data.resize(FileFormat::GetSignatureSize(cia.tmd.sig.type));

        cia.ncch = ncch;

        // Generate ticket from scratch
        auto dummy_sig = FileFormat::Signature{0x10004 /* ECDSA with RSA256 */};
        dummy_sig.data.resize(0x100);

        cia.ticket = FileFormat::Ticket{dummy_sig};
        std::memset(cia.ticket.data.signature_pubkey.data(), 0,
                    sizeof(cia.ticket.data.signature_pubkey) + 0x13);
        cia.ticket.data.version = 1;
        cia.ticket.data.title_version = 0x842;
        cia.ticket.data.ticket_id = 0x0004988F4CA451C8; // ???
        cia.ticket.data.title_id = alt_titleid;
        const char default_issuer[] = "Root-CA00000003-XS0000000c";
        std::memcpy(cia.ticket.data.issuer.data(), default_issuer, sizeof(default_issuer));
        // NOTE: The stuff below seems to be relevant. meh :(
        cia.ticket.data.unknown4[1] = 0x1;
        cia.ticket.data.unknown4[3] = 0x14;
        cia.ticket.data.unknown4[7] = 0xac;
        cia.ticket.data.unknown4[11] = 0x14;
        cia.ticket.data.unknown4[13] = 0x1;
        cia.ticket.data.unknown4[15] = 0x14;
        cia.ticket.data.unknown4[23] = 0x28;
        cia.ticket.data.unknown4[27] = 0x1;
        cia.ticket.data.unknown4[31] = 0x84;
        cia.ticket.data.unknown4[35] = 0x84;
        cia.ticket.data.unknown4[37] = 0x3;
        cia.ticket.data.unknown4[44] = 0x1;

        cia.tmd = FileFormat::TMD{dummy_sig};
        auto& tmd_data = cia.tmd.data;
        const char default_tmd_issuer[] = "Root-CA00000003-CP0000000b";
        std::memcpy(cia.tmd.data.issuer.data(), default_tmd_issuer, sizeof(default_tmd_issuer));
        tmd_data.version = 1;
        tmd_data.title_id = alt_titleid;
        // Content count filled in later
        cia.tmd.content_info_hashes[0].index_offset = 0;
        cia.tmd.content_info_hashes[0].chunk_count = 1;
        cia.tmd.content_infos.emplace_back(FileFormat::TMD::ContentInfo{0, 0, 0, 0 /* content size filled in later */});

        cia.meta = FileFormat::CIAMeta{};
        // Meta doesn't seem to be necessary anyway
        cia.header = FileFormat::CIAHeader{};
        cia.header.header_size = FileFormat::CIAHeader::Tags::expected_serialized_size;
        // Rest of the header is filled in WriteCIA

        std::ofstream ofile(std::string(in_3dsx_filename) + ".cia", std::ios_base::binary);
        ofile.exceptions(std::ofstream::badbit | std::ofstream::failbit | std::ofstream::eofbit);

        WriteCIA(cia, ofile, static_cast<WriteNCCHFlags>(0));

        {
            std::ifstream ifile(in_3dsx_filename + std::string(".cia"), std::ios_base::binary);
            ifile.exceptions(std::ofstream::badbit | std::ofstream::failbit | std::ofstream::eofbit);
            (void)ParseCIA(ifile);
        }
    }
}
