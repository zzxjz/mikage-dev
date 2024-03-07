#include <framework/formats.hpp>
#include <platform/file_formats/ncch.hpp>

#include <range/v3/algorithm/find.hpp>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

#include <fmt/fmt.h>

#include <iostream>
#include <fstream>

int main(int argc, char* argv[]) {
    std::string in_filename;
    std::string target_path;
    bool force_overwrite;

    {
        namespace bpo = boost::program_options;

        bpo::options_description desc;
        desc.add_options()
            ("help,h", "Print this help")
            ("input,i", bpo::value<std::string>(&in_filename)->required(), "Path to input NCCH file")
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

    auto [header, exheader] = Meta::invoke([&]() {
        std::ifstream file(in_filename, std::ios_base::binary);
        file.exceptions(std::ifstream::badbit | std::ifstream::failbit | std::ifstream::eofbit);

        auto strbuf_it = std::istreambuf_iterator<char>(file.rdbuf());
        auto stream = FileFormat::MakeStreamInFromContainer(strbuf_it, decltype(strbuf_it){});
        auto header = FileFormat::Load<FileFormat::NCCHHeader>(stream);

        // NOTE: The given exheader_size only refers to the hashed exheader region rather than the full exheader size
        assert(header.exheader_size <= FileFormat::ExHeader::Tags::expected_serialized_size);

        auto exheader = FileFormat::Load<FileFormat::ExHeader>(stream);

        return std::make_tuple(header, exheader);
    });

    //exheader.application_title;
    std::string title(exheader.application_title.begin(), exheader.application_title.end());
    title.resize(ranges::find(title, '\0') - ranges::begin(title));
    if (title.empty())
        title = "(no_title)";
    uint32_t titleid_high = header.program_id >> 32;
    uint32_t titleid_low = static_cast<uint32_t>(header.program_id);

    auto out_filename = boost::filesystem::path(target_path);
    out_filename += boost::filesystem::path(fmt::format("./{:08x}/{:08x}/content/", titleid_high, titleid_low));
    boost::filesystem::create_directories(out_filename);
    out_filename += boost::filesystem::path("./00000000.cxi");

    boost::filesystem::copy(in_filename, out_filename);

    std::cout << fmt::format("{:08x} {:08x} {} {}", titleid_high, titleid_low, title, out_filename.c_str()) << std::endl;
}
