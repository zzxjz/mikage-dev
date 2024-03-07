#pragma once

#include <tuple>
#include <type_traits>
#include <utility>

namespace Config {

/**
 * \page config Configuration Framework
 *
 * Provides frontend-agnostic access to configuration.
 * Frontend code may be written such that it is ensured that all options are
 * handled properly at compile-time. This design keeps out issues of
 * representation out of the main configuration provider. Things like parsing
 * of options and stringifying configuration are purely frontend matters
 * (especially since the frontend is the one dictating the medium and format
 * for configuration storage).
 *
 * @todo Add support for gathering options into groups
 * @todo Allow encoding some sort of "cannot be changed during emulation" property into option tags
 * @todo Add a signalling mechanism for emulator subsystems to listen for configuration changes
 * @todo Support arrays of an option
 * @todo Evaluate whether adding support for option tags referring to incomplete storage types is feasible. This may be desirable for options that are set according to enumerations dictated by the emulated platform
 *
 * \section config_option_tags Option Tags
 *
 * Instead of having a structure that stores and defines all configuration
 * entries along with their current value, we explicitly define the set of
 * configuration entries using option tags, while the actual configuration
 * storage is handled separately.
 *
 * A set of configuration entries is defined by passing a set of option tags
 * to the \ref Options template. Option tags are structures inheriting the
 * \ref Option structure (or one of its child classes provided for convenience)
 * and need to provide a number of properties to define the configuration
 * entry:
 * - a type alias named \c type defining the data type used to represent the entry value
 * - a static member-function named \c default_value that returns the default value of this entry
 * - a static C-string member named \c name specifying a human-readable identifier for this entry
 *
 * \section config_rationale Design Rationale
 *
 * Encoding default options into option tags guarantees that the internal
 * configuration storage object is always in a valid state (as opposed to being
 * created in some sort of "uninitialized" state). It also reduced duplicate
 * frontend code, since frontends can just use the default configuration rather
 * than setting it up all by themselves (in particular, the CLI backend doesn't
 * really care about the particular configuration used, anyway). On the
 * negative side, changes to the defaults in the backend may be unexpected to
 * some frontents. Furthermore, frontends need to watch out not to forget
 * adjusting their code to consider the addition of new options. For these two
 * reasons, frontends are advised to either just accept the default settings
 * and stack individual modifications on top of them or to instead override
 * all of the settings with own ones (in a way that statically ensures all
 * options are covered).
*/

/// Base option tag. All custom tags need to inherit this structure.
struct Option { Option() = delete; };

template<typename Type, typename Tag = Type>
struct OptionDefault : Option { using type = Type; static type default_val; static type default_value() { return default_val; } };

/// Convenience option structure to serve as a base for option tags representing boolean switches. default_value() must be defined by the user
/// Uses CRTP so that each instance of this type is different, so that default_value can be implemented for each setting separately
/// the member function default_value is merely provided for backwards compatibility reasons
/// TODO: Clean up docstring
template<typename CRTP>
struct BooleanOption : Option { using type = bool; static type default_val; static type default_value() { return default_val; } };

/// Convenience option structure to serve as a base for option tags representing integer-based settings with a given default value.
/// TODO: Clean up docstring
template<typename IntType, typename CRTP>
struct IntegralOption : Option { using type = IntType; static type default_val; static type default_value() { return default_val; } };

namespace detail {

// Helper type for easy lookup of a tagged element in a std::tuple
template<typename Tag, typename Data>
struct TaggedData {
    Data data;
};

} // namespace detail

/**
 * Template class defining and storing a set of options in a type-safe manner
 */
template<typename... Tags>
struct Options {
    using tags = std::tuple<Tags...>;

    using storage_type = std::tuple<detail::TaggedData<Tags, typename Tags::type>...>;
    storage_type storage { { Tags::default_value() }... };

    /// Get the value corresponding to the given option tag
    template<typename T>
    const typename T::type& get() const {
        static_assert(std::is_base_of_v<Option, T>, "Given type is not an option tag");
        return std::get<detail::TaggedData<T, typename T::type>>(storage).data;
    }

    template<typename T>
    void set(const typename T::type& data) {
        static_assert(std::is_base_of_v<Option, T>, "Given type is not an option tag");
        std::get<detail::TaggedData<T, typename T::type>>(storage).data = data;
    }
};

} // namespace Config
