#pragma once

#include "fake_process.hpp"

namespace HLE {

namespace OS {

class FakeThread;

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<struct FakeCAM>(OS&, Interpreter::Setup&, uint32_t, const std::string&);

}  // namespace OS

}  // namespace HLE
