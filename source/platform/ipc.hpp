#pragma once

#include "../bit_field.h"

#include <boost/mp11.hpp>

#include <cstdint>

namespace Platform {

namespace IPC {

using BitField::v1::BitField;

union CommandHeader {
    uint32_t raw;

    BitField< 0,  6, uint32_t> size_translate_params; // in words
    BitField< 6,  6, uint32_t> num_normal_params;
    BitField<16, 16, uint32_t> command_id;

    static CommandHeader Make(uint32_t command_id, uint32_t num_normal_params, uint32_t size_translate_params) {
        CommandHeader command = { 0 };
        command.command_id = command_id;
        command.num_normal_params = num_normal_params;
        command.size_translate_params = size_translate_params;
        return command;
    }
};

union TranslationDescriptor {
    uint32_t raw;

    enum Type : uint32_t {
        Handles = 0,
        StaticBuffer = 1,
        PXIBuffer = 2,
        PXIConstBuffer = 3,

        // Technically this is a mapping request, but it will be rejected with a kernel panic
        MapInvalid = 4,
        MapReadOnly = 5,
        MapWriteOnly = 6,
        MapReadWrite = 7
    };

    enum MapAccessLevel : uint32_t {
        Invalid = 0,
        Read = 1,
        Write = 2,
        ReadWrite = 3
    };

    BitField<1, 3, Type> type;

    union {
        // Close client handle
        BitField< 4, 1, uint32_t> close_handle;

        // Fill the following words with the process id
        BitField< 5, 1, uint32_t> fill_process_id;

        BitField<26, 6, uint32_t> num_handles_minus_one;

        uint32_t NumHandles() const {
            return num_handles_minus_one + 1;
        }
    } handles;

    union {
        // buffer id for the destination process to store data in
        BitField< 10,  4, uint32_t> id;

        // number of bytes to store in the destination process buffer
        BitField< 14, 18, uint32_t> size; // TODO: Maximal size unconfirmed
    } static_buffer;

    union {
        BitField<1, 1, uint32_t> read_only;

        // NOTE: The actual size of this field is unknown
        BitField<4, 4, uint32_t> id;

        // NOTE: The actual size of this field is unknown
        BitField<8, 24, uint32_t> size;
    } pxi_buffer;

    union {
        // Permissions for the mapped buffer in the target process
        BitField< 1,  2, MapAccessLevel> permissions;

        // number of bytes to store in the destination process buffer
        BitField< 4, 27, uint32_t> size; // TODO: Maximal size unknown

        // If this field is set, the buffer will be unmapped from the sending process instead of mapped in the target process
        BitField<31,  1, uint32_t> unmap;
    } map_buffer;

    static TranslationDescriptor MakeHandles(uint32_t num_handles, bool close = false) {
        TranslationDescriptor desc = { 0 };
        desc.type = Type::Handles;
        desc.handles.num_handles_minus_one = num_handles - 1;
        desc.handles.close_handle = close;
        return desc;
    }

    static TranslationDescriptor MakeProcessHandle() {
        TranslationDescriptor desc = { 0 };
        desc.type = Type::Handles;
        desc.handles.fill_process_id = true;
        return desc;
    }

    static TranslationDescriptor MakeStaticBuffer(uint32_t id, uint32_t num_bytes) {
        TranslationDescriptor desc = { 0 };
        desc.type = Type::StaticBuffer;
        desc.static_buffer.id = id;
        desc.static_buffer.size = num_bytes;
        return desc;
    }

    static TranslationDescriptor MakePXIBuffer(uint32_t id, uint32_t num_bytes) {
        TranslationDescriptor desc = { 0 };
        desc.type = Type::PXIBuffer;
        desc.pxi_buffer.id = id;
        desc.pxi_buffer.size = num_bytes;
        return desc;
    }

    static TranslationDescriptor MakePXIConstBuffer(uint32_t id, uint32_t num_bytes) {
        TranslationDescriptor desc = { 0 };
        desc.type = Type::PXIConstBuffer;
        desc.pxi_buffer.id = id;
        desc.pxi_buffer.size = num_bytes;
        return desc;
    }

