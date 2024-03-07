#include "gamecard.hpp"
#include "platform/file_formats/ncch.hpp"
#include "platform/file_formats/3dsx.hpp"
#include "processes/pxi_fs.hpp"
#include "processes/pxi.hpp"
#include "host_file.hpp"

#include <framework/exceptions.hpp>

#include <range/v3/algorithm/copy.hpp>
#include <range/v3/algorithm/equal.hpp>
#include <range/v3/algorithm/fill.hpp>
#include <range/v3/algorithm/transform.hpp>

#include <spdlog/sinks/null_sink.h>

namespace Loader {

GameCard::~GameCard() = default;

GameCardFromCXI::GameCardFromCXI(std::string_view filename) : source(std::string { filename }) {
}

GameCardFromCXI::GameCardFromCXI(int file_descriptor) : source(file_descriptor) {
}

struct GameCardSourceOpener {
    std::unique_ptr<HLE::PXI::FS::File>
    operator()(const std::string& filename) const {
        return std::unique_ptr<HLE::PXI::FS::File>(new HLE::PXI::FS::HostFile(filename, HLE::PXI::FS::HostFile::Default));
    }

    std::unique_ptr<HLE::PXI::FS::File>
    operator()(int file_descriptor) const {
        return FileSystem::OpenNativeFile(file_descriptor);
    }
};

std::optional<std::unique_ptr<HLE::PXI::FS::File>> GameCardFromCXI::GetPartitionFromId(NCSDPartitionId id) {
    if (id == NCSDPartitionId::Executable) {
        return std::visit(GameCardSourceOpener{}, source);
    } else {
        // TODO: Provide dummy-partitions instead
        throw std::runtime_error("Attempted to open non-existing partition from CXI game image");
    }
}

static bool IsLoadableCXIFile(HLE::PXI::FS::File& file) {
    // TODO: Enable proper logging
    auto logger = std::make_shared<spdlog::logger>("dummy", std::make_shared<spdlog::sinks::null_sink_st>());
    auto file_context = HLE::PXI::FS::FileContext { *logger };
    file.Open(file_context, false);

    FileFormat::NCCHHeader header;
    auto [result, bytes_read] = file.Read(file_context, 0, sizeof(header), HLE::PXI::FS::FileBufferInHostMemory(header));
    if (result != HLE::OS::RESULT_OK || bytes_read != sizeof(header)) {
        return false;
    }
    return ranges::equal(header.magic, std::string_view { "NCCH" });
}

bool GameCardFromCXI::IsLoadableFile(std::string_view filename) {
    auto file = HLE::PXI::FS::HostFile(filename, {});
    return IsLoadableCXIFile(file);
}

bool GameCardFromCXI::IsLoadableFile(int file_descriptor) {
    return IsLoadableCXIFile(*FileSystem::OpenNativeFile(file_descriptor));
}

GameCardFromCCI::GameCardFromCCI(std::string_view filename) : source(std::string { filename }) {
}

GameCardFromCCI::GameCardFromCCI(int file_descriptor) : source(file_descriptor) {
}

std::optional<std::unique_ptr<HLE::PXI::FS::File>> GameCardFromCCI::GetPartitionFromId(NCSDPartitionId id) {
    // TODO: Enable logging
    auto logger = std::make_shared<spdlog::logger>("dummy", std::make_shared<spdlog::sinks::null_sink_st>());
    auto file_context = HLE::PXI::FS::FileContext { *logger };
    auto cci_file = std::visit(GameCardSourceOpener{}, source);

    cci_file->Open(file_context, false);
    FileFormat::NCSDHeader ncsd;
    cci_file->Read(file_context, 0, decltype(ncsd)::Tags::expected_serialized_size, HLE::PXI::FS::FileBufferInHostMemory(ncsd));
    cci_file->Close();

    auto& ncsd_partition = ncsd.partition_table[Meta::to_underlying(id)];
    auto ncch_file = std::unique_ptr<HLE::PXI::FS::File>(new HLE::PXI::FS::FileView(std::move(cci_file), ncsd_partition.offset.ToBytes(), ncsd_partition.size.ToBytes()));
    return std::move(ncch_file);
}

static bool IsLoadableCCIFile(HLE::PXI::FS::File& file) {
    // TODO: Enable proper logging
    auto logger = std::make_shared<spdlog::logger>("dummy", std::make_shared<spdlog::sinks::null_sink_st>());
    auto file_context = HLE::PXI::FS::FileContext { *logger };
    file.Open(file_context, false);

    FileFormat::NCSDHeader header;
    auto [result, bytes_read] = file.Read(file_context, 0, sizeof(header), HLE::PXI::FS::FileBufferInHostMemory(header));
    if (result != HLE::OS::RESULT_OK || bytes_read != sizeof(header)) {
        return false;
    }
    return ranges::equal(header.magic, std::string_view { "NCSD" });
}

bool GameCardFromCCI::IsLoadableFile(std::string_view filename) {
    auto file = HLE::PXI::FS::HostFile(filename, {});
    return IsLoadableCCIFile(file);
}

bool GameCardFromCCI::IsLoadableFile(int file_descriptor) {
    return IsLoadableCCIFile(*FileSystem::OpenNativeFile(file_descriptor));
}

namespace {

/**
 * Adaptor to make 3DSX files "look" like NCCHs.
 */
class Adaptor3DSXToNCCH : public HLE::PXI::FS::File {
    std::unique_ptr<HLE::PXI::FS::File> file; // 3DSX source data

