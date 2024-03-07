#include <framework/bit_field_new.hpp>

#include <catch2/catch.hpp>

// TODO: Integrate into cmake
#include <boost/endian/arithmetic.hpp>

using namespace BitField::v3;

enum MyUnscopedEnum {
    MyUnscopedEnumValue_0x45 = 0x45,
    MyUnscopedEnumValue_0xab = 0xab,
};

enum class MyScopedEnum {
    Value_0x23 = 0x23,
    Value_0xcd = 0xcd,
};

struct OneElementStruct {
    uint8_t element;
};

constexpr bool operator==(const OneElementStruct& a, const OneElementStruct& b) {
    return a.element == b.element;
}

struct OneElementStructWithNonDefaultConstructor {
    explicit constexpr OneElementStructWithNonDefaultConstructor(uint8_t value) : element(value) {
    }

    uint8_t element;
};

constexpr bool operator==(const OneElementStructWithNonDefaultConstructor& a, const OneElementStructWithNonDefaultConstructor& b) {
    return a.element == b.element;
}

struct Test {
    uint_least32_t value;

    constexpr auto full_value() const { return MakeFieldOn<0, 32>(this); }

    constexpr auto byte0() const { return MakeFieldOn<0, 8>(this); }
    constexpr auto byte1() const { return MakeFieldOn<8, 8>(this); }
    constexpr auto byte2() const { return MakeFieldOn<16, 8>(this); }
    constexpr auto byte3() const { return MakeFieldOn<24, 8>(this); }

    // Boolean flags
    constexpr auto flag0() const { return MakeFlagOn<24>(this); }
    constexpr auto flag1() const { return MakeFlagOn<25>(this); }

    // Signed values
    constexpr auto byte3_signed() const { return MakeFieldOn<24, 8, int8_t>(this); }

    // Different exposures
    constexpr auto byte1_as_unscoped_enum() const { return MakeFieldOn<8, 8, MyUnscopedEnum>(this); }
    constexpr auto byte2_as_scoped_enum() const { return MakeFieldOn<16, 8, MyScopedEnum>(this); }
    constexpr auto byte3_as_struct() const { return MakeFieldOn<24, 8, OneElementStruct>(this); }
    constexpr auto byte0_as_nondefault_constructed_struct() const { return MakeFieldOn<0, 8, OneElementStructWithNonDefaultConstructor>(this); }
};

// TODO: Can BitField support using boost::endian::little_uintX_t as Storage?

TEST_CASE("BitField") {
    // TODO: Access through plain integral value

    SECTION("Simple access through parent structure")
    {
        constexpr auto test = Test { 0x1234567 };

        // Read bit fields
        static_assert(test.full_value() == 0x1234567);

        static_assert(test.byte0() == 0x67);
        static_assert(test.byte1() == 0x45);
        static_assert(test.byte2() == 0x23);
        static_assert(test.byte3() == 0x01);

        static_assert(test.byte3_signed() == 1);

        static_assert(test.flag0());
        static_assert(!test.flag1());

        static_assert(test.byte1_as_unscoped_enum() == MyUnscopedEnumValue_0x45);
        static_assert(test.byte2_as_scoped_enum() == MyScopedEnum::Value_0x23);
        static_assert(test.byte3_as_struct() == OneElementStruct { 0x01 });
        static_assert(test.byte0_as_nondefault_constructed_struct() == OneElementStructWithNonDefaultConstructor { 0x67 });


        // Create new value using bit field modifiers
        {
            constexpr auto modified_test = test.byte0()(0x89).byte1()(0xab).byte2()(0xcd).byte3()(0xef);
            static_assert(std::is_same_v<decltype(modified_test), const Test>); // Ensure the return value is of the input structure type

            static_assert(modified_test.full_value() == 0xefcdab89);

            static_assert(modified_test.byte0() == 0x89);
            static_assert(modified_test.byte1() == 0xab);
            static_assert(modified_test.byte2() == 0xcd);
            static_assert(modified_test.byte3() == 0xef);

            static_assert(modified_test.byte3_signed() == -17);

            static_assert(modified_test.flag0());
            static_assert(modified_test.flag1());

            static_assert(modified_test.byte1_as_unscoped_enum() == MyUnscopedEnumValue_0xab);
            static_assert(modified_test.byte2_as_scoped_enum() == MyScopedEnum::Value_0xcd);
            static_assert(modified_test.byte3_as_struct() == OneElementStruct { 0xef });
            static_assert(modified_test.byte0_as_nondefault_constructed_struct() == OneElementStructWithNonDefaultConstructor { 0x89 });
        }

        // Modify via signed integer
        // NOTE: Signed integers are two's complement
        {
            static_assert(test.byte3_signed()(-128).byte3_signed() == -128);
            static_assert(test.byte3_signed()(-127).byte3_signed() == -127);
            static_assert(test.byte3_signed()(127).byte3_signed() == 127);
            static_assert(test.byte3_signed()(0).byte3_signed() == 0);

            static_assert(test.byte3_signed()(-128).full_value() == 0x80234567);
            static_assert(test.byte3_signed()(-127).full_value() == 0x81234567);
            static_assert(test.byte3_signed()(127).full_value() == 0x7f234567);
            static_assert(test.byte3_signed()(0).full_value() == 0x0234567);
        }

        // Modify via flags
        {
            constexpr auto modified_test = test.flag0()(!test.flag0()).flag1()(!test.flag1());
            static_assert(modified_test.full_value() == 0x2234567);
            static_assert(!modified_test.flag0());
            static_assert(modified_test.flag1());
        }

        // Modify via enums
        {
            constexpr auto modified_test = test.byte1_as_unscoped_enum()(MyUnscopedEnumValue_0xab).byte2_as_scoped_enum()(MyScopedEnum::Value_0xcd);
            static_assert(modified_test.full_value() == 0x1cdab67);
            static_assert(modified_test.byte1_as_unscoped_enum() == MyUnscopedEnumValue_0xab);
            static_assert(modified_test.byte2_as_scoped_enum() == MyScopedEnum::Value_0xcd);
        }

        // TODO: Also support modifying OneElementStruct and OneElementStructWithNonDefaultConstructor
    }
}
