#pragma once

namespace HLE {

namespace OS {

class FakeThread;

struct FakeFRIEND {
    FakeFRIEND(FakeThread& thread);
};

}  // namespace OS

}  // namespace HLE