    // TODO: Actually store the memory in here!

    FileFormat::NCCHHeader ncch = {};
    FileFormat::ExHeader exheader = {};

    FileFormat::Dot3DSX::Header dot3dsxheader = {};

public:
    Adaptor3DSXToNCCH(std::unique_ptr<HLE::PXI::FS::File> file_3dsx) : file(std::move(file_3dsx)) {
        // Create a fake ncch
        ncch = FileFormat::NCCHHeader{};
        ncch.magic = std::array<uint8_t, 4>{{'N', 'C', 'C', 'H'}};
        // TODO: content_size
        ncch.partition_id = 0xdeadbeefdeadbeef;
        ncch.program_id = 0xdeadbeefdeadbeef;
        ncch.crypto_method = 0;
        ncch.platform = 1; // Old3DS
        ncch.type_mask = 0; // 3 ???
        ncch.unit_size_log2 = 0;
        ncch.flags = 0x1 | 0x2 | 0x4; // don't mount RomFS; don't encrypt contents
        ncch.exefs_hash_message_size = FileFormat::MediaUnit32{1};
        ncch.romfs_hash_message_size = FileFormat::MediaUnit32{1};

        ncch.exheader_size = 0x400;


        // Generate fake exheader with maximum permissions
        // TODO: Restrict to fields which are actually necessary in emulation
        // TODO: Parts of this are re-initialized in Read()!
        exheader = {};
        exheader.application_title = {};
        exheader.unknown = {};
        exheader.flags = {};//exheader.flags.compress_exefs_code().Set(0);
        exheader.remaster_version = {};
        // Filled out by Generate3DSX
//         exheader.section_text = FileFormat::ExHeader::CodeSetInfo{text_vaddr, text_numpages, header.text_size};
//         exheader.section_ro = FileFormat::ExHeader::CodeSetInfo{ro_vaddr, ro_numpages, header.ro_size};
//         exheader.section_data = FileFormat::ExHeader::CodeSetInfo{data_vaddr, data_numpages, header.data_bss_size - header.bss_size};
//         exheader.bss_size = header.bss_size;
        exheader.stack_size = 0x4000; // TODO: Should we adjust this to some other value?
        exheader.stack_size = 0x20000; // TODO: Should we adjust this to some other value?
        exheader.unknown3 = {};
        exheader.unknown4 = {};

        exheader.jump_id = {}; // TODO: Consider passing in alt_titleid here
        exheader.save_data_size = {};
        exheader.dependencies = {};
        // Use a set of default dependencies to ensure the system is in a reasonable state when we launch this 3dsx on boot
        std::vector<uint64_t> exheader_dependencies = {
            uint64_t { 0x0004013000008002 /* ns */ },
            uint64_t { 0x0004013000001c02 /* gsp */ },
            uint64_t { 0x0004013000001d02 /* hid */ },
            uint64_t { 0x0004013000001802 /* codec */ },
            uint64_t { 0x0004013000003102 /* ps */ },
            uint64_t { 0x0004013000002202 /* ptm */ },
        };
        ranges::copy(exheader_dependencies, exheader.dependencies.begin());

        exheader.subsignature = {};
        exheader.ncch_sig_pub_key = {};


//         exheader.aci.program_id = alt_titleid;
        exheader.aci.version = 2;
        exheader.aci.flags = FileFormat::ExHeader::ACIFlags{}.flag0()(4).flag1()(3).flag2()(2).priority()(0x30);
        exheader.aci.unknown5 = {};

        ranges::fill(exheader.aci.arm11_kernel_capabilities, FileFormat::ExHeader::ARM11KernelCapabilityDescriptor{0xffffffff});
        using SVCMaskTableEntry = FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::SVCMaskTableEntry;
        using HandleTableInfo = FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::HandleTableInfo;
        using MappedMemoryRange = FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::MappedMemoryRange;
        using KernelFlags = FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::KernelFlags;

        exheader.aci.arm11_kernel_capabilities[0].storage = SVCMaskTableEntry::Make().entry_index()(0).mask()(0xfffffe).storage;
        exheader.aci.arm11_kernel_capabilities[1].storage = SVCMaskTableEntry::Make().entry_index()(1).mask()(0xffffff).storage;
        exheader.aci.arm11_kernel_capabilities[2].storage = SVCMaskTableEntry::Make().entry_index()(2).mask()(0x807fff).storage;
        exheader.aci.arm11_kernel_capabilities[3].storage = SVCMaskTableEntry::Make().entry_index()(3).mask()(0x1ffff).storage;
        exheader.aci.arm11_kernel_capabilities[4].storage = SVCMaskTableEntry::Make().entry_index()(4).mask()(0xef3fff).storage;
        exheader.aci.arm11_kernel_capabilities[5].storage = SVCMaskTableEntry::Make().entry_index()(5).mask()(0x3f).storage;
        exheader.aci.arm11_kernel_capabilities[6].storage = HandleTableInfo::Make().num_handles()(0x200).storage; // TODO: 0x200 handles is somewhat overkill...
        exheader.aci.arm11_kernel_capabilities[7].storage = MappedMemoryRange::Make().page_index()(0x1ff00).storage;
        exheader.aci.arm11_kernel_capabilities[8].storage = MappedMemoryRange::Make().page_index()(0x1ff80).storage;
        exheader.aci.arm11_kernel_capabilities[9].storage = MappedMemoryRange::Make().page_index()(0x1f000).read_only()(1).storage;
        exheader.aci.arm11_kernel_capabilities[10].storage = MappedMemoryRange::Make().page_index()(0x1f600).read_only()(1).storage;
        exheader.aci.arm11_kernel_capabilities[11].storage = KernelFlags::Make().memory_type()(1).storage;

        // Map all IO memory
        exheader.aci.arm11_kernel_capabilities[12].storage = MappedMemoryRange::Make().page_index()(0x1ec00).storage;
        exheader.aci.arm11_kernel_capabilities[13].storage = MappedMemoryRange::Make().page_index()(0x1f000).storage;

        exheader.aci.service_access_list = {};
        const char* default_services[] = {
            "APT:U", "ac:u", "am:net", "boss:U", "cam:u",
            "cecd:u", "cfg:nor", "cfg:u", "csnd:SND",
            "dsp::DSP", "frd:u", "fs:USER", "gsp::Gpu",
            "gsp::Lcd", "hid:USER", "http:C", "ir:rst",
            "ir:u", "ir:USER", "mic:u", "ndm:u",
            "news:s", "nwm::EXT", "nwm::UDS", "ptm:sysm",
            "ptm:u", "pxi:dev", "soc:U", "ssl:C",
            "y2r:u", "pm:app"
        };
//         static_assert(sizeof(default_services) / sizeof(default_services[0]) < exheader.aci.service_access_list.size(), // TODO: Not a constant expression?
//                       "Maximum number of services exhausted");
        ranges::transform(default_services, exheader.aci.service_access_list.begin(),
                          [](auto& service) {
                              std::array<uint8_t, 8> ret{};
                              ranges::copy(service, service + strlen(service), ret.begin());
                              return ret;
                          });

        exheader.aci.unknown6 = {};
        exheader.aci.unknown7 = {};
        // TODO: Not sure what these are about.
        exheader.aci.unknown7[0x10] = 0xff;
        exheader.aci.unknown7[0x11] = 0x3;
        exheader.aci.unknown7[0x1f] = 0x2;
        exheader.aci_limits = exheader.aci;

        // Ideal processor in primary ACI must be smaller or equal to the one in the secondary (TODO: Verify!)
        // Priority in primary ACI must be larger or equal to the one in the secondary (TODO: Verify!)
        exheader.aci_limits.flags = exheader.aci_limits.flags.ideal_processor()(1).priority()(0x18);
    }