    static TranslationDescriptor UnmapBuffer(uint32_t num_bytes) {
        TranslationDescriptor desc = { 0 };
        desc.type = Type::MapReadOnly; // Not sure whether the permissions matter at all for unmapping
        desc.map_buffer.size = num_bytes;
        return desc;
    }

    static TranslationDescriptor MapBuffer(uint32_t num_bytes, MapAccessLevel access) {
        TranslationDescriptor desc = { 0 };
        desc.type = Type::MapInvalid; // Proper type is set through "access".
        desc.map_buffer.permissions = access;
        desc.map_buffer.size = num_bytes;
        return desc;
    }
};

enum class HandleType {
    Object,
    Semaphore,
    ServerPort,
    ClientSession,
    SharedMemoryBlock,
    Event,
    Process,
    Mutex,
};

namespace detail {

template<typename T, typename = std::void_t<>>
struct has_ipc_serialize : std::false_type {};
template<typename T>
struct has_ipc_serialize<T, std::void_t<decltype(T::IPCSerialize(std::declval<T>()))>> : std::true_type {};

template<typename T, typename = std::void_t<>>
struct has_ipc_deserialize : std::false_type {};
template<typename T>
struct has_ipc_deserialize<T, std::void_t<decltype(std::apply(T::IPCDeserialize, T::IPCSerialize(std::declval<T>())))>> : std::true_type {};

template<typename T>
auto Serialize(const T& data) {
    if constexpr (std::is_enum_v<T>) {
        using underlying = std::underlying_type_t<T>;
        static_assert(  std::is_same_v<underlying, uint32_t>,
                        "Only 32-bit enums are supported currently");
        return std::make_tuple(static_cast<underlying>(data));
    } else {
        static_assert(  has_ipc_serialize<T>::value,
                        "T cannot be used in IPC commands because it does not define IPCSerialize");
        return T::IPCSerialize(data);
    }
}

template<typename T>
T EnumDeserialize(uint32_t data) {
    return T { data };
}

template<typename T>
auto GetDeserializer() {
    if constexpr (std::is_enum_v<T>) {
        static_assert(  std::is_same_v<std::underlying_type_t<T>, uint32_t>,
                        "Only 32-bit enums are supported currently");
        return EnumDeserialize<T>;
    } else {
        static_assert(  has_ipc_deserialize<T>::value,
                        "T cannot be used in IPC commands because it does not define IPCDeserialize");
        return T::IPCDeserialize;
    }
}

} // namespace detail

/// Collection of tags used to describe each parameter in an IPC request
namespace CommandTags {
    template<size_t num_words>
    struct tag { static constexpr size_t size = num_words; };

    struct uint32_tag : tag<1> {};
    struct uint64_tag : tag<2> {};

    /**
     * Specifically describes the command result as a tag; technically, this
     * could be described using uint32_t, but some implementations may want to
     * treat the result separately. In particular, ctrulib 3DS homebrew treats
     * the result code as a signed int32_t value.
     */
    struct result_tag : tag<1> {};

    /// Copy one or more kernel object handles between processes
    template<HandleType Type, size_t num = 1>
    struct handle_tag : tag<1 + num> {};

    /// Move one or more kernel object handles between processes
    template<HandleType Type, size_t num = 1>
    struct handle_close_tag : tag<1 + num> {};

    struct process_id_tag : tag<2> {};

    // TODO: Not sure whether to encode the static buffer id in this tag or not
    //template<unsigned id>
    struct static_buffer_tag : tag<2> { /*static_assert(id < 16, "Static buffer id must be smaller than 16"); */ };

    template<bool read_only>
    struct pxi_buffer_tag : tag<2> { /*static_assert(id < 16, "Target static buffer id must be smaller than 16"); */ };

    // Separates "request data" from "response data"
    struct response_tag : tag<2> {};

