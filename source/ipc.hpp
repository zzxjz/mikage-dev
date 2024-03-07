#pragma once

#include "framework/meta_tools.hpp"
#include "platform/ipc.hpp"
#include "os.hpp"

#include "bit_field.h"

#include <cstdint>

namespace HLE {

namespace OS {
class Thread;
struct Handle;
using Result = uint32_t;
}

namespace IPC {

using namespace Platform::IPC;

/// Dummy structure used for IPC command tags that don't need any actual data
struct EmptyValue {
};

struct MappedBuffer {
    /*VAddr*/uint32_t addr;
    uint32_t size;

    uint32_t EndAddr() const {
        return addr + size;
    }
};

struct StaticBuffer {
    uint32_t addr;
    uint32_t size;
    uint32_t id;
};

template<typename T>
struct NormalParamToCommandTagHelper { using type = CommandTags::serialized_tag<T>; };
template<>
struct NormalParamToCommandTagHelper<uint32_t> { using type = CommandTags::uint32_tag; };
template<>
struct NormalParamToCommandTagHelper<uint64_t> { using type = CommandTags::uint64_tag; };

template<typename T>
using NormalParamToCommandTag = typename NormalParamToCommandTagHelper<T>::type;


template<typename F, typename T, typename ArgsTuple, size_t... Idxs>
auto DoDeserialize(F& f, std::index_sequence<Idxs...>) {
    return Meta::CallWithSequentialEvaluation<decltype(detail::GetDeserializer<T>()(f(NormalParamToCommandTag<std::tuple_element_t<Idxs, ArgsTuple>>{})...))> {
        detail::GetDeserializer<T>(),
        f(NormalParamToCommandTag<std::tuple_element_t<Idxs, ArgsTuple>>{})...
    }.GetResult();
}

/// Utility class to parse IPC messages from TLS
class TLSReader {
    OS::Thread& thread;

    // Skip header
    uint32_t offset = 0x84;

    OS::Handle ParseHandle();

    template<std::size_t Num>
    using HandleReturnType = std::conditional_t<Num == 1, OS::Handle, std::array<OS::Handle, Num>>;

public:
    TLSReader(OS::Thread& thread);
    TLSReader(const TLSReader&) = delete;
    TLSReader(TLSReader&&) = default;

    OS::Result operator()(const IPC::CommandTags::result_tag& /* unused */);
    uint32_t operator()(const IPC::CommandTags::uint32_tag& /* unused */);
    uint64_t operator()(const IPC::CommandTags::uint64_tag& /* unused */);
    StaticBuffer operator()(const IPC::CommandTags::static_buffer_tag& /* unused */);
    StaticBuffer operator()(const IPC::CommandTags::pxi_buffer_tag<false>& /* unused */);
    StaticBuffer operator()(const IPC::CommandTags::pxi_buffer_tag<true>& /* unused */);
    /*ProcessId*/uint32_t operator()(const IPC::CommandTags::process_id_tag& /* unused */);
    MappedBuffer operator()(const IPC::CommandTags::map_buffer_r_tag& /* unused */);
    MappedBuffer operator()(const IPC::CommandTags::map_buffer_w_tag& /* unused */);

    template<typename T>
    T operator()(const IPC::CommandTags::serialized_tag<T>& /* unused */) {
        using arg_list = typename Meta::function_traits<decltype(detail::GetDeserializer<T>())>::args;
        return DoDeserialize<TLSReader, T, arg_list>(*this, std::make_index_sequence<std::tuple_size<arg_list>::value>{});
    }

