#pragma once

#include <boost/filesystem/path.hpp>

namespace HLE {

class FakeFS;
using FSContext = FakeFS;

inline uint64_t GetId0(FSContext&) {
    // TODO
    return 0;
}

inline uint64_t GetId1(FSContext&) {
    // TODO
    return 0;
}

} // namespace HLE