    struct map_buffer_r_tag : tag<2> {};
    struct map_buffer_w_tag : tag<2> {};
    struct map_buffer_rw_tag : tag<2> {};


    namespace detail {

        template<typename T>
        constexpr std::size_t IPCSizeForType();

        template<typename T>
        struct size_of_tag_for_type : std::integral_constant<size_t, IPCSizeForType<T>()> {};

        template<typename T>
        constexpr std::size_t IPCSizeForType() {
            if constexpr (std::is_same_v<T, uint32_t>) {
                return 1;
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                return 2;
            } else if constexpr (std::is_enum_v<T>) {
                using underlying = std::underlying_type_t<T>;
                static_assert((sizeof(underlying) % 4) == 0, "Enum size must be a multiple of 4");
                return sizeof(underlying) / 4;
            } else {
                using constitutent_types = std::invoke_result_t<decltype(Platform::IPC::detail::Serialize<T>), T>;
                return boost::mp11::mp_fold<
                                            boost::mp11::mp_transform<size_of_tag_for_type, constitutent_types>,
                                            boost::mp11::mp_size_t<0>,
                                            boost::mp11::mp_plus>::value;
            }
        }

    }  // namespace detail

    /// Serialized C++ structure into a list of consecutive normal parameters
    template<typename T>
    struct serialized_tag : tag<detail::IPCSizeForType<T>()> {};

    // Size of the corresponding data in TLS memory
    template<typename Tag>
    struct data_size : boost::mp11::mp_size_t<Tag::size> {
    };
}

namespace detail {

/// Convenience typedef for the empty type list (TODO: This is redundant now!)
using empty_tuple = boost::mp11::mp_list<>;

/// Convenience template for constructing a single-element type list (TODO: This is redundant now!)
template<typename T>
using singleton_tuple = boost::mp11::mp_list<T>;

/// Convenience template for appending a type to a type list (TODO: This is redundant now!)
template<typename List, typename Type>
using append = boost::mp11::mp_push_back<List, Type>;

} // namespace detail

template<template<unsigned, typename, typename, typename, typename> class Base,
         unsigned id,
         typename RequestNormalList = detail::empty_tuple,
         typename RequestTranslateList = detail::empty_tuple,
         typename ResponseNormalList = detail::empty_tuple,
         typename ResponseTranslateList = detail::empty_tuple>
struct IPCCommandBuilder;

/**
 * Specialization of IPCCommandBuilder used when only request normal parameters
 * have been given thus far: Adding request translate parameters or starting
 * the response list will move to the next specialization and thus prevent
 * further request normal parameters from being added.
 *
 * This enforces IPC parameters to be specified in a well-defined order.
 */
template<template<unsigned, typename A, typename B = detail::empty_tuple, typename C = detail::empty_tuple, typename D = detail::empty_tuple> class Base, unsigned id, typename RequestNormalList>
struct IPCCommandBuilder<Base, id, RequestNormalList> {
    using add_uint32 = Base<id, detail::append<RequestNormalList, CommandTags::uint32_tag>>;
    using add_uint64 = Base<id, detail::append<RequestNormalList, CommandTags::uint64_tag>>;

    // TODO: Rename to simply "add"
    template<typename T>
    using add_serialized = Base<id, detail::append<RequestNormalList, CommandTags::serialized_tag<T>>>;

    template<typename NewTag>
    using AddTranslateParam = Base<id, RequestNormalList, detail::singleton_tuple<NewTag>>;

