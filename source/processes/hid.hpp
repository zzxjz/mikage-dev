#pragma once

#include "fake_process.hpp"

namespace HLE {

namespace OS {

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<struct FakeHID>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name);

}  // namespace OS

}  // namespace HLE
