#pragma once

#include "fake_process.hpp"

namespace HLE::OS {

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<struct FakeErrorDisp>(OS&, Interpreter::Setup&, uint32_t, const std::string&);

}  // namespace HLE::OS