    template<HandleType Type>
    using add_handle = AddTranslateParam<CommandTags::handle_tag<Type>>;
    template<HandleType Type, size_t num>
    using add_handles = AddTranslateParam<CommandTags::handle_tag<Type, num>>;
    template<HandleType Type>
    using add_and_close_handle = AddTranslateParam<CommandTags::handle_close_tag<Type>>;
    template<HandleType Type, size_t num>
    using add_and_close_handles = AddTranslateParam<CommandTags::handle_close_tag<Type, num>>;
    using add_process_id = AddTranslateParam<CommandTags::process_id_tag>;
    using add_static_buffer = AddTranslateParam<CommandTags::static_buffer_tag>;
    using add_pxi_buffer = AddTranslateParam<CommandTags::pxi_buffer_tag<false>>;
    using add_pxi_buffer_r = AddTranslateParam<CommandTags::pxi_buffer_tag<true>>;
    using add_buffer_mapping_read = AddTranslateParam<CommandTags::map_buffer_r_tag>;
    using add_buffer_mapping_write = AddTranslateParam<CommandTags::map_buffer_w_tag>;
    using add_buffer_mapping_read_write = AddTranslateParam<CommandTags::map_buffer_rw_tag>;

    using response = Base<id, RequestNormalList, detail::empty_tuple, detail::singleton_tuple<CommandTags::result_tag>>;
};

/**
 * Specialization of IPCCommandBuilder used when request translate parameters
 * are being given: It's not possible to add further request normal parameters
 * in this specialization. Furthermore, starting the response list will move
 * to the next specialization and thus prevent any further request parameters
 * from being added.
 *
 * This enforces IPC parameters to be specified in a well-defined order.
 */
template<template<unsigned, typename A, typename B, typename C = detail::empty_tuple, typename D = detail::empty_tuple> class Base, unsigned id, typename RequestNormalList, typename RequestTranslateList>
struct IPCCommandBuilder<Base, id, RequestNormalList, RequestTranslateList> {
    template<typename NewTag>
    using AddTranslateParam = Base<id, RequestNormalList, detail::append<RequestTranslateList, NewTag>>;

    template<HandleType Type>
    using add_handle = AddTranslateParam<CommandTags::handle_tag<Type>>;
    template<HandleType Type, size_t num>
    using add_handles = AddTranslateParam<CommandTags::handle_tag<Type, num>>;
    template<HandleType Type>
    using add_and_close_handle = AddTranslateParam<CommandTags::handle_close_tag<Type>>;
    template<HandleType Type, size_t num>
    using add_and_close_handles = AddTranslateParam<CommandTags::handle_close_tag<Type, num>>;
    using add_process_id = AddTranslateParam<CommandTags::process_id_tag>;
    using add_static_buffer = AddTranslateParam<CommandTags::static_buffer_tag>;
    using add_pxi_buffer = AddTranslateParam<CommandTags::pxi_buffer_tag<false>>;
    using add_pxi_buffer_r = AddTranslateParam<CommandTags::pxi_buffer_tag<true>>;
    using add_buffer_mapping_read = AddTranslateParam<CommandTags::map_buffer_r_tag>;
    using add_buffer_mapping_write = AddTranslateParam<CommandTags::map_buffer_w_tag>;
    using add_buffer_mapping_read_write = AddTranslateParam<CommandTags::map_buffer_rw_tag>;

    using response = Base<id, RequestNormalList, RequestTranslateList, detail::singleton_tuple<CommandTags::result_tag>>;
};

/**
 * Specialization of IPCCommandBuilder used when response normal parameters
 * are being given: It's not possible to add further request parameters in this
 * specialization. Furthermore, adding any response translate parameters
 * will move to the next specialization and thus prevent any further request
 * normal parameters from being added.
 *
 * This enforces IPC parameters to be specified in a well-defined order.
 */
template<template<unsigned, typename A, typename B, typename C, typename D = detail::empty_tuple> class Base, unsigned id, typename RequestNormalList, typename RequestTranslateList, typename ResponseNormalList>
struct IPCCommandBuilder<Base, id, RequestNormalList, RequestTranslateList, ResponseNormalList> {
    using add_uint32 = Base<id, RequestNormalList, RequestTranslateList, detail::append<ResponseNormalList, CommandTags::uint32_tag>>;
    using add_uint64 = Base<id, RequestNormalList, RequestTranslateList, detail::append<ResponseNormalList, CommandTags::uint64_tag>>;
    template<typename T>
    using add_serialized = Base<id, RequestNormalList, RequestTranslateList, detail::append<ResponseNormalList, CommandTags::serialized_tag<T>>>;

