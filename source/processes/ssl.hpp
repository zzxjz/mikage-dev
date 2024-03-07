#pragma once

#include "fake_process.hpp"

namespace HLE {

namespace OS {

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<struct FakeSSL>(OS&, Interpreter::Setup&, uint32_t pid, const std::string& name);

}  // namespace OS

}  // namespace HLE
