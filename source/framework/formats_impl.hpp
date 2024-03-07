/**
 * The purpose of this file is to provide the definition of LoadWithReadCallback.
 * This function is heavy on template metaprogramming, so we don't want to
 * recompile it in every translation unit. Instead, each serialized structure
 * should have exactly one file associated to it that includes this header and
 * then explicitly instantiates LoadWithReadCallback.
 */

#pragma once

#ifndef FORMATS_IMPL_EXPLICIT_FORMAT_INSTANTIATIONS_INTENDED
#error This file should only be included if you want to explicitly instantiate LoadWithReadCallback for user-defined structures
#endif

#include <framework/formats.hpp>
#include <framework/meta_tools.hpp>

#include <boost/hana/tuple.hpp>
#include <boost/hana/ext/std/tuple.hpp>
#include <boost/hana/transform.hpp>

#include <functional>

namespace FileFormat {

/// Type-erasing helper class to allow compiling the deserialization code only once
template<typename T, template<typename> class TypeMapper>
struct StreamReader {
    T operator()(std::function<void(char*, size_t)>& reader) const {
        typename TypeMapper<T>::type val;
        reader(reinterpret_cast<char*>(&val), sizeof(val));
        return T { val };
    }
};

template<typename T, size_t N, template<typename> class TypeMapper>
struct StreamReader<std::array<T, N>, TypeMapper> {
    auto operator()(std::function<void(char*, size_t)>& reader) const {
        std::array<T, N> ret;
        for (auto& val : ret)
            val = StreamReader<T, TypeMapper>(reader);
        return ret;
    }
};

template<typename T, template<typename> class TypeMapper>
struct StreamWriter {
    void operator()(const T& val, std::function<void(char*, size_t)>& writer) const {
        // TODO: Check if Data is brace-intializable from T...

        typename TypeMapper<T>::type mapped_val{val};
        writer(reinterpret_cast<char*>(&mapped_val), sizeof(mapped_val));
    }
};

template<typename T, size_t N, template<typename> class TypeMapper>
struct StreamWriter<std::array<T, N>, TypeMapper> {
    void operator()(const std::array<T, N>& val, std::function<void(char*, size_t)>& writer) const {
        for (auto& elem : val)
            StreamWriter<T, TypeMapper>{}(elem, writer);
    }
};

template<typename T>
using void_t = void;

namespace detail {

struct DefaultTags : endianness_tag<Endianness::Undefined>, expected_size_tag<0> {
};

template<typename Tags, typename NewData, typename = void>
struct SelectEndiannessTagT {
    using type = endianness_tag<Tags::endianness>;
};
template<typename Tags, typename NewData>
struct SelectEndiannessTagT<Tags, NewData, Meta::void_t<decltype(NewData::Tags::endianness)>> {
    using type = endianness_tag<NewData::Tags::endianness>;
};
template<typename Tags, typename NewData>
using SelectEndiannessTag = typename SelectEndiannessTagT<Tags, NewData>::type;

template<typename Tags, typename NewData, typename = void>
struct SelectExpectedSizeTagT {
    // No size expectation for the new data unless explicitly provided
    using type = expected_size_tag<0>;
};
template<typename Tags, typename NewData>
struct SelectExpectedSizeTagT<Tags, NewData, Meta::void_t<decltype(NewData::Tags::expected_serialized_size)>> {
    using type = expected_size_tag<NewData::Tags::expected_serialized_size>;
};
template<typename Tags, typename NewData>
using SelectExpectedSizeTag = typename SelectExpectedSizeTagT<Tags, NewData>::type;

} // namespace detail

template<typename DataTags, typename NewData>
using MergeTags = Meta::inherited<detail::SelectEndiannessTag<DataTags, NewData>,
                                  detail::SelectExpectedSizeTag<DataTags, NewData>>;

template<typename Data>
using TagsOf = MergeTags<detail::DefaultTags, Data>;

template<typename Tags, typename TestTag>
static constexpr auto HasTag = std::is_base_of<TestTag, Tags>::value;

template<typename DataTags>
struct HasBigEndianTag : std::integral_constant<bool, HasTag<DataTags, big_endian_tag>> {

};

template<bool Cond, template<typename T> class OnTrue, template<typename T> class OnFalse, typename T>
using MetaConditional = typename std::conditional<Cond, OnTrue<T>, OnFalse<T>>::type;

template<typename DataTags>
struct Mapper {
    template<typename T>
    using mapper_template = MetaConditional<HasBigEndianTag<DataTags>::value, TypeMapperBigEndian, TypeMapper3DS, T>;
};

namespace hana = boost::hana;

template<typename T>
struct is_std_array : std::false_type {};

template<typename T, size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};

template<typename T, typename = void>
struct DataToTupleT;

template<typename T>
struct DataToTupleT<T, typename std::enable_if<std::is_class<T>::value && !is_std_array<T>::value>::type> {
    auto operator()(const T& data) const {
        return hana::to<hana::ext::std::tuple_tag>(hana::transform(hana::to_tuple(data), hana::second));
    }
};

