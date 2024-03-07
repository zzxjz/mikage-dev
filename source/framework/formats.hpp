#pragma once

#include <boost/endian/arithmetic.hpp>

#include <algorithm>
#include <cassert>
#include <iosfwd>
#include <functional>

namespace FileFormat {

enum class Endianness {
    Big,
    Little,
    Undefined
};

template<Endianness E>
struct endianness_tag {
    static constexpr auto endianness = E;
};

using big_endian_tag = endianness_tag<Endianness::Big>;
using little_endian_tag = endianness_tag<Endianness::Little>;

struct expected_size_tag_base { };

/// Used to annotate serializeable structures with the expected total size of the serialized data (used as a static assertion)
template<size_t Size>
struct expected_size_tag : expected_size_tag_base { static constexpr auto expected_serialized_size = Size; };

template<typename T>
struct StreamIn;

template<typename T>
struct StreamOut;

/*template<>
struct StreamIn<std::istream> {
    static constexpr bool IsStreamInInstance = true;

    // TODO: Consider changing this to a template for size? Or better yet, the storage type?
    void Read(char* dest, size_t size);

private:
    std::istream stream;
};*/

template<typename ForwardIt, typename EndIt = ForwardIt>
struct StreamInFromContainer {
    StreamInFromContainer(ForwardIt begin, EndIt end) : cursor(begin), end(end) {
    }

    void Read(char* dest, size_t size) {
//        static_assert(sizeof(typename std::iterator_traits<ForwardIt>::value_type) == sizeof(char), "");
        // TODO: Assert we don't copy past the end!
        for (auto remaining = size; remaining != 0; --remaining) {
            assert(cursor != end);
            *dest++ = *cursor++;
        }
    }

    operator std::function<void(char*, size_t)>() {
        return [this](char* dest, size_t size) {
            Read(dest, size);
        };
    }

    static constexpr bool IsStreamInInstance = true;

private:
    ForwardIt cursor;
    EndIt end;
};

template<typename ForwardIt, typename EndIt = ForwardIt>
StreamInFromContainer<ForwardIt, EndIt> MakeStreamInFromContainer(ForwardIt begin, EndIt end) {
    return StreamInFromContainer<ForwardIt, EndIt>{begin, end};
}

template<typename Rng>
auto MakeStreamInFromContainer(Rng&& rng) -> StreamInFromContainer<decltype(std::begin(rng)), decltype(std::end(rng))> {
    return StreamInFromContainer { std::begin(rng), std::end(rng) };
}

template<typename ForwardIt>
struct StreamOutFromContainer {
    StreamOutFromContainer(ForwardIt begin) : cursor(begin) {
    }

    void Write(char* data, size_t size) {
        for (auto remaining = size; remaining != 0; --remaining)
            *cursor++ = *data++;
    }

    static constexpr bool IsStreamOutInstance = true;

private:
    ForwardIt cursor;
};

template<typename ForwardIt>
StreamOutFromContainer<ForwardIt> MakeStreamOutFromContainer(ForwardIt begin) {
    return StreamOutFromContainer<ForwardIt>{begin};
}

/**
 * Maps given type to a buffer type. The buffer type must be convertible to the type.
 * For instance, the type uint32_t would map to little_uint32_t.
 */
template<typename T>
struct TypeMapper3DS;

template<>
struct TypeMapper3DS<uint64_t> {
    using type = boost::endian::little_uint64_at;
};

template<>
struct TypeMapper3DS<uint32_t> {
    using type = boost::endian::little_uint32_at;
};

template<>
struct TypeMapper3DS<uint16_t> {
    using type = boost::endian::little_uint16_at;
};

template<>
struct TypeMapper3DS<uint8_t> {
    using type = boost::endian::little_uint8_at;
};

template<typename T, size_t size>
struct TypeMapper3DS<std::array<T, size>> {
    using type = std::array<typename TypeMapper3DS<T>::type, size>;
};

/**
 * Maps given type to a buffer type. The buffer type must be convertible to the type.
 * For instance, the type uint32_t would map to little_uint32_t.
 */
template<typename T>
struct TypeMapperBigEndian;

template<>
struct TypeMapperBigEndian<uint64_t> {
    using type = boost::endian::big_uint64_at;
};

template<>
struct TypeMapperBigEndian<uint32_t> {
    using type = boost::endian::big_uint32_at;
};

template<>
struct TypeMapperBigEndian<uint16_t> {
    using type = boost::endian::big_uint16_at;
};

template<>
struct TypeMapperBigEndian<uint8_t> {
    using type = boost::endian::big_uint8_at;
};

template<typename T, size_t size>
struct TypeMapperBigEndian<std::array<T, size>> {
    using type = std::array<typename TypeMapperBigEndian<T>::type, size>;
};

// TODO: TypeMapper for enum -> TypeMapper<underlyingtype>
// TODO: TypeMapper for union -> TypeMapper<uintN_t<sizeof(T)>>, iff the union has a tag "TypeMap_to_storage"

/**
 * Helper class to offload compilation-time intensive part of the implementation in a standalone
 * translation unit. The implementation is quite heavy on metaprogramming and hence would make
 * compile time explode if we were to recompile it in each unit. Unfortunately, this means we
 * cannot have a proper generic implementation in terms of the Stream parameter, but the overhead
 * of std::function is probably small compared to the actual file reading operation anyway.
 * NOTE: This is a template struct rather than a template function because that eases explicit instantiation in the main translation unit.
 */
template<typename Data>
struct SerializationInterface {
    static Data Load(std::function<void(char*, size_t)> reader);
    static void Save(const Data& data, std::function<void(char*, size_t)> writer);
};

template<typename Data, typename Stream>
Data Load(Stream& stream_in) {
    static_assert(Stream::IsStreamInInstance, "Given stream must have an IsStreamInInstance member");

    return SerializationInterface<Data>::Load([&stream_in](char* dest, size_t size) {stream_in.Read(dest, size); });
}

template<typename Data>
Data Load(std::istream& istream) {
    // Wrap in a dummy enable_if_t so that this header doesn't need to pull in <iostream> (only users of this function do)
    auto& istream2 = static_cast<std::enable_if_t<(sizeof(Data) > 0), std::istream&>>(istream);
    return SerializationInterface<Data>::Load([&istream2](char* dest, size_t size) {istream2.read(dest, size); });
}

template<typename Data, boost::endian::order Order>
Data LoadValue(std::function<void(char*, size_t)> reader) {
    boost::endian::endian_buffer<Order, Data, sizeof(Data) * 8> buffer;
    reader(reinterpret_cast<char*>(buffer.data()), sizeof(Data));
    return buffer.value();
}

template<typename Data, typename Stream>
auto Save(const Data& data, Stream& stream_out) -> std::enable_if_t<Stream::IsStreamOutInstance> {
    static_assert(Stream::IsStreamOutInstance, "Given stream must have an IsStreamOutInstance member");

    SerializationInterface<Data>::Save(data, [&stream_out](char* data, size_t size) {stream_out.Write(data, size); });
}

// Wrap in a dummy enable_if_t so that this header doesn't need to pull in <iostream> (only users of this function do)
template<typename Data>
void Save(const Data& data, std::ostream& ostream) {
    // Wrap in a dummy enable_if_t so that this header doesn't need to pull in <iostream> (only users of this function do)
    auto& ostream2 = static_cast<std::enable_if_t<(sizeof(Data) > 0), std::ostream&>>(ostream);
    SerializationInterface<Data>::Save(data, [&ostream2](char* data, size_t size) {ostream2.write(data, size); });
}

} // namespace FileFormat
