#include <os.hpp>

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace HLE {

namespace OS {
class Thread;
}
namespace IPC {
struct StaticBuffer;
}
namespace PXI {
struct PXIBuffer;
}

// Produce error codes on invalid API usage instead of throwing exceptions.
// Used for the test suite, but disabled by default since we expect games to
// behave nicely more often than not.
// TODO: Expose this through a Setting instead
inline constexpr bool test_mode = true;

struct Utf8PathType : std::string {
    std::filesystem::path to_std_path() const {
        return std::filesystem::path { static_cast<const std::string&>(*this) };
    }
};
struct BinaryPathType : std::vector<uint8_t> { };

/**
 * Helper class for unified path handling.
 *
 * Non-binary paths are automatically converted to Utf-8, with trailing \0 characters removed.
 */
class CommonPath {
    static constexpr uint32_t max_length = 256; // Unverified. TODO: What's the actual maximum?

    std::variant<Utf8PathType, BinaryPathType> data;

    static CommonPath FromUtf16(std::u16string_view utf16_data);
    static CommonPath FromUtf16(std::basic_string_view<uint16_t> utf16_data);

    CommonPath(decltype(data) raw) : data(raw) {
    }

    template<typename ReadByte>
    CommonPath(uint32_t type, uint32_t num_bytes, ReadByte&& read);

public:
    CommonPath(OS::Thread& thread, uint32_t type, uint32_t num_bytes, const IPC::StaticBuffer&);
    CommonPath(OS::Thread& thread, uint32_t type, uint32_t num_bytes, const PXI::PXIBuffer&);

    template<typename Utf8Handler, typename BinaryHandler>
    auto Visit(Utf8Handler&& utf8_handler, BinaryHandler&& bin_handler) const {
        return std::visit(  [&](const auto& arg) {
                                if constexpr (std::is_invocable_v<Utf8Handler, decltype(arg)>) {
                                    return std::forward<Utf8Handler>(utf8_handler)(arg);
                                } else {
                                    return std::forward<BinaryHandler>(bin_handler)(arg);
                                }
                            },
                            data);
    }
};

/**
 * Absolute path verified to be within some sandbox path.
 *
 * As long as the "path" member isn't modified, it's save to recursively delete
 * data from this path without harming the host filesystem
 */
struct ValidatedHostPath {
    std::filesystem::path path;

    operator const std::filesystem::path&() const {
        return path;
    }

private:
    ValidatedHostPath(std::filesystem::path path) : path(std::move(path)) {}
    friend class PathValidator;
};

class PathValidator {
protected:
    struct ErrFileNotFound {};
    struct ErrDirectoryNotFound {};
    struct ErrFileAlreadyExists {};
    struct ErrDirectoryAlreadyExists {};

    ValidatedHostPath ValidateAndGetSandboxedTreePath(const Utf8PathType&) const;
    virtual std::filesystem::path GetSandboxHostRoot() const = 0;
};

std::filesystem::path GetRootDataDirectory(Settings::Settings& settings);

} // namespace HLE
