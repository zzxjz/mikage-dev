#pragma once

namespace HLE {

namespace OS {

class FakeThread;

struct FakeNEWS {
    FakeNEWS(FakeThread& thread);
};

}  // namespace OS

}  // namespace HLE
