#pragma once

#include "framework/console.hpp"

namespace HLE {

namespace OS {

class OS;

class ConsoleModule : public ::ConsoleModule {
    OS& os;

public:
    ConsoleModule(OS& os);
    virtual ~ConsoleModule() = default;

    virtual std::optional<std::string> HandleCommand(const std::string& command) override;
};

}  // namespace OS

}  // namespace HLE
