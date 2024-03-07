#include "firm.hpp"

namespace Loader {

bool IsFirm(std::istream& str) {
    auto file_begin = str.tellg();

    unsigned char magic[4];
    str.read(reinterpret_cast<char*>(magic), sizeof(magic));
    str.seekg(file_begin);

    if (str.gcount() != sizeof(magic) ||
        magic[0] != 'F' ||
        magic[1] != 'I' ||
        magic[2] != 'R' ||
        magic[3] != 'M') {
        return false;
    }

    return true;
}

}  // namespace Loader
