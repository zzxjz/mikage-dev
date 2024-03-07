#pragma once

#include <array>
#include <cstddef>
#include <type_traits>

namespace Meta {

template<typename... T>
using void_t = void;

// Evaluate to false unless this template is specialized for T
// TODO: Once we move to C++17, make the template take a generic "auto..." parameter pack
template<typename T, template<typename...> class Template>
struct is_instantiation_of : std::false_type {};

// If a set of template arguments that produce T exists, evaluate to true
template<typename... Args, template<typename...> class Template>
struct is_instantiation_of<Template<Args...>, Template> : std::true_type {};

// Specialized variant of the above to deal with the std::array template argument list
template<typename T, template<typename, std::size_t> class Template>
struct is_instantiation_of2 : std::false_type {};
template<typename Arg1, std::size_t Arg2, template<typename, std::size_t> class Template>
struct is_instantiation_of2<Template<Arg1, Arg2>, Template> : std::true_type {};

template<typename T>
constexpr auto is_std_tuple_v = is_instantiation_of<T, std::tuple>::value;

template<typename T>
constexpr auto is_std_array_v = is_instantiation_of2<T, std::array>::value;

/// Produce a structure that inherits all given Base classes
template<typename... Bases>
struct inherited : Bases... {
};

/**
 * Turn the given template template argument in a type, such that it can be
 * passed to standard type functors like std::enable_if without having to
 * evaluate the template with any particular arguments
 */
template<template<typename...> class Temp>
struct delay {
	template<typename... Ts>
	using eval = Temp<Ts...>;
};

/// Conditionally selects one of the two given templates depending on the given boolean
template<bool Cond, template<typename T> class OnTrue, template<typename T> class OnFalse>
struct MetaConditional {
    template<typename T>
    using eval = typename std::conditional_t<Cond, delay<OnTrue>, delay<OnFalse>>::type::template eval<T>;
};

template<typename T>
constexpr auto is_class_v = std::is_class<T>::value;
template<typename B, typename C>
constexpr auto is_base_of_v = std::is_base_of<B, C>::value;
template<typename T>
constexpr auto tuple_size_v = std::tuple_size<T>::value;
template<typename T, typename U>
constexpr auto is_same_v = std::is_same<T, U>::value;

namespace detail {
template<bool NoError>
struct CHECK_EQUALITY_VERBOSELY_DEFAULT_ERROR { static_assert(NoError, "Given integers are not equal!"); };
}

/**
 * Helper structure for asserting that the compile-time constants A and B
 * are equal. If they aren't, the type Error<true> will be instantiated, which
 * is supposed to abort compilation due to a static_assert failure.
 *
 * The error message can be customized by passing an own Error template, which
 * is expected to have a static_assert fail if its instantiated with a "true"
 * argument. This is useful to give helpful error messages instead of cryptic
 * "5 != 3" messages.
 */
template<int A, int B,
         template<bool Err> class Error = detail::CHECK_EQUALITY_VERBOSELY_DEFAULT_ERROR>
struct CHECK_EQUALITY_VERBOSELY
{
    // Force Error<A==B> to be instantiated and error out if A!=B
    // If A==B, use sizeof to form the value "true".
    static constexpr bool value = sizeof(Error<A==B>);
};


template<typename F, typename... Args>
decltype(auto) invoke(F&& f, Args&&... args) noexcept(noexcept(std::forward<F>(f)(std::forward<Args>(args)...))) {
    return std::forward<F>(f)(std::forward<Args>(args)...);
}

//template<typename T>
//using typeof_foo = decltype(std::declval<T>().foo);

/*template<template<typename...> class Templ, typename ArgsTuple, typename = void_t<ArgsTuple>>
struct is_valid_impl : std::false_type {};

template<template<typename...> class Templ, typename... Args>
struct is_valid_impl<Templ, std::tuple<Args...>, Templ<Args...>> : std::true_type {};

template<template<typename...> class Templ, typename... Args>
using is_valid = is_valid_impl<Templ, std::tuple<Args...>>;
*/

template<typename AlwaysVoid, template<typename...> class Templ, typename... Args>
struct is_valid_impl : std::false_type {};

template<template<typename...> class Templ, typename... Args>
struct is_valid_impl<void_t<Templ<Args...>>, Templ, Args...> : std::true_type {};

template<template<typename...> class Templ, typename... Args>
using is_valid = is_valid_impl<void, Templ, Args...>;


/*template<typename F, typename = void_t<F>>
struct is_valid_call_impl : std::false_type {};

template<typename F, typename... Args>
struct is_valid_call_impl<F, void_t<decltype(std::declval<F&&>()(std::declval<Args&&>()...))>> : std::true_type {};

template<typename F, typename... Args>
static constexpr auto is_valid_call(F&& f, Args&&... args) {
    return is_valid_call_impl<F, Args...>::value;
}*/

/*template<typename, typename F, typename... Args>
struct invoke_result;

template<typename F, typename... Args>
struct invoke_result<decltype(std::declval<F>()(std::declval<Args>()...)), F, Args...> {
    using type = decltype(std::declval<F>()(std::declval<Args>()...));
}*/

template<typename F, typename... Args>
using invoke_result = std::invoke_result<F(Args...)>;

//template<typename F, typename... Args>
//using invoke_result_t = invoke_result<F, Args...>::type;

template<typename F, typename... Args>
using invoke_result_t = typename invoke_result<F, Args...>::type;


/**
 * Helper class for calling a function such that its arguments are evaluated in
 * sequential order. This couldn't easily be achieved otherwise, since the
 * C++ standard doesn't guarantee evaluation order for function arguments.
 * It does guarantee left-to-right evaluation in brace-initialization (cf.
 * C++ standard draft n4296 ([dcl.init.list] (8.5.4.4))), though, which is why
 * this class exists.
 *
 * Usage example:
 *   char f(int a, int b);
 *   int num = 0;
 *   CallWithSequentialEvaluation { f, ++num, ++num };
 *
 * Note that the return type cannot easily be deduced and hence must be
 * specified manually.
 *
 * TODO: Chances are this behavior isn't implemented in all major
 *       compilers. The following may be affected:
 *       * MSVC 2015 Update 3 (untested)
 *       * GCC 7.1.1 (seems to be working for nontrivial cases)
 *       Symptoms of this issue are that IPC command arguments may be
 *       loaded in the wrong order.
 */
template<typename Result>
class CallWithSequentialEvaluation {
    Result result;

public:
    template<typename F, typename... Args>
    CallWithSequentialEvaluation(F&& f, Args&&... args) : result(std::forward<F>(f)(std::forward<Args>(args)...)) {}

    Result GetResult() && {
        return std::move(result);
    }
};

template<typename F, typename... Args>
CallWithSequentialEvaluation(F&&, Args&&...) -> CallWithSequentialEvaluation<std::invoke_result_t<F, Args...>>;

// Specialization for void
template<>
class CallWithSequentialEvaluation<void> {
public:
    template<typename F, typename... Args>
    CallWithSequentialEvaluation(F&& f, Args&&... args) {
        std::forward<F>(f)(std::forward<Args>(args)...);
    }
};

template<typename F>
struct function_traits;

template<typename Ret, typename... Args>
struct function_traits<Ret(Args...)> { using args = std::tuple<Args...>; };

template<typename Ret, typename... Args>
struct function_traits<Ret(*)(Args...)> { using args = std::tuple<Args...>; };

template<typename Enum>
constexpr std::underlying_type_t<Enum> to_underlying(Enum value) noexcept {
    return static_cast<std::underlying_type_t<Enum>>(value);
}

template<typename Enum>
constexpr Enum next_enum(Enum value, std::underlying_type_t<Enum> n = 1) noexcept {
    return Enum { Meta::to_underlying(value) + n };
}

} // namespace Meta
