#pragma once

#include "os.hpp"

#include <memory>

namespace HLE {

namespace OS {

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<struct FakeNDM>(OS&, Interpreter::Setup&, uint32_t pid, const std::string& name);

}  // namespace OS

}  // namespace HLE

