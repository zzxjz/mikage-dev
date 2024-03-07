#pragma once

#include <framework/formats.hpp>

#include <boost/hana/define_struct.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace FileFormat {

struct CIAHeader {
    BOOST_HANA_DEFINE_STRUCT(CIAHeader,
        (uint32_t, header_size),
        (uint16_t, type), // TODO: Enumize
        (uint16_t, version),
        (uint32_t, certificate_chain_size), // in bytes
        (uint32_t, ticket_size),
        (uint32_t, tmd_size),
        (uint32_t, meta_size),
        (uint64_t, content_size)
    );

    // Followed by:
    // * 0x2000 bytes of content mask data
    // * Certificate chain (first one verifying the other two certificates, second one verifying the ticket signature, third one verifying the tmd signature)
    // * Ticket (per-title "permission" to launch the title, including the encrypted titlekey)
    // * Title Meta Data
    // * content data (NCCHs or SRLs)
    //     * these are encrypted using 128-bit AES-CBC with the titlekey iff the corresponding flag is set in the TMD (3dbrew erratum: This means they need not always be encrypted, although in practice they are for official content)
    // * meta file data

    // NOTE: This is indeed little-endian according to 3dbrew...
    struct Tags : little_endian_tag, expected_size_tag<0x20> {};
};

struct Signature {
    struct Tags : big_endian_tag {};

    uint32_t type; // TODO: Enumize
    std::vector<uint8_t> data; // padded such that the CertificateSignature ends at multiples of 0x40 bytes

    // TODO: Merge the signature issue into this struct!
};

static size_t GetSignatureSize(uint32_t signature_type) {
    switch (signature_type) {
    case 0x10000:
    case 0x10003:
        return 0x200;

    case 0x10001:
    case 0x10004:
        return 0x100;

    case 0x10002:
    case 0x10005:
        return 0x3c;

    default:
        throw std::runtime_error("Unknown signature type " + std::to_string(signature_type));
    }
}

// TODO: Consider BOOST_HANA_DEFINE_STRUCT-izing this union
// NOTE: Possible 3dbrew erratum: Improper padding for these structs. they should be padded to 0x40 bytes
union CertificatePublicKey {
    template<size_t bytes>
    struct RSAKey {
        BOOST_HANA_DEFINE_STRUCT(RSAKey<bytes>,
            (std::array<uint8_t, bytes>, modulus),
            (std::array<uint8_t, 4>, exponent), // TODO: Is this actually a uint32_t?
            (std::array<uint8_t, 0x34 + (bytes == 2057/8) * 0x100>, padding) // TODO: Figure out whether the 2048 key is padded like this...
        );
    };

    RSAKey<2048/8> rsa2048;
    RSAKey<4096/8> rsa4096;

    struct ECCKey {
        BOOST_HANA_DEFINE_STRUCT(ECCKey,
            (std::array<uint8_t, 0x3c>, key),
            (std::array<uint8_t, 0x3c>, padding)
        );
    } ecc;

    // Must be padded like this to guarantee the overall certificate data adds up to multiples of 0x40 bytes
    static_assert((sizeof(rsa2048) % 0x40) == 0x38);
    static_assert((sizeof(rsa4096) % 0x40) == 0x38);
    static_assert((sizeof(ecc) % 0x40) == 0x38);
};

static size_t GetCertificatePublicKeySize(uint32_t type) {
    switch (type) {
    case 0: return sizeof(std::declval<CertificatePublicKey>().rsa4096);
    case 1: return sizeof(std::declval<CertificatePublicKey>().rsa2048);
    case 2: return sizeof(std::declval<CertificatePublicKey>().ecc);
    default:
        throw std::runtime_error("Unknown certificate public key type " + std::to_string(type));
    }
}

struct Certificate {
    Signature sig;

    struct Data {
        BOOST_HANA_DEFINE_STRUCT(Data,
            (std::array<uint8_t, 0x40>, issuer),
            (uint32_t, key_type),
            (std::array<uint8_t, 0x40>, name),
            (uint32_t, unknown)
        );

        struct Tags : big_endian_tag {};
    } cert;

    CertificatePublicKey pubkey;
};

struct TicketTimeLimit {
    BOOST_HANA_DEFINE_STRUCT(TicketTimeLimit,
        (uint32_t, enable),
        (uint32_t, limit_in_seconds)
    );
};

struct Ticket {
    Signature sig;

