#pragma once

#include "os.hpp"

#include <string>

namespace HLE {

namespace OS {

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<struct FakeDLP>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name);

}  // namespace OS

}  // namespace HLE