    HLE::OS::OS::ResultAnd<> Open(HLE::PXI::FS::FileContext& context, bool create_or_truncate) override {
        if (create_or_truncate) {
            throw std::runtime_error("Can't create/truncate game cards");
        }

        auto ret = file->Open(context, create_or_truncate);
        if (ret != std::tie(HLE::OS::RESULT_OK)) {
            return ret;
        }

        return ret;
    }

    FileFormat::Dot3DSX::Header Read3DSXHeader(HLE::PXI::FS::FileContext& context) {
        // TODO: Use serialization interface instead
        FileFormat::Dot3DSX::Header dot3dsxheader;
        auto ret = file->Read(context, 0, sizeof(dot3dsxheader), HLE::PXI::FS::FileBufferInHostMemory(dot3dsxheader));
        if (ret != HLE::OS::OS::ResultAnd<uint32_t> { HLE::OS::RESULT_OK, sizeof(dot3dsxheader) }) {
            throw std::runtime_error("Failed to read 3DSX header");
        }

        // TODO: Check magic, check header size

        return dot3dsxheader;
    }

    std::optional<FileFormat::Dot3DSX::SecondaryHeader> Read3DSXSecondaryHeader(HLE::PXI::FS::FileContext& context, uint32_t header_size) {
        if (header_size == sizeof(FileFormat::Dot3DSX::Header)) {
            // No secondary header present
            return std::nullopt;
        } else if (header_size != sizeof(FileFormat::Dot3DSX::Header) + sizeof(FileFormat::Dot3DSX::SecondaryHeader)) {
            throw std::runtime_error("Unexpected header size in 3DSX file");
        }

        // TODO: Use serialization interface instead
        FileFormat::Dot3DSX::SecondaryHeader secondary_header;
        auto ret = file->Read(context, sizeof(FileFormat::Dot3DSX::Header), sizeof(secondary_header), HLE::PXI::FS::FileBufferInHostMemory(secondary_header));
        if (ret != HLE::OS::OS::ResultAnd<uint32_t> { HLE::OS::RESULT_OK, sizeof(secondary_header) }) {
            throw std::runtime_error("Failed to read 3DSX header");
        }

        return secondary_header;
    }