    struct Data {
        BOOST_HANA_DEFINE_STRUCT(Data,
            (std::array<unsigned char, 0x40>, issuer),
            (std::array<uint8_t, 0x3c>, signature_pubkey), // Only for ECDSA mode?
            (uint8_t, version), // 1 for 3DS
            (std::array<uint8_t, 2>, unknown),
            // The title key is encrypted using a keyscrambler pair of AES keyslot 0x3D keyX and a keyY selected by key_y_index
            (std::array<uint8_t, 0x10>, title_key_encrypted),
            (uint8_t, unknown2),
            (uint64_t, ticket_id),
            (uint32_t, console_id),
            (uint64_t, title_id),
            (uint16_t, sys_access), // ???
            (uint16_t, title_version),
            (uint32_t, time_mask), // ???
            (uint32_t, permit_mask), // ???
            (uint8_t, title_export), // called "license type" on 3dbrew???
            // index into a Process9-internal table of keyYs used to de/encrypt the title key
            (uint8_t, key_y_index),
            (std::array<uint8_t, 0x72>, unknown3),
            (std::array<TicketTimeLimit, 8>, time_limit),
            (std::array<uint8_t, 0xac>, unknown4) // "Content Index" on 3dbrew
        );

        struct Tags : big_endian_tag, expected_size_tag<0x210> {};
    } data;
};

/**
 * Title Meta Data
 */
struct TMD {
    Signature sig;

    // Semantics need to be verified
    struct ContentInfoHash {
        BOOST_HANA_DEFINE_STRUCT(ContentInfoHash,
            (uint16_t, index_offset), // ???
            (uint16_t, chunk_count),
            (std::array<uint8_t, 0x20>, sha256) // hash over the next "chunk_count" content infos
        );

        struct Tags : big_endian_tag, expected_size_tag<0x24> {};
    };

    // Semantics need to be verified
    struct ContentInfo {
        BOOST_HANA_DEFINE_STRUCT(ContentInfo,
            // ID of the described content. Used to name .app files in the NAND filesystem
            (uint32_t, id),
            // 0=Main application, 1=Manual, 2=DLP child container ... not sure if this is by convention! 3dbrew suggests it might be used as an index into the CIA contents
            (uint16_t, index),
            (uint16_t, type), // Collection of flags: encrypted, disc, ...
            (uint64_t, size), // in bytes
            (std::array<uint8_t, 0x20>, sha256)
        );

         struct Tags : big_endian_tag, expected_size_tag<0x30> {};
    };

    struct Data {
        BOOST_HANA_DEFINE_STRUCT(Data,
            (std::array<unsigned char, 0x40>, issuer),
            (uint8_t, version), // Always 1?
            (uint8_t, ca_crl_version), // Version for certificate revocation list
            (uint8_t, signer_crl_version), // ???
            (uint8_t, unknown),
            (uint64_t, system_version),
            (uint64_t, title_id),
            (uint32_t, title_type), // ???
            (uint16_t, group_id), // ???
            (uint32_t, save_data_size), // in bytes; used for SRL Public Save Data Size for DS titles
            (uint32_t, srl_private_data_size),
            (uint32_t, unknown2),
            (uint8_t, srl_flag),
            (std::array<uint8_t, 0x31>, unknown3),
            (uint32_t, access_rights),
            (uint16_t, title_version),
            (uint16_t, content_count), // Number of encrypted NCCHs used by this title
            (uint16_t, main_content), // NCCH to boot upon launch
            (uint16_t, unknown4),
            (std::array<uint8_t, 0x20>, content_info_records_sha256) // Hash over content_info_hashes
        );

        struct Tags : big_endian_tag, expected_size_tag<0xC4> {};
    } data;

    // NOTE: Due to the convoluted hashing involved in generating a TMD,
    //       it is more practical to move these out of the Data struct
    std::array<ContentInfoHash, 64> content_info_hashes;

    // Followed by content_count of these:
    // TODO: Are these indeed dynamically sized?
    std::vector<ContentInfo> content_infos;
};

struct CIAMeta {
    BOOST_HANA_DEFINE_STRUCT(CIAMeta,
        /// List of title IDs this application depends on
        (std::array<uint64_t, 0x30>, dependencies),
        (std::array<uint8_t, 0x180>, unknown),
        (uint32_t, core_version),
        (std::array<uint8_t, 0xfc>, unknown2),
        (std::array<uint8_t, 0x36c0>, icon_data)
    );
};

} // namespace FileFormat
