#pragma once

#include <climits>
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace v2 {

namespace BitField {

template<typename>
struct Fields;

namespace detail {

// TODO: Consider adding a policy parameter; a candidate for a policy is bounds-checking and wrapping/clamping the argument to Set()
template<std::size_t Position, std::size_t Bits, typename ParentClass, typename Storage, typename Exposure = Storage>
class Field {
    friend struct Fields<Storage>;

    ParentClass object;

    constexpr Field(ParentClass object) : object(object) {}

    // Store intermediate computations as integral_constants to make sure even the dumbest compiler can inline our member functions
    // NOTE: the double Storage cast in full_mask is intended: E.g. if Storage is uint64_t and assuming 0 is of type int32_t, ~0 wouldn't match the expected 0xffffffffffffffff. On the other hand, if Storage is unsigned char, ~(Storage)0 gets promoted to the int value -1.
    static constexpr auto full_mask = std::integral_constant<Storage, static_cast<Storage>(~Storage{0})>::value;
    static constexpr auto field_mask = std::integral_constant<Storage, ((full_mask >> (8 * sizeof(Storage) - Bits)) << Position)>::value;
    static constexpr auto neg_field_mask = std::integral_constant<Storage, static_cast<Storage>(~field_mask)>::value;

public:
    operator Exposure() const {
        return static_cast<Exposure>((object.storage & field_mask) >> Position);
    }

    // Setter: Copies the storage of the parent instance and returns the manipulated value (contained in the new parent instance)
    ParentClass Set(Exposure value) const {
        //static_assert(std::is_base_of_v<BitsOnBase, ParentClass>, "Given ParentClass does not inherit BitsOnBase");
        static_assert(sizeof(ParentClass) == sizeof(Storage), "ParentClass has additional members other than storage");

        auto ret = object;
        ret.storage = (object.storage & neg_field_mask) | ((static_cast<Storage>(value) << Position) & field_mask);
        return ret;
    }

    /// Concise version of casting to Exposure
    Exposure operator ()() const {
        return static_cast<Exposure>(*this);
    }

    /// Concise version of Set(value)
    ParentClass operator ()(Exposure value) const {
        return Set(value);
    }

    static constexpr Exposure MaxValue() {
        // TODO: This may overflow (e.g. Bits=32, Exposure=uint32_t)
        return static_cast<Exposure>((Storage{1} << Bits) - 1);
    }

    // TODO: SignedBitField
};

// Base class defining the Field substruct. Expects ParentClass to have a single (inherited or non-inherited) member called "storage" of unsigned type (custom arithmetic allowed, howver, e.g. to implement endianness conversion).
template<typename Storage, typename ParentClass>
struct BitsOnBase {
    template<std::size_t Pos, std::size_t Bits, typename Exposure = Storage>
    auto MakeField() const {
        return Fields<Storage>{}.Make();
    }
};

} // namespace detail

/// BitFields on a given Storage type
/// TODO: Consider refactoring MakeOn to be a static constexpr function object rather than a function
template<typename Storage>
struct Fields {
    template<std::size_t Pos, std::size_t Bits, typename ParentClass, typename Exposure = Storage>
    static auto MakeOn(ParentClass obj) {
        return detail::Field<Pos, Bits, ParentClass, Storage, Exposure>{ obj };
    }

    /// Convenience overload to allow directly passing in a this pointer
    template<std::size_t Pos, std::size_t Bits, typename ParentClass, typename Exposure = Storage>
    static auto MakeOn(const ParentClass* obj) {
        return detail::Field<Pos, Bits, ParentClass, Storage, Exposure>{ *obj };
    }

    /// TODO: Privatize
    template<std::size_t Pos, std::size_t Bits, typename Exposure>
    struct MakeOn2Type {
        template<typename ParentClass>
        constexpr auto operator()(ParentClass obj) const {
            return detail::Field<Pos, Bits, ParentClass, Storage, Exposure>{ obj };
        }
    };

    template<std::size_t Pos, std::size_t Bits, typename Exposure = Storage>
    static constexpr auto MakeOn2 = MakeOn2Type<Pos, Bits, Exposure>{};
};


struct OwnStoragePolicy {};

/**
 * Convenience class to allow for terser syntax and to automatially allocate storage.
 * Child classes only define the actual fields.
 */
template<typename Storage, typename ParentClass, typename Policy = OwnStoragePolicy>
struct BitsOn : detail::BitsOnBase<Storage, ParentClass> {
    // Rely on ParentClass providing a "storage" member unless ProvideStorage is true (see specialization below)
};

template<typename Storage, typename ParentClass>
struct BitsOn<Storage, ParentClass, OwnStoragePolicy> : detail::BitsOnBase<Storage, ParentClass> {
    // Automatically allocate the "storage" member
    Storage storage;
};

} // namespace BitField

} // namespace v2

