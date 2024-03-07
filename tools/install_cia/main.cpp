#include <framework/formats.hpp>
#include <framework/meta_tools.hpp>
#include <platform/file_formats/3dsx.hpp>
#include <platform/file_formats/cia.hpp>
#include <platform/file_formats/ncch.hpp>

#include <fmt/format.h>

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

#include <boost/filesystem.hpp>

#include <boost/hana/functional/overload.hpp>

#include <boost/program_options.hpp>

#include <iostream>
#include <fstream>
#include <cstdint>
#include <iterator>
#include <iomanip>

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

struct CIA {
    FileFormat::CIAHeader header;

    std::array<FileFormat::Certificate, 3> certificates;
    FileFormat::Ticket ticket;
    FileFormat::TMD tmd;

    std::vector<uint8_t> ncch;

    FileFormat::CIAMeta meta;
};

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

        if (content_info.type & 1) {
            throw std::runtime_error("Encrypted CIA contents are unsupported");
        }

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
    ret.ncch.resize(cia.content_size);
    file.read(reinterpret_cast<char*>(ret.ncch.data()), ret.ncch.size());

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

int main(int argc, char* argv[]) {
    std::string in_filename;
    std::string target_path;
    bool force_overwrite;

    {
        namespace bpo = boost::program_options;

        bpo::options_description desc;
        desc.add_options()
            ("help,h", "Print this help")
            ("input,i", bpo::value<std::string>(&in_filename)->required(), "Path to input CIA file")
            ("base,b", bpo::value<std::string>(&target_path)->required(), "Path to target NAND tree")
            ("force,f", bpo::bool_switch(&force_overwrite)->default_value(false), "Force overwrite if any output path already exists");

        try {
            auto positional_arguments = bpo::positional_options_description{}.add("input", -1);
            bpo::variables_map var_map;
            bpo::store(bpo::command_line_parser(argc, argv).options(desc).positional(positional_arguments).run(),
                       var_map);
            if (var_map.count("help") ) {
                std::cout << desc << std::endl;
                return 0;
            }
            bpo::notify(var_map);
        } catch (bpo::error error) {
            std::cout << desc << std::endl;
            return 1;
        }
    }

    if (!boost::filesystem::exists(target_path)) {
        std::cout << "Target base directory does not exist!" << std::endl;
        return 1;
    }

    std::cout << "Attempting to open \"" << in_filename << "\" for reading" << std::endl;
    std::ifstream file;
    file.exceptions(std::ifstream::badbit | std::ifstream::failbit | std::ifstream::eofbit);
    file.open(in_filename, std::ios_base::binary);

    auto cia = ParseCIA(file);
    auto&& tmd = cia.tmd;

    uint32_t content_id = 0; // TODO: Should read this from the tmd instead

    // TODO: Change extension to ".app" once we support this!
    boost::filesystem::path content_filename = fmt::format("{}/{:08x}/{:08x}/content/{:08x}.cxi", target_path, tmd.data.title_id >> 32, tmd.data.title_id & 0xffffffff, content_id);

    std::cout << "Attempting to create " << content_filename << "" << std::endl;
    if (!force_overwrite && boost::filesystem::exists(content_filename)) {
        std::cout << "File already exists!" << std::endl;
        return 1;
    }

    try {
        (void)boost::filesystem::create_directories(content_filename.parent_path());
    } catch (boost::filesystem::filesystem_error& err) {
        std::cout << "Couldn't create directory structure: " << err.what() << std::endl;
        return 1;
    }

    std::ofstream ofile;
    ofile.exceptions(std::ifstream::badbit | std::ifstream::failbit | std::ifstream::eofbit);
    ofile.open(content_filename, std::ios_base::binary);
    ofile.write(reinterpret_cast<char*>(cia.ncch.data()), cia.ncch.size());
    ofile.close();

    // TODO: Also copy over tmd once we support that
    // TODO: Also generate cmd once we support that
    std::cout << "Success!" << std::endl;
}
