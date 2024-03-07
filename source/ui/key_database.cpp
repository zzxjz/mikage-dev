#include "key_database.hpp"

#include <framework/exceptions.hpp>

#include <spdlog/spdlog.h>

#include <string>
#include <fstream>

#include <boost/spirit/home/x3.hpp>

KeyDatabase LoadKeyDatabase(spdlog::logger& logger, const std::filesystem::path& filename) {
    KeyDatabase ret;

    std::ifstream file(filename);
    if (!file) {
        throw Mikage::Exceptions::InvalidUserInput("Failed to load crypto keys: Could not open {}", filename.string());
    }

    std::string line;
    while (std::getline(file, line)) {
        namespace x3 = boost::spirit::x3;

        auto parse_match = [&](auto& ctx) {
            auto& attrs = x3::_attr(ctx);
            const uint32_t key_index = at_c<0>(attrs);
            const char key_type = at_c<1>(attrs);
            const std::vector<uint8_t>& key = at_c<2>(attrs);
            const auto max_key_index = (key_type == 'C' ? std::tuple_size_v<decltype(KeyDatabase::common_y)> : KeyDatabase::num_aes_slots);

            if (key_index >= max_key_index) {
                throw Mikage::Exceptions::InvalidUserInput("Invalid key slot index {:#x} in line \"{}\" of {}", key_index, line, filename.string());
            }

            if (key.size() != std::tuple_size_v<KeyDatabase::KeyType>) {
                throw Mikage::Exceptions::InvalidUserInput("Invalid key in line \"{}\" of {} (should be 32 hex digits)", line, filename.string());
            }

            auto& dest_key = key_type == 'X' ? ret.aes_slots[key_index].x
                            : key_type == 'Y' ? ret.aes_slots[key_index].y
                            : key_type == 'N' ? ret.aes_slots[key_index].n
                            : key_type == 'C' ? ret.common_y[key_index]
                            : throw Mikage::Exceptions::InvalidUserInput("Invalid key type '{}' in line \"{}\" of {}", key_type, line, filename.string());

            dest_key = KeyDatabase::KeyType {};
            memcpy(dest_key.value().data(), key.data(), sizeof(dest_key.value()));
        };

        auto pattern_aes = x3::lit("slot0x") >> x3::uint_parser<uint32_t, 16, 2, 2>() >> x3::lit("Key") >> x3::alpha >> x3::lit("=") >> x3::repeat(16)[x3::uint_parser<uint8_t, 16, 2, 2>()];
        auto pattern_common = x3::lit("common") >> x3::uint_parser<uint32_t, 10, 1>() >> x3::attr('C') >> x3::lit("=") >> x3::repeat(16)[x3::uint_parser<uint8_t, 16, 2, 2>()];
        if (x3::parse(line.begin(), line.end(), pattern_aes[parse_match])) {
        } else if (x3::parse(line.begin(), line.end(), pattern_common[parse_match])) {
        } else if (line.empty() || line[0] == '#') {
            // Ignore
        } else if (line.starts_with("dlp") || line.starts_with("nfc")) {
            // Not currently used
        } else {
            logger.warn("Ignoring line in {}: {}", filename.string(), line);
        }
    }

    return ret;
}