    /// Get program code size in bytes
    uint64_t GetProgramCodeSize(const FileFormat::Dot3DSX::Header& dot3dsxheader) const {
        uint32_t text_numpages = ((dot3dsxheader.text_size + 0xfff) >> 12);
        uint32_t ro_numpages = ((dot3dsxheader.ro_size + 0xfff) >> 12);
        uint32_t data_numpages = (((dot3dsxheader.data_bss_size - dot3dsxheader.bss_size) + 0xfff) >> 12);

        return (text_numpages + ro_numpages + data_numpages) << 12;
    }

    std::vector<std::uint8_t> ReadProgramCode(HLE::PXI::FS::FileContext& context, const FileFormat::Dot3DSX::Header& dot3dsxheader) {
        std::array<FileFormat::Dot3DSX::RelocationHeader, 3> relocation_headers;

        std::vector<FileFormat::Dot3DSX::RelocationInfo> relocations;
        uint64_t offset = dot3dsxheader.header_size;
        for (size_t header = 0; header < relocation_headers.size(); ++header) {
            auto& reloc_header = relocation_headers[header];
            auto ret = file->Read(context, offset, sizeof(reloc_header), HLE::PXI::FS::FileBufferInHostMemory(reloc_header));
            if (std::get<0>(ret) != HLE::OS::RESULT_OK) {
                throw std::runtime_error("Failed to read ExeFS data");
            }
            offset += std::get<1>(ret);
        }


        // Read program sections
        uint32_t text_numpages = ((dot3dsxheader.text_size + 0xfff) >> 12);
        uint32_t ro_numpages = ((dot3dsxheader.ro_size + 0xfff) >> 12);
        uint32_t data_numpages = (((dot3dsxheader.data_bss_size - dot3dsxheader.bss_size) + 0xfff) >> 12);

        uint32_t text_vaddr = 0x00100000;
        uint32_t ro_vaddr = text_vaddr + (text_numpages << 12);
        uint32_t data_vaddr = ro_vaddr + (ro_numpages << 12);

        uint32_t total_size = GetProgramCodeSize(dot3dsxheader);
        std::vector<uint32_t> program_data(total_size / 4, 0);
        const std::array<uint32_t*, 3> segment_ptrs = {{ program_data.data(),
                                                        program_data.data() + (ro_vaddr - text_vaddr) / 4,
                                                        program_data.data() + (data_vaddr - text_vaddr) / 4 }};

        // TODO: Endianness!
        {
            auto ret = file->Read(context, offset, dot3dsxheader.text_size,
                                 HLE::PXI::FS::FileBufferInHostMemory(segment_ptrs[0], dot3dsxheader.text_size));
            if (std::get<0>(ret) != HLE::OS::RESULT_OK) {
                throw std::runtime_error("Failed to read ExeFS data");
            }
            offset += std::get<1>(ret);

            ret = file->Read(context, offset, dot3dsxheader.ro_size,
                            HLE::PXI::FS::FileBufferInHostMemory(segment_ptrs[1], dot3dsxheader.ro_size));
            if (std::get<0>(ret) != HLE::OS::RESULT_OK) {
                throw std::runtime_error("Failed to read ExeFS data");
            }
            offset += std::get<1>(ret);

            ret = file->Read(context, offset, dot3dsxheader.data_bss_size - dot3dsxheader.bss_size,
                            HLE::PXI::FS::FileBufferInHostMemory(segment_ptrs[2], dot3dsxheader.data_bss_size - dot3dsxheader.bss_size));
            if (std::get<0>(ret) != HLE::OS::RESULT_OK) {
                throw std::runtime_error("Failed to read ExeFS data");
            }
            offset += std::get<1>(ret);
        }

        // Patch in relocations
        for (unsigned segment = 0; segment < 3; ++segment) {
            auto& reloc_header = relocation_headers[segment];

            // patch_kind = 0: absolute relocation; patch_kind = 1: relative relocation
            for (unsigned patch_kind = 0; patch_kind < 2; ++patch_kind) {
                unsigned num_relocations = (patch_kind==0) ? reloc_header.num_abs_relocs : reloc_header.num_rel_relocs;

                // Pointer to the word currently being patched
                uint32_t* patch_ptr = segment_ptrs[segment];

                for (unsigned relocation = 0; relocation < num_relocations; ++relocation) {
                    FileFormat::Dot3DSX::RelocationInfo reloc_info;
                    auto ret = file->Read(context, offset, sizeof(reloc_info),
                                         HLE::PXI::FS::FileBufferInHostMemory(reloc_info));
                    if (std::get<0>(ret) != HLE::OS::RESULT_OK) {
                        throw std::runtime_error("Failed to read ExeFS data");
                    }
                    offset += std::get<1>(ret);

                    patch_ptr += reloc_info.words_to_skip;
                    for (unsigned word_index = 0; word_index < reloc_info.words_to_patch; ++word_index) {
                        // TODO: Make sure patch_ptr is still within the current segment!

                        uint32_t unpatched_word = *patch_ptr;
                        uint32_t virtual_address = std::invoke([=]() {
                            // Translate offset given by unpatched_word into the target virtual address
                            auto offset = (unpatched_word & 0x0FFFFFFF);
                            auto numbytes_text = (text_numpages << 12);
                            auto numbytes_text_and_ro = (text_numpages << 12) + (ro_numpages << 12);
                            if (offset < numbytes_text) {
                                return offset + text_vaddr;
                            } else if (offset < numbytes_text_and_ro) {
                                return offset + ro_vaddr - numbytes_text;
                            } else {
                                return offset + data_vaddr - numbytes_text_and_ro;
                            }
                        });
                        uint32_t sub_type = unpatched_word >> 28;

                        if (patch_kind == 0) {
                            assert(sub_type == 0);
                            *patch_ptr = virtual_address;
                        } else {
                            assert(sub_type < 2);

                            uint32_t offset = text_vaddr + sizeof(*patch_ptr) * (patch_ptr - segment_ptrs[0]);
                            if (sub_type == 0) {
                                *patch_ptr = virtual_address - offset;
                            } else {
                                *patch_ptr = (virtual_address - offset) & 0x7fffffff;
                            }
                        }

                        patch_ptr++;
                    }
                }
            }
        }

        // TODO: Eh.
        std::vector<uint8_t> program_data8(total_size, 0);
        std::memcpy(program_data8.data(), program_data.data(), total_size);
        return program_data8;
    }