template<typename T>
struct DataToTupleT<T, typename std::enable_if<!std::is_class<T>::value || is_std_array<T>::value>::type> {
    auto operator()(const T& data) const {
        return std::tuple<T>(data);
    }
};

template<typename T, typename = void>
struct MembersAsTuple;

template<typename T>
struct MembersAsTuple<T, typename std::enable_if<std::is_class<T>::value && !is_std_array<T>::value>::type> {
    using type = decltype(DataToTupleT<T>{}(std::declval<T>()));
};

template<typename T>
struct MembersAsTuple<T, typename std::enable_if<!std::is_class<T>::value || is_std_array<T>::value>::type> {
    using type = std::tuple<T>;
};

template<typename T>
using MembersAsTuple_t = typename MembersAsTuple<T>::type;

template<typename T, typename = void_t<T>>
struct SerializedSize : std::integral_constant<size_t, sizeof(T)> { };


template<size_t... Nums>
struct MakeSum;

template<>
struct MakeSum<> : std::integral_constant<size_t, 0> {};

// TODO: Static assert against overflows!
template<size_t Num1, size_t... Nums>
struct MakeSum<Num1, Nums...> : std::integral_constant<size_t, Num1 + MakeSum<Nums...>::value> {};

template<size_t... Nums>
struct MakeProduct;

template<>
struct MakeProduct<> : std::integral_constant<size_t, 1> {};

// TODO: Static assert against overflows!
template<size_t Num1, size_t... Nums>
struct MakeProduct<Num1, Nums...> : std::integral_constant<size_t, Num1 * MakeProduct<Nums...>::value> {};


// Non-array structure types only
template<typename T>
struct SerializedSize<T, typename std::enable_if<Meta::is_class_v<T> && !Meta::is_std_array_v<T> && !Meta::is_std_tuple_v<T>>::type>
    : SerializedSize<MembersAsTuple_t<T>> { };

// Arrays only
template<typename T, size_t N>
struct SerializedSize<std::array<T, N>, void>
    : MakeProduct<N, SerializedSize<T>::value> { };

// Non-structure and non-tuple types
template<typename T>
struct SerializedSize<T, typename std::enable_if<!Meta::is_class_v<T> && !Meta::is_std_tuple_v<T>>::type>
    : std::integral_constant<size_t, sizeof(T)> { };

// Collections of types
template<typename... T>
struct SerializedSize<std::tuple<T...>, void> : MakeSum<SerializedSize<T>::value...> {};


template<typename DataTags, size_t ActualSize, typename = void>
struct CheckSizeExpectations {
    // No size expectations specified, so just return a match
};

template<typename DataTags, size_t ActualSize>
struct CheckSizeExpectations<DataTags, ActualSize, typename std::enable_if<DataTags::expected_serialized_size != 0>::type> {
    static_assert(Meta::CHECK_EQUALITY_VERBOSELY<ActualSize, DataTags::expected_serialized_size>::value, "");
};

template<typename, typename, typename, typename = void>
struct ConstructFromReadCallback;

// Non-array structure types only
template<typename Data, typename DataTags, typename... T>
struct ConstructFromReadCallback<Data, DataTags, std::tuple<T...>, typename std::enable_if<std::is_class<Data>::value && !is_std_array<Data>::value>::type> {
    // TODO: Check if Data is brace-intializable from T...

    Data operator()(std::function<void(char*, size_t)>& reader) const {
        (void)CheckSizeExpectations<DataTags, SerializedSize<Data>::value>{};

        namespace hana = boost::hana;
        return Data { ConstructFromReadCallback<T, MergeTags<DataTags, T>, MembersAsTuple_t<T>>{}(reader)... };
    }
};

// Arrays only
template<typename Data, typename DataTags, typename... T>
struct ConstructFromReadCallback<Data, DataTags, std::tuple<T...>, typename std::enable_if<is_std_array<Data>::value>::type> {
    // TODO: Check if Data is brace-intializable from T...

    Data operator()(std::function<void(char*, size_t)>& reader) const {
        using Value = typename Data::value_type;

        Data ret;
        for (auto& val : ret)
            val = ConstructFromReadCallback<Value, MergeTags<DataTags, Value>, std::tuple<Value>>{}(reader);

        return ret;
    }
};

// Non-array plain elements only
template<typename Data, typename DataTags, typename... T>
struct ConstructFromReadCallback<Data, DataTags, std::tuple<T...>, typename std::enable_if<!std::is_class<Data>::value && !is_std_array<Data>::value>::type> {
    // TODO: Check if Data is brace-intializable from T...

    Data operator()(std::function<void(char*, size_t)>& reader) const {
        // TODO: Why do we bother using T here at all? Should be sufficient to just use Data directly...

        return Data { StreamReader<T, Mapper<DataTags>::template mapper_template>{}(reader)... };
    }
};

