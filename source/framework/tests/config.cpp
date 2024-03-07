#include "framework/config_framework.hpp"

#include <catch2/catch.hpp>

#include <string>

struct IntegerTag : Config::Option {
    static constexpr const char* name = "IntegerTag";
    using type = int;
    static type default_value() { return -1; }
};

struct BooleanOptionTagTrue : Config::BooleanOption<BooleanOptionTagTrue> {
    static constexpr const char* name = "BooleanTagTrue";
};

struct BooleanOptionTagFalse : Config::BooleanOption<BooleanOptionTagFalse> {
    static constexpr const char* name = "BooleanTagFalse";
};

template<>
bool Config::BooleanOption<BooleanOptionTagTrue>::default_val = true;
template<>
bool Config::BooleanOption<BooleanOptionTagFalse>::default_val = false;

struct StructTag : Config::Option {
    struct Data {
        std::string string;
        bool operator == (const Data& d) const {
            return string == d.string;
        }
    };
    static constexpr const char* name = "StructTag";
    using type = Data;
    static type default_value() { return {"Hello World"}; }
};

using Settings = Config::Options<IntegerTag, BooleanOptionTagTrue, BooleanOptionTagFalse, StructTag>;

TEST_CASE("Default Settings") {
    Settings settings;

    REQUIRE(settings.get<IntegerTag>() == -1);
    REQUIRE(settings.get<BooleanOptionTagTrue>() == true);
    REQUIRE(settings.get<BooleanOptionTagFalse>() == false);
    REQUIRE(settings.get<StructTag>() == StructTag::Data { "Hello World" });
}