    HLE::OS::OS::ResultAnd<uint32_t> Read(HLE::PXI::FS::FileContext& context, uint64_t offset, uint32_t num_bytes, HLE::PXI::FS::FileBuffer&& dest) override {
        FileFormat::Dot3DSX::Header dot3dsxheader = Read3DSXHeader(context);
        std::optional<FileFormat::Dot3DSX::SecondaryHeader> dot3dsx_secondary_header = Read3DSXSecondaryHeader(context, dot3dsxheader.header_size);

        // Build ExeFS header
        FileFormat::ExeFSHeader exefsheader = {};
        ranges::copy(".code", exefsheader.files[0].name.begin());
        exefsheader.files[0].size_bytes = GetProgramCodeSize(dot3dsxheader);

        // Offsets within the exposed NCCH file
        const uint64_t ncch_offset = 0;
        const uint64_t exheader_offset = ncch_offset + sizeof(FileFormat::NCCHHeader);
        const uint64_t exefsheader_offset = exheader_offset + sizeof(FileFormat::ExHeader); // TODO: ncch.exefs_offset.ToBytes() ?
        const uint64_t exefsdata_offset = exefsheader_offset + sizeof(FileFormat::ExeFSHeader);

        // +0x1ff: Round up to the next media unit size
        ncch.exefs_offset = FileFormat::MediaUnit32::FromBytes(exefsheader_offset);
        ncch.exefs_size = FileFormat::MediaUnit32::FromBytes(exefsheader.GetExeFSSize() + 0x1ff);
        if (dot3dsx_secondary_header) {
            ncch.romfs_offset = FileFormat::MediaUnit32::FromBytes(0x1ff + ncch.exefs_offset.ToBytes() + ncch.exefs_size.ToBytes());
            auto [result, total_size] = file->GetSize(context);
            if (result != HLE::OS::RESULT_OK) {
                throw std::runtime_error("Failed to get own file size");
            }
            ncch.romfs_size = FileFormat::MediaUnit32::FromBytes(total_size - dot3dsx_secondary_header->romfs_offset + 0x1ff + 0x1000); // Offset by 0x1000 to get to the level3 image (the only part contained in 3dsx files)
        }

        if (offset >= ncch_offset && offset + num_bytes <= exheader_offset) {
            // TODO: Use FileFormat::Save instead!
            dest.Write(reinterpret_cast<char*>(&ncch) + (offset - ncch_offset), num_bytes);
        } else if (offset >= exheader_offset && offset < exheader_offset + sizeof(exheader)) {
            if (offset != exheader_offset || num_bytes != sizeof(exheader)) {
                throw std::runtime_error("Cannot partially read extended header for 3DSX files currently");
            }

            // Patch 3DSX data
            uint32_t text_numpages = ((dot3dsxheader.text_size +0xfff) >> 12);
            uint32_t ro_numpages = ((dot3dsxheader.ro_size+0xfff) >> 12);
            uint32_t data_numpages = (((dot3dsxheader.data_bss_size - dot3dsxheader.bss_size) + 0xfff) >> 12);

            uint32_t text_vaddr = 0x00100000;
            uint32_t ro_vaddr = text_vaddr + (text_numpages << 12);
            uint32_t data_vaddr = ro_vaddr + (ro_numpages << 12);

            ranges::copy("3DSXCart", exheader.application_title.begin());
            exheader.unknown = {};
            exheader.flags = {};//inject_target.exheader.flags.compress_exefs_code().Set(0);
            exheader.remaster_version = {};
            exheader.section_text = FileFormat::ExHeader::CodeSetInfo{text_vaddr, text_numpages, dot3dsxheader.text_size };
            exheader.section_ro = FileFormat::ExHeader::CodeSetInfo{ro_vaddr, ro_numpages, dot3dsxheader.ro_size };
            exheader.section_data = FileFormat::ExHeader::CodeSetInfo{data_vaddr, data_numpages, dot3dsxheader.data_bss_size - dot3dsxheader.bss_size };
            exheader.bss_size = dot3dsxheader.bss_size;
            exheader.stack_size = 0x4000; // TODO: Should we adjust this to some other value?
            exheader.unknown3 = {};
            exheader.unknown4 = {};
            // TODO: Use FileFormat::Save instead!
            dest.Write(reinterpret_cast<char*>(&exheader), sizeof(exheader));
        } else if (offset >= exefsheader_offset && offset < exefsheader_offset + sizeof(exefsheader)) {
            if (offset != exefsheader_offset || num_bytes != sizeof(exefsheader)) {
                throw Mikage::Exceptions::NotImplemented(   "Cannot partially read ExeFS header for 3DSX files currently (header at {:#x}-{:#x}, requested {:#x}-{:#x})",
                                                            exefsheader_offset, exefsheader_offset + sizeof(exefsheader), offset, offset + num_bytes);
            } else {
                // TODO: Use FileFormat::Save instead!
                dest.Write(reinterpret_cast<char*>(&exefsheader), sizeof(exefsheader));
            }
        } else if (offset >= exefsdata_offset && offset < exefsdata_offset + GetProgramCodeSize(dot3dsxheader)) {
            if (offset != exefsdata_offset || num_bytes != GetProgramCodeSize(dot3dsxheader)) {
                throw std::runtime_error("Cannot partially read ExeFS data for 3DSX files currently");
            }

            auto program_code = ReadProgramCode(context, dot3dsxheader);
            dest.Write(reinterpret_cast<char*>(program_code.data()), program_code.size());
        } else if (offset >= ncch.romfs_offset.ToBytes() + 0x1000 && offset + num_bytes <= ncch.romfs_offset.ToBytes() + ncch.romfs_size.ToBytes()) {
            if (!dot3dsx_secondary_header) {
                throw std::runtime_error("Tried reading RomFS even though no RomFS is present");
            }

            if (offset - ncch.romfs_offset.ToBytes() < 0x1000) {
                // TODO: offset_within_romfs would end up negative in this case, but I'm not sure this condition will ever be hit in practice.
                throw std::runtime_error("Unimplemented case");
            }
            auto offset_within_romfs = offset - ncch.romfs_offset.ToBytes() - 0x1000; // Offset by 0x1000 to get to the level3 image (the only part contained in 3dsx files)
            file->Read(context, dot3dsx_secondary_header->romfs_offset + offset_within_romfs, num_bytes, std::move(dest));
        } else {
            throw std::runtime_error(fmt::format("Unsupported read range: Tried reading {:#x} bytes from offset {:#x}", num_bytes, offset));
        }

        return std::make_tuple(HLE::OS::RESULT_OK, num_bytes);
    }