    template<HandleType Type, std::size_t Num>
    HandleReturnType<Num> operator()(const IPC::CommandTags::handle_close_tag<Type, Num>& /* unused */) {
        return (*this)(IPC::CommandTags::handle_tag<Type, Num> { });
    }
    template<HandleType Type, std::size_t Num>
    HandleReturnType<Num> operator()(const IPC::CommandTags::handle_tag<Type, Num>& /* unused */) {
        offset += 4; // Move past descriptor
        if constexpr (Num == 1) {
            return ParseHandle();
        } else {
            HandleReturnType<Num> ret;
            for (auto& handle : ret) {
                handle = ParseHandle();
            }
            return ret;
        }
    }
};

template<typename F, typename DataTuple, typename Data, size_t... Idxs>
inline void DoWrite(F& f, const Data& data, std::index_sequence<Idxs...>) {
    (f(NormalParamToCommandTag<std::tuple_element_t<Idxs, DataTuple>>{}, std::get<Idxs>(data)), ...);
}

/// Utility class to construct IPC messages
class TLSWriter {
    OS::Thread& thread;

    // Initial offset (response header omitted)
    uint32_t offset = 0x84;

    void WriteHandleDescriptor(const OS::Handle* data, size_t num, bool close);

public:
    TLSWriter(OS::Thread& thread);
    TLSWriter(const TLSWriter& tls) = delete;
    TLSWriter(TLSWriter&& tls) = default;

    void operator()(IPC::CommandTags::result_tag, OS::Result data);
    void operator()(IPC::CommandTags::uint32_tag, uint32_t data);
    void operator()(IPC::CommandTags::uint64_tag, uint64_t data);
    void operator()(IPC::CommandTags::map_buffer_r_tag, const MappedBuffer& buffer);
    void operator()(IPC::CommandTags::map_buffer_w_tag, const MappedBuffer& buffer);
    void operator()(IPC::CommandTags::static_buffer_tag, const StaticBuffer& buffer);
    void operator()(IPC::CommandTags::process_id_tag, const EmptyValue&);

    void operator()(IPC::CommandTags::pxi_buffer_tag<false>, const StaticBuffer& buffer);
    void operator()(IPC::CommandTags::pxi_buffer_tag<true>, const StaticBuffer& buffer);

    template<HandleType Type>
    void operator()(IPC::CommandTags::handle_close_tag<Type>, OS::Handle data) {
        WriteHandleDescriptor(&data, 1, true);
    }

    template<HandleType Type>
    void operator()(IPC::CommandTags::handle_tag<Type>, OS::Handle data) {
        WriteHandleDescriptor(&data, 1, false);
    }

    template<HandleType Type, size_t num>
    void operator()(IPC::CommandTags::handle_close_tag<Type,num>, const std::array<OS::Handle,num>& data) {
        WriteHandleDescriptor(data.data(), num, true);
    }

    template<HandleType Type, size_t num>
    void operator()(IPC::CommandTags::handle_tag<Type,num>, const std::array<OS::Handle,num>& data) {
        WriteHandleDescriptor(data.data(), num, false);
    }

    template<typename T>
    void operator()(IPC::CommandTags::serialized_tag<T>, T& data) {
        auto parameters = detail::Serialize<T>(data);
        constexpr auto tuple_size = std::tuple_size<decltype(parameters)>::value;
        DoWrite<TLSWriter, decltype(parameters)>(*this, parameters, std::make_index_sequence<tuple_size>{});
    }
};

namespace detail {
template<bool NoError>
struct CHECK_EQUALITY_VERBOSELY_DEFAULT_ERROR { static_assert(NoError, "Given integers are not equal!"); };
}

/**
 * Utility structure for asserting that the compile-time constants A and B
 * are equal. If they aren't, the type Error<true> will be instantiated, which
 * is supposed to abort compilation due to a static_assert failure.
 *
 * The error message can be customized by passing an own Error template, which
 * is expected to have a static_assert fail if its instantiated with a "true"
 * argument. This is useful to give helpful error messages instead of cryptic
 * "5 != 3" messages.
 *
 * @todo Move this into a utility header
 */
template<int A, int B,
         template<bool Err> class Error = detail::CHECK_EQUALITY_VERBOSELY_DEFAULT_ERROR>
struct CHECK_EQUALITY_VERBOSELY
{
    // Force Error<A==B> to be instantiated and error out if A!=B
    // If A==B, use sizeof to form the value "true".
    static constexpr bool value = sizeof(Error<A==B>);
};

/// Exception class thrown by SendIPCRequest on error
struct IPCError {
    IPCError(uint32_t response_header, OS::Result result) noexcept : header(response_header), result(result) {
    }
    ~IPCError() noexcept = default;