namespace v3 {

namespace BitField {

namespace detail {

template<std::size_t Bits>
using uint_least_n_bits =
    std::conditional_t<Bits <= sizeof(std::uint_least8_t) * CHAR_BIT, std::uint_least8_t,
        std::conditional_t<Bits <= sizeof(std::uint_least16_t) * CHAR_BIT, std::uint_least16_t,
            std::conditional_t<Bits <= sizeof(std::uint_least32_t) * CHAR_BIT, std::uint_least32_t,
                std::conditional_t<Bits <= sizeof(std::uint_least64_t) * CHAR_BIT, std::uint_least64_t,
                    std::uintmax_t>>>>;

template<std::size_t Position, std::size_t Bits, typename ParentClass, typename Storage, typename Exposure = Storage>
class Field {
public:
    ParentClass object;

    constexpr Field(ParentClass object) : object(object) {}

    // Store intermediate computations as integral_constants to make sure even the dumbest compiler can inline our member functions
    // NOTE: the double Storage cast in full_mask is intended: E.g. if Storage is uint64_t and assuming 0 is of type int32_t, ~0 wouldn't match the expected 0xffffffffffffffff. On the other hand, if Storage is unsigned char, ~(Storage)0 gets promoted to the int value -1.
    static constexpr auto full_mask = std::integral_constant<Storage, static_cast<Storage>(~Storage{0})>::value;
    static constexpr auto field_mask = std::integral_constant<Storage, ((full_mask >> (8 * sizeof(Storage) - Bits)) << Position)>::value;
    static constexpr auto neg_field_mask = std::integral_constant<Storage, static_cast<Storage>(~field_mask)>::value;

    // TODO: static_assert the Exposure type can store up to Bits bits. Refer to "operator Exposure()" to see how to distinguish the various cases

public:
    constexpr operator Exposure() const {
        // Extract the bit field value and convert it to Exposure. The latter step is trivial for scalar Exposures, but can be tricky for class-types:
        // * For aggregates, assign through a structured binding (because brace-initialization could be narrowing)

        if constexpr (std::is_class_v<ParentClass>) {
            auto [storage] = object;
            if constexpr (std::is_scalar_v<Exposure>) {
                return static_cast<Exposure>((storage & field_mask) >> Position);
            } else {
                // * use aggregate-initialization for aggregate types, or
                // * call single-argument constructor for non-aggregate types
                //
                // In both cases, the result type of the bit field extraction
                // may be larger than the aggregate member or constructor
                // argument. To prevent narrowing warnings in this case, we
                // static_cast the intermediate value to the smallest integer
                // that can hold Bits bits

                // TODO: How well will this work with signed storages?

                // TODO: Consider adding a customization point for this conversion
                return Exposure { static_cast<uint_least_n_bits<Bits>>((storage & field_mask) >> Position) };
            }
        } else {
            auto storage = object;
//            return Exposure { (storage & field_mask) >> Position };
            return static_cast<Exposure>((storage & field_mask) >> Position);
        }
    }

    // Setter: Copies the storage of the parent instance and returns the manipulated value (contained in the new parent instance)
    constexpr ParentClass Set(Exposure value) const {
        //static_assert(std::is_base_of_v<BitsOnBase, ParentClass>, "Given ParentClass does not inherit BitsOnBase");
        static_assert(sizeof(ParentClass) == sizeof(Storage), "ParentClass has additional members other than storage");

        auto ret = object;
        if constexpr (std::is_class_v<ParentClass>) {
            auto& [storage] = ret;
            storage = (storage & neg_field_mask) | ((static_cast<Storage>(value) << Position) & field_mask);
        } else {
            auto storage = object;
            ret = (storage & neg_field_mask) | ((static_cast<Storage>(value) << Position) & field_mask);
        }
        return ret;
    }

    /// Concise version of casting to Exposure
    constexpr Exposure operator ()() const {
        return static_cast<Exposure>(*this);
    }

    /// Concise version of Set(value)
    constexpr ParentClass operator ()(Exposure value) const {
        return Set(value);
    }

    static constexpr Exposure MaxValue() {
        return Exposure { (Storage{1} << Bits) - 1 };
    }

    static constexpr std::size_t NumBits() noexcept {
        return Bits;
    }
};

template<typename T>
constexpr auto GetStorage(const T& t) {
    auto& [storage] = t;
    return storage;
}

// TODO: Instead of using a PointerToMember here, a generic lens would allow for accessing e.g. arrays!
// TODO: Define generic Lens concept: Must be a callable with signature Class -> (Member -> Member) -> (Member, Class)
template<size_t Position, size_t Bits, typename ParentClass, typename MemberLens,
         typename Exposure = typename MemberLens::MemberType>
class FieldNew {
    using Storage = typename MemberLens::MemberType;

public:
    ParentClass object;

    constexpr FieldNew(ParentClass object) : object(object) {}

    // Store intermediate computations as integral_constants to make sure even the dumbest compiler can inline our member functions
    // NOTE: the double Storage cast in full_mask is intended: E.g. if Storage is uint64_t and assuming 0 is of type int32_t, ~0 wouldn't match the expected 0xffffffffffffffff. On the other hand, if Storage is unsigned char, ~(Storage)0 gets promoted to the int value -1.
    static constexpr auto full_mask = std::integral_constant<Storage, static_cast<Storage>(~Storage{0})>::value;
    static constexpr auto field_mask = std::integral_constant<Storage, ((full_mask >> (8 * sizeof(Storage) - Bits)) << Position)>::value;
    static constexpr auto neg_field_mask = std::integral_constant<Storage, static_cast<Storage>(~field_mask)>::value;

public:
    constexpr operator Exposure() const {
        return static_cast<Exposure>((MemberLens::get(object) & field_mask) >> Position);
    }