template<typename Data>
Data SerializationInterface<Data>::Load(std::function<void(char*, size_t)> reader) {
    // For small data, read to a stack buffer first
    std::array<char, sizeof(Data)> raw;
    if (sizeof(Data) <= 0x200) {
        size_t read_size = sizeof(Data);
        if constexpr (TagsOf<Data>::expected_serialized_size != 0) {
            read_size = Data::Tags::expected_serialized_size;
        }
        reader(raw.data(), read_size);
        reader = [&raw, offset = size_t { 0 }](char* dest, size_t size) mutable {
            memcpy(dest, raw.data() + offset, size);
            offset += size;
        };
    }

    namespace hana = boost::hana;
    using hana_tuple       = decltype(hana::to_tuple(std::declval<Data>()));
    using hana_types_tuple = decltype(hana::transform(hana_tuple{}, hana::second));
    using TupleType        = decltype(hana::to<hana::ext::std::tuple_tag>(hana_types_tuple{}));
    return ConstructFromReadCallback<Data, TagsOf<Data>, TupleType>{}(reader);
}

template<>
uint32_t SerializationInterface<uint32_t>::Load(std::function<void(char*, size_t)> reader);

template<>
boost::endian::little_uint32_t SerializationInterface<boost::endian::little_uint32_t>::Load(std::function<void(char*, size_t)> reader);

template<>
boost::endian::big_uint32_t SerializationInterface<boost::endian::big_uint32_t>::Load(std::function<void(char*, size_t)> reader);


template<typename, typename, typename, typename = void>
struct SaveWithWriteCallback;

template<size_t index, typename DataTags, typename MembersTuple, typename = void>
struct SaveWithWriteCallbackHelper {
    void operator()(const MembersTuple& data, std::function<void(char*, size_t)>& writer) {
        // Do nothing if index is larger than the tuple size
    }
};

template<size_t index, typename DataTags, typename MembersTuple>
//struct SaveWithWriteCallbackHelper<index, DataTags, MembersTuple, void_t<decltype(std::get<index>(std::declval<MembersTuple>()))>> {
struct SaveWithWriteCallbackHelper<index, DataTags, MembersTuple, std::enable_if_t<(index < Meta::tuple_size_v<MembersTuple>)>> {
    void operator()(const MembersTuple& data, std::function<void(char*, size_t)>& writer) {
//        using SubType = std::remove_cv_t<std::remove_reference_t<decltype(std::get<index>(data))>>;
        using SubType = std::remove_cv_t<std::remove_reference_t<std::tuple_element_t<index, MembersTuple>>>;
        SaveWithWriteCallback<SubType, MergeTags<DataTags, SubType>, MembersAsTuple_t<SubType>>{}(std::get<index>(data), writer);
        SaveWithWriteCallbackHelper<index + 1, DataTags, MembersTuple>{}(data, writer);        
    }
};

// Non-array structure types only
template<typename Data, typename DataTags, typename... T>
struct SaveWithWriteCallback<Data, DataTags, std::tuple<T...>, typename std::enable_if<std::is_class<Data>::value && !is_std_array<Data>::value>::type> {
    void operator()(const Data& data, std::function<void(char*, size_t)>& writer) const {
        (void)CheckSizeExpectations<DataTags, SerializedSize<Data>::value>{};

        namespace hana = boost::hana;
        SaveWithWriteCallbackHelper<0, DataTags, MembersAsTuple_t<Data>>{}(DataToTupleT<Data>{}(data), writer);
    }
};

// Arrays only
template<typename Data, typename DataTags, typename... T>
struct SaveWithWriteCallback<Data, DataTags, std::tuple<T...>, typename std::enable_if<is_std_array<Data>::value>::type> {
    void operator()(const Data& data, std::function<void(char*, size_t)>& writer) const {
        using Value = typename Data::value_type;

        for (auto& val : data)
            SaveWithWriteCallback<Value, MergeTags<DataTags, Value>, std::tuple<Value>>{}(val, writer);
    }
};

// Non-array plain elements only
template<typename Data, typename DataTags, typename... T>
struct SaveWithWriteCallback<Data, DataTags, std::tuple<T...>, typename std::enable_if<!std::is_class<Data>::value && !is_std_array<Data>::value>::type> {
    void operator()(const Data& data, std::function<void(char*, size_t)>& writer) const {
        StreamWriter<Data, Mapper<DataTags>::template mapper_template>{}(data, writer);
    }
};

template<typename Data>
void SerializationInterface<Data>::Save(const Data& data, std::function<void(char*, size_t)> writer) {
    namespace hana = boost::hana;
    using hana_tuple       = decltype(hana::to_tuple(std::declval<Data>()));
    using hana_types_tuple = decltype(hana::transform(hana_tuple{}, hana::second));
    using TupleType        = decltype(hana::to<hana::ext::std::tuple_tag>(hana_types_tuple{}));
    SaveWithWriteCallback<Data, TagsOf<Data>, TupleType>{}(data, writer);
}

template<>
inline void SerializationInterface<uint32_t>::Save(const uint32_t& data, std::function<void(char*, size_t)> writer) {
    SaveWithWriteCallback<uint32_t, detail::DefaultTags, std::tuple<uint32_t>>{}(data, writer);
}

template<>
void SerializationInterface<boost::endian::big_uint32_t>::Save(const boost::endian::big_uint32_t& data, std::function<void(char*, size_t)> writer);

} // namespace FileFormat