    uint32_t header;
    OS::Result result;
};

template<typename F, typename Tuple, size_t... Idxs>
auto TransformTupleSequentiallyHelper(F&& f, Tuple&& tuple, std::index_sequence<Idxs...>)
{
    return std::tuple<std::invoke_result_t<F, std::tuple_element_t<Idxs, Tuple>>...> { f(std::get<Idxs>(tuple))... };
}

template<typename F, typename Tuple>
auto TransformTupleSequentially(F&& f, Tuple&& tuple)
{
    return TransformTupleSequentiallyHelper(std::forward<F>(f), std::forward<Tuple>(tuple), std::make_index_sequence<std::tuple_size<Tuple>::value>{});
}

namespace detail {
template<bool NoError>
struct IPCArgumentMatchError { static_assert(NoError, "Given number of function arguments doesn't match the expected number per the IPC message descriptor"); };

template<typename ResponseList>
struct IPCResponseChecker;

template<typename... Parameters>
struct IPCResponseChecker<boost::mp11::mp_list<Parameters...>> {
    void Check(Parameters...) {
        // Do nothing, just syntax-check this is a valid function call
    }
};

template<typename T>
struct TagToTypeHelper;
template<>
struct TagToTypeHelper<CommandTags::uint32_tag> : boost::mp11::mp_identity<uint32_t> {};
template<>
struct TagToTypeHelper<CommandTags::uint64_tag> : boost::mp11::mp_identity<uint64_t> {};
template<>
struct TagToTypeHelper<CommandTags::result_tag> : boost::mp11::mp_identity<OS::Result> {};
template<HandleType T>
struct TagToTypeHelper<CommandTags::handle_tag<T, 1>> : boost::mp11::mp_identity<OS::Handle> {};
template<HandleType T>
struct TagToTypeHelper<CommandTags::handle_close_tag<T, 1>> : boost::mp11::mp_identity<OS::Handle> {};
template<HandleType T, std::size_t num>
struct TagToTypeHelper<CommandTags::handle_tag<T, num>> : boost::mp11::mp_identity<std::array<OS::Handle, num>> {};
template<HandleType T, std::size_t num>
struct TagToTypeHelper<CommandTags::handle_close_tag<T, num>> : boost::mp11::mp_identity<std::array<OS::Handle, num>> {};
template<>
struct TagToTypeHelper<CommandTags::map_buffer_r_tag> : boost::mp11::mp_identity<MappedBuffer> {};
template<>
struct TagToTypeHelper<CommandTags::map_buffer_w_tag> : boost::mp11::mp_identity<MappedBuffer> {};
template<>
struct TagToTypeHelper<CommandTags::map_buffer_rw_tag> : boost::mp11::mp_identity<MappedBuffer> {};
template<>
struct TagToTypeHelper<CommandTags::static_buffer_tag> : boost::mp11::mp_identity<StaticBuffer> {};
template<typename T>
struct TagToTypeHelper<CommandTags::serialized_tag<T>> : boost::mp11::mp_identity<T> {};
template<bool read_only>
struct TagToTypeHelper<CommandTags::pxi_buffer_tag<read_only>> : boost::mp11::mp_identity<StaticBuffer> {};
template<>
struct TagToTypeHelper<CommandTags::process_id_tag> : boost::mp11::mp_identity<uint32_t> {};
template<typename T>
using TagToType = typename TagToTypeHelper<T>::type;

template<typename Command, bool IsResponse, typename Thread, typename ArgTagsTuple>
struct IPCMessageWriter;

template<typename Command, bool IsResponse, typename Thread, typename... ArgTags>
struct IPCMessageWriter< Command, IsResponse, Thread, boost::mp11::mp_list<ArgTags...>> {
    Thread& thread;

