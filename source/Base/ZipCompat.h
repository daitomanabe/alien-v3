#pragma once

// std::views::zip requires libstdc++ >= 13. The CUDA host compiler on some build
// hosts (e.g. nvcc 12.1 + GCC 11) cannot use GCC 13, so provide a minimal
// substitute there. Remove once the minimum toolchain ships __cpp_lib_ranges_zip.

#include <ranges>

#if defined(__cpp_lib_ranges_zip)

namespace aliencompat
{
inline constexpr auto zip = []<typename... Rs>(Rs&&... rs) { return std::views::zip(std::forward<Rs>(rs)...); };
}

#else

#include <iterator>
#include <tuple>
#include <utility>

namespace aliencompat
{
// Minimal zip view: forward iteration, stops at the shortest range, yields a
// tuple of references. Covers the range-based for loops in this codebase; not
// a full std::views::zip replacement.
template <typename... Rs>
class ZipView
{
public:
    explicit ZipView(Rs... rs)
        : _ranges(std::forward<Rs>(rs)...)
    {}

    class Iterator
    {
    public:
        using Its = std::tuple<decltype(std::begin(std::declval<Rs&>()))...>;

        explicit Iterator(Its its)
            : _its(std::move(its))
        {}

        auto operator*() const
        {
            return std::apply([](auto&... its) { return std::tuple<decltype(*its)...>(*its...); }, _its);
        }

        Iterator& operator++()
        {
            std::apply([](auto&... its) { ((void)++its, ...); }, _its);
            return *this;
        }

        bool operator!=(Iterator const& other) const { return notEqual(other, std::index_sequence_for<Rs...>{}); }

    private:
        template <std::size_t... I>
        bool notEqual(Iterator const& other, std::index_sequence<I...>) const
        {
            // Stop as soon as any range is exhausted (shortest-range semantics).
            return ((std::get<I>(_its) != std::get<I>(other._its)) && ...);
        }

        Its _its;
    };

    auto begin() { return Iterator(std::apply([](auto&... rs) { return std::make_tuple(std::begin(rs)...); }, _ranges)); }
    auto end() { return Iterator(std::apply([](auto&... rs) { return std::make_tuple(std::end(rs)...); }, _ranges)); }

private:
    std::tuple<Rs...> _ranges;
};

template <typename... Rs>
auto zip(Rs&&... rs)
{
    return ZipView<Rs...>(std::forward<Rs>(rs)...);
}
}

#endif
