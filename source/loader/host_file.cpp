#include "host_file.hpp"

#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/operations.hpp>

namespace FileSystem {

class NativeFile final : public HLE::PXI::FS::File {
public:
    boost::iostreams::file_descriptor_source source;

public:
    NativeFile(int file_descriptor) : source(file_descriptor, boost::iostreams::never_close_handle) {
    }

    HLE::OS::ResultAnd<> Open(HLE::PXI::FS::FileContext&, bool create) override {
        // Nothing to do
        return std::make_tuple(HLE::OS::RESULT_OK);
    }

    HLE::OS::ResultAnd<uint64_t> GetSize(HLE::PXI::FS::FileContext&) override {
        auto begin = boost::iostreams::seek(source, 0, std::ios_base::beg);
        auto end = boost::iostreams::seek(source, 0, std::ios_base::end);
        return std::make_tuple(HLE::OS::RESULT_OK, static_cast<uint64_t>(end - begin));
    }

    HLE::OS::ResultAnd<uint32_t> Read(HLE::PXI::FS::FileContext& context, uint64_t offset, uint32_t num_bytes, HLE::PXI::FS::FileBuffer&& dest) override {
        boost::iostreams::seek(source, offset, std::ios_base::beg);

        std::vector<char> data;
        data.resize(num_bytes);
        boost::iostreams::read(source, data.data(), num_bytes);
        dest.Write(data.data(), num_bytes);
        return std::make_tuple(HLE::OS::RESULT_OK, num_bytes);
    }

    void Close() override {

    }
};

std::unique_ptr<HLE::PXI::FS::File> OpenNativeFile(int file_desctiptor) {
    return std::unique_ptr<HLE::PXI::FS::File>(new NativeFile(file_desctiptor));
}

} // namespace FileSystem