    using TypeList = std::conditional_t<IsResponse, typename Command::response_list, typename Command::request_list>;

    template<typename... Data>
    auto operator()(Data... data) {
        if constexpr (IsResponse) {
            // Check if the returned value matches the command definition
            // TODO: Enable this check for requests, too
            using paramlist = boost::mp11::mp_transform<TagToType, TypeList>;
            IPCResponseChecker<paramlist>{}.Check(data...);
        }

        thread.WriteTLS(0x80, (IsResponse ? Command::response_header : Command::request_header));

        IPC::TLSWriter writer(thread);
        (writer(ArgTags { }, data), ...);
    }
};

} // namespace detail

template<typename Command, typename Thread, typename... Data>
void WriteIPCReplyFromTuple(Thread& thread, std::tuple<Data...> data) {
    std::apply(detail::IPCMessageWriter<Command, true, Thread, typename Command::response_list> { thread }, data);
}

template<typename Func, typename MainArgs, typename... ExtraArgs>
struct IPCMessageResultsHelper;

template<typename Func, typename... MainArgs, typename... ExtraArgs>
struct IPCMessageResultsHelper<Func, boost::mp11::mp_list<MainArgs...>, ExtraArgs...>
    : std::invoke_result<   Func,
                            std::invoke_result_t<IPC::TLSReader, MainArgs>...,
                            ExtraArgs...> {
};

template<typename Func, typename Command, typename... ExtraArgs>
using IPCMessageResults = IPCMessageResultsHelper<Func, typename Command::request_list, ExtraArgs...>;

template<   typename Func, typename... ExtraArgs, typename... ArgTags>
auto DispatchIPCMessageHelper(IPC::TLSReader reader, Func&& handler, ExtraArgs&&... args, boost::mp11::mp_list<ArgTags...>) {
    // Read request data from TLS (via ArgTags) and invoke request handler
    // TODO: The following may perform implicit conversion of uint64_t
    //       parameters to uint32_t (or vice versa), hence it actually provides
    //       less compile-time safety that we would like it to, currently.
    if constexpr (std::is_invocable_v<  Func, ExtraArgs...,
                                        std::invoke_result_t<IPC::TLSReader, ArgTags>...>) {
        // Use CallWithSequentialEvaluation to get well-defined execution order
        // TODO: We should static assert for handlers returning void, but this somehow doesn't always compile currently
//        static_assert(  !std::is_invocable_r_v<void, Func, ExtraArgs..., std::invoke_result_t<IPC::TLSReader, ArgTags>...>,
//                        "Handler must have non-void return type");
        // std::move is needed to work around a clang bug with class template argument deduction
        return std::move(Meta::CallWithSequentialEvaluation {
                    std::forward<Func>(handler),
                    std::forward<ExtraArgs>(args)...,
                    reader(ArgTags{})... }).GetResult();
    } else {
        // handler is not invocable with the given arguments:
        // Use a regular function call to get a good compiler diagnostic
        return std::forward<Func>(handler)(
                    std::forward<ExtraArgs>(args)...,
                    reader(ArgTags{})...);
    }
}

/**
 * Read IPC request data from the thread's TLS and forward the decoded
 * arguments to the given handler.
 */
template<typename Command, typename Func, typename Thread, typename... ExtraArgs>
auto DispatchIPCMessage(Func&& handler, Thread& thread, ExtraArgs&&... args) {
    if (thread.ReadTLS(0x80) != Command::request_header)
        throw std::runtime_error(fmt::format("Expected command header {:#x}, but got {:#x}", Command::request_header, thread.ReadTLS(0x80)));

    return DispatchIPCMessageHelper<Func, ExtraArgs...>(IPC::TLSReader(thread), std::forward<Func>(handler), std::forward<ExtraArgs>(args)..., typename Command::request_list { });
}

/**
 * Read IPC request data from the thread's TLS, forward the decoded
 * arguments to the given handler, and write results back to TLS
 */
template<typename Command, typename Func, typename Thread, typename... ExtraArgs>
void HandleIPCCommand(Func&& handler, Thread& thread, ExtraArgs&&... args) {
    // Decode request data from TLS and invoke handler
    auto output_data = DispatchIPCMessage<Command>(std::forward<Func>(handler), thread, std::forward<ExtraArgs>(args)...);

    // Write response to TLS
    // TODO: Do not pass output_data per copy!
    WriteIPCReplyFromTuple<Command>(thread, output_data);
}

namespace detail {
/**
 * Unwrap small tuples:
 * For single-element tuples, this returns the contained element.
 * For zero-element tuples, this returns void.
 * Otherwise, this just returns the unmodified tuple.
 */
template<typename... T>
inline auto UnwrapTuple(std::tuple<T...>&& tuple) {
    return tuple;
}

template<typename... T>
inline decltype(auto) UnwrapTuple(std::tuple<T...>& tuple) {
    return tuple;
}

template<typename T>
inline decltype(auto) UnwrapTuple(std::tuple<T>&& tuple) {
    return std::get<0>(tuple);
}

template<typename T>
inline decltype(auto) UnwrapTuple(std::tuple<T>& tuple) {
    return std::get<0>(tuple);
}

inline void UnwrapTuple(std::tuple<>&&) {
}

inline void UnwrapTuple(std::tuple<>&) {
}

}

/**
 * Submit an IPC request from the given thread through the given session
 * handle. The request parameters are taken from the parameter pack, which is
 * expected to be compatible with the parameter list inferred from the Command
 * type.
 * @return A tuple of return values inferred from the given Command. This does
 *         not include the return code. If only a single value is returned, the
 *         tuple is unwrapped to the value itself. If no value is returned,
 *         this function returns void.
 * @throws IPCError if sending the IPC request was unsuccessful, if the
 *                  response indicates a failure, and if the response header
 *                  is different from the one indicated by Command.
 * @todo Consider changing "t..." to use (universal) reference semantics
 * @todo Currently, things like the process_id tag require an EmptyValue
 *       dummy argument, despite not actually needing any input data
 */
template<typename Command, typename Thread, typename... T>
auto SendIPCRequest(Thread& thread, OS::Handle session_handle, T... t) {
    // Write request data to TLS
    detail::IPCMessageWriter<Command, false, Thread, typename Command::request_list> { thread } (t...);

    // Send IPC request and wait for response
    // TODO: The command header returned here shouldn't be read from TLS but
    //       rather constructed on-the-fly!
    OS::Result result = std::get<0>(thread.CallSVC(&OS::OS::SVCSendSyncRequest, session_handle));
    if (result != 0 /* RESULT_OK */)
        throw IPCError(thread.ReadTLS(0x080), result);

    // Read result code of the response
    auto reader = TLSReader(thread);
    result = reader(CommandTags::result_tag{});
    if (result != 0)
        throw IPCError{thread.ReadTLS(0x80), result};

    // TODO: Currently, we build the response header incorrectly and hence
    //       cannot actually perform this important check
    //assert(thread.ReadTLS(0x80) == Command::response_header);

    // Read results, but before that drop the result code (if it doesn't
    // indicate success, we aborted before anyway)
    using ResponseTypeList = boost::mp11::mp_rename<typename Command::response_list, std::tuple>;
    // TODO: Filter out EmptyValue values!
    using FixedResponseTypeList = boost::mp11::mp_pop_front<ResponseTypeList>;
    auto result_data = TransformTupleSequentially(reader, FixedResponseTypeList{});

    // Return data: Decay to "void" or an unwrapped type if the data tuple is
    // empty or a singleton, respectively
    return detail::UnwrapTuple(result_data);
}

}  // namespace IPC

}  // namespace HLE