    // Setter: Copies the storage of the parent instance and returns the manipulated value (contained in the new parent instance)
    ParentClass Set(Exposure value) const {
        return MemberLens::modify(   *this,
                                    [&](const auto& data) { return (data & neg_field_mask) | ((static_cast<Storage>(value) << Position) & field_mask); });
    }

    /// Concise version of casting to Exposure
    constexpr Exposure operator ()() const {
        return static_cast<Exposure>(*this);
    }

    /// Concise version of Set(value)
    constexpr ParentClass operator ()(Exposure value) const {
        return Set(value);
    }

    static constexpr Exposure MaxValue() {
        // TODO: This may overflow (e.g. Bits=32, Exposure=uint32_t)
        return static_cast<Exposure>((Storage{1} << Bits) - 1);
    }

    // TODO: SignedBitField
};

template<auto PointerToMember>
struct PMDLens;

template<typename Member, typename Class, Member Class::*PointerToMember>
struct PMDLens<PointerToMember> {
    using MemberType = Member;

    template<typename Modifier>
    static Class modify(Class&& object, Modifier&& modifier) {
        // TODO: Should avoid copy for rvalue objects
        auto ret = std::forward<Class>(object);
        ret.*PointerToMember = std::forward<Modifier>(modifier)(ret.*PointerToMember);
        return ret;
    }

    template<typename Modifier>
    static Class modify(const Class& object, Modifier&& modifier) {
        auto ret = object;
        ret.*PointerToMember = std::forward<Modifier>(modifier)(ret.*PointerToMember);
        return ret;
    }

    static MemberType get(Class&& object) {
        return std::forward<Class>(object).*PointerToMember;
    }

    static MemberType get(const Class& object) {
        return object.*PointerToMember;
    }
};

} // namespace detail

template<std::size_t Position, std::size_t Bits, typename Exposure, typename ParentClass>
constexpr auto MakeFieldOn(const ParentClass* t) {
    if constexpr (std::is_class_v<ParentClass>) {
        using Storage = decltype(detail::GetStorage(std::declval<ParentClass>()));
        return detail::Field<Position, Bits, ParentClass, Storage, Exposure> { *t };
    } else {
        return detail::Field<Position, Bits, ParentClass, ParentClass, Exposure> { *t };
    }
}

template<std::size_t Position, std::size_t Bits, typename ParentClass>
constexpr auto MakeFieldOn(const ParentClass* t) {
    if constexpr (std::is_class_v<ParentClass>) {
        using Storage = decltype(detail::GetStorage(std::declval<ParentClass>()));
        return detail::Field<Position, Bits, ParentClass, Storage, Storage> { *t };
    } else {
        return detail::Field<Position, Bits, ParentClass, ParentClass, ParentClass> { *t };
    }
}

// Usage, e.g.: auto enabled() const { return BitField::v3::MakeFlagOn< 0, &StencilTest::raw1>(this); }
template<auto member, size_t Position, size_t Bits, typename Exposure = typename detail::PMDLens<member>::MemberType, typename ParentClass>
constexpr detail::FieldNew<Position, Bits, ParentClass, detail::PMDLens<member>, Exposure>
MakeFieldOn(const ParentClass* t) {
    static_assert(std::is_class_v<ParentClass>, "Must only call this overload on class types");
    static_assert(std::is_member_pointer_v<decltype(member)>, "Given member must be a pointer-to-member");
    static_assert(std::is_invocable_v<decltype(member), ParentClass>, "Given object type is not compatible with the pointer-to-member");

    return { *t };
}

template<auto member, size_t Position, typename ParentClass>
constexpr detail::FieldNew<Position, 1, ParentClass, detail::PMDLens<member>, bool>
MakeFlagOn(const ParentClass* t) {
    static_assert(std::is_class_v<ParentClass>, "Must only call this overload on class types");
    static_assert(std::is_member_pointer_v<decltype(member)>, "Given member must be a pointer-to-member");
    static_assert(std::is_invocable_v<decltype(member), ParentClass>, "Given object type is not compatible with the pointer-to-member");

    return { *t };
}


template<std::size_t Position, typename ParentClass>
constexpr auto MakeFlagOn(const ParentClass* t) {
    if constexpr (std::is_class_v<ParentClass>) {
        using Storage = decltype(detail::GetStorage(std::declval<ParentClass>()));
        return detail::Field<Position, 1, ParentClass, Storage, bool> { *t };
    } else {
        return detail::Field<Position, 1, ParentClass, ParentClass, bool> { *t };
    }
}

} // namespace BitField

} // namespace v3

namespace BitField {

namespace v2 {
using namespace ::v2::BitField;
}

namespace v3 {
using namespace ::v3::BitField;
}

}
