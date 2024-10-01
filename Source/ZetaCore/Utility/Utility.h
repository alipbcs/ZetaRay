#pragma once

#include "Span.h"

namespace ZetaRay::Util
{
    // Callable type Accessor that accepts type T and returns type Key
    template<typename T, typename Key, typename Accessor>
    concept GetKey = requires(Accessor a, T t) {
        { a(t) } -> std::same_as<Key>;
    };

    // Alternatively:
    // template<typename T, typename Key, typename Accessor> 
    // requires GetKey<T, Key, Accessor>
    // int64_t BinarySearch(Span<T> data, Key key, Accessor getMember, int64_t beg = 0, int64_t end = -1)
    // requires requires(Accessor a, T t) { { a(t) } -> std::same_as<Key>; }

    // Performs binary search in the range [beg, end)
    template<typename T, typename Key, typename Accessor>
    requires GetKey<T, Key, Accessor>
    int64_t BinarySearch(Span<T> data, Key key, Accessor getMember, int64_t beg = 0, int64_t end = -1)
    {
        if (data.empty())
            return -1;

        end = end == -1 ? (int64_t)data.size() : end;

        while (beg < end)
        {
            int64_t mid = (beg + end) >> 1;

            if (getMember(data[mid]) < key)
                beg = mid + 1;
            else
                end = mid;
        }

        if (beg >= (int64_t)data.size() || getMember(data[beg]) != key)
            return -1;

        return beg;
    }

    template<typename T>
    requires std::integral<T>
    int64_t BinarySearch(Span<T> data, T key, int64_t beg = 0, int64_t end = -1)
    {
        if (data.empty())
            return -1;

        end = end == -1 ? (int64_t)data.size() : end;

        while (beg < end)
        {
            int64_t mid = (beg + end) >> 1;

            if (data[mid] < key)
                beg = mid + 1;
            else
                end = mid;
        }

        if (beg >= (int64_t)data.size() || data[beg] != key)
            return -1;

        return beg;
    }

    // Callable type Accessor that accepts type T and returns type Key
    template<typename T, typename Key, typename Accessor>
    concept GetFloatKey = std::floating_point<Key> && 
        requires(Accessor a, T t) 
        {
            { a(t) } -> std::same_as<Key>;
        };

    // Performs binary search in the range [beg, end]
    template<typename T, typename Key, typename Accessor>
    requires GetFloatKey<T, Key, Accessor>
    int64_t FindInterval(Span<T> data, Key key, Accessor getMember, int64_t beg = 0, int64_t end = -1)
    {
        if (data.size() < 2)
            return -1;

        end = end == -1 ? (int64_t)data.size() - 1 : end;

        while (beg != end)
        {
            int64_t mid = 1 + ((beg + end - 1) >> 1);

            if (getMember(data[mid]) > key)
                end = mid - 1;
            else
                beg = mid;
        }

        if (getMember(data[beg]) <= key && getMember(data[beg + 1]) > key)
            return beg;

        return -1;
    }

    ZetaInline uint32_t XXH3_64_To_32(uint64_t hash)
    {
        // Ref: https://github.com/Cyan4973/xxHash/issues/453
        return hash & 0xffffffff;
    }
}