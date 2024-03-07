#pragma once

namespace HLE {

namespace OS {

class FakeThread;

struct FakeHTTP {
    FakeHTTP(FakeThread& thread);
};

}  // namespace OS

}  // namespace HLE