    HLE::OS::OS::ResultAnd<uint64_t> GetSize(HLE::PXI::FS::FileContext& context) override {
        // Return original file size as upper bound; this is merely needed to prevent the bounds check in FileViews from failing
        return file->GetSize(context);
    }

    void Close() override {
        file->Close();
    }
};

} // end of anonymous namespace

GameCardFrom3DSX::GameCardFrom3DSX(std::string_view filename) : source(std::string { filename }) {
}

GameCardFrom3DSX::GameCardFrom3DSX(int file_descriptor) : source(file_descriptor) {
}

std::optional<std::unique_ptr<HLE::PXI::FS::File>> GameCardFrom3DSX::GetPartitionFromId(Loader::NCSDPartitionId id) {
    if (id == NCSDPartitionId::Executable) {
        auto file = new Adaptor3DSXToNCCH(std::visit(GameCardSourceOpener{}, source));
        return std::unique_ptr<HLE::PXI::FS::File>(file);
    } else {
        throw std::runtime_error("Not implemented yet");
        return std::nullopt;
    }
}

static bool IsLoadable3DSXFile(HLE::PXI::FS::File& file) {
    // TODO: Enable proper logging
    auto logger = std::make_shared<spdlog::logger>("dummy", std::make_shared<spdlog::sinks::null_sink_st>());
    auto file_context = HLE::PXI::FS::FileContext { *logger };
    file.Open(file_context, false);

    FileFormat::Dot3DSX::Header header;
    auto [result, bytes_read] = file.Read(file_context, 0, sizeof(header), HLE::PXI::FS::FileBufferInHostMemory(header));
    if (result != HLE::OS::RESULT_OK || bytes_read != sizeof(header)) {
        return false;
    }
    return ranges::equal(header.magic, std::string_view { "3DSX" });
}

bool GameCardFrom3DSX::IsLoadableFile(std::string_view filename) {
    auto file = HLE::PXI::FS::HostFile(filename, {});
    return IsLoadable3DSXFile(file);
}

bool GameCardFrom3DSX::IsLoadableFile(int file_descriptor) {
    return IsLoadable3DSXFile(*FileSystem::OpenNativeFile(file_descriptor));
}

} // namespace Loader