    template<typename NewTag>
    using AddTranslateParam = Base<id, RequestNormalList, RequestTranslateList, ResponseNormalList, detail::singleton_tuple<NewTag>>;

    template<HandleType Type>
    using add_handle = AddTranslateParam<CommandTags::handle_tag<Type>>;
    template<HandleType Type, size_t num>
    using add_handles = AddTranslateParam<CommandTags::handle_tag<Type, num>>;
    template<HandleType Type>
    using add_and_close_handle = AddTranslateParam<CommandTags::handle_close_tag<Type>>;
    template<HandleType Type, size_t num>
    using add_and_close_handles = AddTranslateParam<CommandTags::handle_close_tag<Type, num>>;
    using add_process_id = AddTranslateParam<CommandTags::process_id_tag>;
    using add_static_buffer = AddTranslateParam<CommandTags::static_buffer_tag>;
    using add_pxi_buffer = AddTranslateParam<CommandTags::pxi_buffer_tag<false>>;
    using add_pxi_buffer_r = AddTranslateParam<CommandTags::pxi_buffer_tag<true>>;
    // TODO: Rename these to "unmap" buffer because that's what they are used for in responses!
    using add_buffer_mapping_read = AddTranslateParam<CommandTags::map_buffer_r_tag>;
    using add_buffer_mapping_write = AddTranslateParam<CommandTags::map_buffer_w_tag>;
    using add_buffer_mapping_read_write = AddTranslateParam<CommandTags::map_buffer_rw_tag>;
};

/**
 * Base class for constructing IPCCommand types: IPCCommandBuilder takes care
 * of ensuring that no invalid commands are constructed; for a command to be
 * valid, all IPC command parameters must be given in order, starting with
 * request normal parameters, followed by request translate parameters,
 * response normal parameters, and finally response translate parameters.
 *
 * In the the default implementation of IPCCommandBuilder, used when response translate
 * parameters are being given, it's not possible to add any request parameters
 * or any further response normal parameters in this specialization.
 *
 * This enforces IPC parameters to be specified in a well-defined order.
 */
template<template<unsigned, typename, typename, typename, typename> class Base, unsigned id, typename RequestNormalList, typename RequestTranslateList, typename ResponseNormalList, typename ResponseTranslateList>
struct IPCCommandBuilder {
    template<typename NewTag>
    using AddTranslateParam = Base<id, RequestNormalList, RequestTranslateList,
                                       ResponseNormalList, detail::append<ResponseTranslateList, NewTag>>;

