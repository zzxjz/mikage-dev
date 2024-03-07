#pragma once

#include "fake_process.hpp"

namespace HLE {

namespace OS {

// Creates a dummy process that immediately exits
template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<struct DummyProcess>(OS&, Interpreter::Setup&, uint32_t pid, const std::string& name);

}  // namespace OS

}  // namespace HLE
