#pragma once

#include <range/v3/view/iota.hpp>
#include <iterator> // for std::size of C arrays

namespace ranges::views {

/// Returns a view of indexes into the given range
template<random_access_range Rng>
inline auto indexes(Rng&& rng) noexcept {
    using std::size;
    using index_t = decltype(size(rng));
    return ranges::views::iota(index_t { 0 }, size(rng));
}

}