    template<HandleType Type>
    using add_handle = AddTranslateParam<CommandTags::handle_tag<Type>>;
    template<HandleType Type, size_t num>
    using add_handles = AddTranslateParam<CommandTags::handle_tag<Type, num>>;
    template<HandleType Type>
    using add_and_close_handle = AddTranslateParam<CommandTags::handle_close_tag<Type>>;
    template<HandleType Type, size_t num>
    using add_and_close_handles = AddTranslateParam<CommandTags::handle_close_tag<Type, num>>;
    using add_process_id = AddTranslateParam<CommandTags::process_id_tag>;
    using add_static_buffer = AddTranslateParam<CommandTags::static_buffer_tag>;
    using add_pxi_buffer = AddTranslateParam<CommandTags::pxi_buffer_tag<false>>;
    using add_pxi_buffer_r = AddTranslateParam<CommandTags::pxi_buffer_tag<true>>;
    // TODO: Rename these to "unmap" buffer because that's what they are used for in responses!
    using add_buffer_mapping_read = AddTranslateParam<CommandTags::map_buffer_r_tag>;
    using add_buffer_mapping_write = AddTranslateParam<CommandTags::map_buffer_w_tag>;
    using add_buffer_mapping_read_write = AddTranslateParam<CommandTags::map_buffer_rw_tag>;
};

/**
 * Utility type used to encode IPC command descriptions in the type system:
 * An IPC command is characterized by its request parameters and its response
 * parameters. Both sets of these are subdivided into normal and translate
 * parameters. The four resulting parameter subsets are specified in the order
 * "request normal", "request translate", "response normal", "response translate".
 *
 * Types describing an IPC command are constructed iteratively using the "add_"
 * type constructors inherited from IPCCommandBuilder, starting from the empty
 * IPCCommand. IPCCommandBuilder ensures that the four parameter subsets are
 * given in the correct order.
 *
 * Encoding IPC commands in the type system like this has various advantages:
 * - IPC commands are defined explicitly in a declarative manner, as opposed to
 *   manually parsing/constructing IPC commands with hardcoded TLS offsets and
 *   parameter translation.
 * - IPC command parameters can automatically be deserialized from thread-local
 *   storage and forwarded to a C++ handler function. This is not only
 *   very convenient, but also reduces potential for human error by implicitly
 *   checking the handler function and the command definition for
 *   compatibility.
 * - IPC command parameters can automatically be serialized to thread-local
 *   storage, such that a generic interface can be created that makes
 *   IPC command submission look like standard C++ function calls.
 */

namespace mp11 = boost::mp11;

namespace detail {

template<typename List, template<typename> class Proj = mp11::mp_identity_t, template<typename...> class Op = mp11::mp_plus>
static constexpr auto projected_fold = mp11::mp_fold<mp11::mp_transform<Proj, List>, std::integral_constant<size_t, 0>, Op>::value;

}  // namespace detail

template<unsigned command_id,
         typename RequestNormalList = detail::empty_tuple,
         typename RequestTranslateList = detail::empty_tuple,
         typename ResponseNormalList = detail::empty_tuple,
         typename ResponseTranslateList = detail::empty_tuple>
struct IPCCommand : IPCCommandBuilder<IPCCommand, command_id, RequestNormalList, RequestTranslateList, ResponseNormalList, ResponseTranslateList> {
    static constexpr auto id = command_id;

    using request_normal_list = RequestNormalList;
    using request_translate_list = RequestTranslateList;
    using request_list = boost::mp11::mp_append<request_normal_list, request_translate_list>;

    using response_normal_list = ResponseNormalList;
    using response_translate_list = ResponseTranslateList;
    using response_list = boost::mp11::mp_append<response_normal_list, response_translate_list>;

    using request_normal_parameter_sizes = mp11::mp_transform<CommandTags::data_size, RequestNormalList>;
    static constexpr auto size_request_normal_parameters = detail::projected_fold<request_normal_parameter_sizes>;
    using response_normal_parameter_sizes = mp11::mp_transform<CommandTags::data_size, ResponseNormalList>;
    static constexpr auto size_response_normal_parameters = detail::projected_fold<response_normal_parameter_sizes>;

    using request_translate_parameter_sizes = mp11::mp_transform<CommandTags::data_size, RequestTranslateList>;
    static constexpr auto size_request_translate_parameters = detail::projected_fold<request_translate_parameter_sizes>;
    using response_translate_parameter_sizes = mp11::mp_transform<CommandTags::data_size, ResponseTranslateList>;
    static constexpr auto size_response_translate_parameters = detail::projected_fold<response_translate_parameter_sizes>;

    static constexpr uint32_t request_header = (static_cast<uint32_t>(id) << 16)
                                                | (static_cast<uint32_t>(size_request_normal_parameters) << 6)
                                                | static_cast<uint32_t>(size_request_translate_parameters);

    // TODO: Which command id does this use? The actual one, or 0?
    static constexpr uint32_t response_header = (static_cast<uint32_t>(id) << 16)
                                                | (static_cast<uint32_t>(size_response_normal_parameters) << 6)
                                                | static_cast<uint32_t>(size_response_translate_parameters);
};

}  // namespace IPC

}  // namespace Platform
