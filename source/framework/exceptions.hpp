#pragma once

#include <cassert>
#include <stdexcept>

#include <fmt/core.h>

namespace Mikage {

namespace Exceptions {

std::string generate_backtrace();

/**
 * Exception to throw upon violation internal contracts that are expected to
 * hold regardless of invalid emulation or user inputs.
 *
 * ValidateContract provides a drop-in replacement for assert that throws
 * exceptions of this kind.
 */
struct ContractViolated : std::runtime_error {
    ContractViolated(std::string_view condition, std::string_view function, std::string_view file, int line)
        : std::runtime_error(FormatMessage(condition, function, file, line) + generate_backtrace()) {
    }

    static std::string FormatMessage(std::string_view condition, std::string_view function, std::string_view file, int line);
};

/**
 * Exception to throw when the given situation is valid but handling it is not
 * implemented currently.
 *
 * Examples of this are unhandled (but valid) enum inputs to emulated APIs or
 * unhandled corner cases in internal libraries.
 *
 * This exception may also be used in situations where it's not entirely clear
 * whether the behavior may or may not be technically valid.
 */
struct NotImplemented : std::runtime_error {
    template<typename... T>
    NotImplemented(const char* message, T&&... ts) : std::runtime_error(fmt::format(fmt::runtime(message), std::forward<T>(ts)...) + "\n" + generate_backtrace()) {

    }
};

/**
 * Exception to throw when the given situation is invalid, such as by violating
 * preconditions of an emulated API.
 *
 * Examples of this are out-of-range parameters and invalid enum values.
 *
 * When it's not entirely clear whether a situation should be considered
 * valid or not, prefer NotImplemented.
 */
struct Invalid : std::runtime_error {
    template<typename... T>
    Invalid(const char* message, T&&... ts) : std::runtime_error(fmt::format(fmt::runtime(message), std::forward<T>(ts)...) + "\n" + generate_backtrace()) {

    }
};

/**
 * Exception to throw when the user provides invalid input, for example from
 * a command-line argument or a configuration file.
 *
 * Emulation logic should never use this.
 */
struct InvalidUserInput : std::runtime_error {
    template<typename... T>
    InvalidUserInput(const char* message, T&&... ts) : std::runtime_error(fmt::format(fmt::runtime(message), std::forward<T>(ts)...) + "\n" + generate_backtrace()) {

    }
};

} // namespace Exceptions

} // namespace Mikage

// ValidateContract is implemented as a macro so that we can stringify the failed condition
#ifdef NDEBUG
#define ValidateContract(cond) do { if (!(cond)) { \
        throw Mikage::Exceptions::ContractViolated(#cond, __PRETTY_FUNCTION__, __FILE__, __LINE__); \
    } } while (false)
#else
// Use assert() directly in debug mode
#define ValidateContract(cond) do { if (!(cond)) { \
        fputs(Mikage::Exceptions::generate_backtrace().c_str(), stderr); \
        assert((cond)); \
    } } while (false)
#endif
