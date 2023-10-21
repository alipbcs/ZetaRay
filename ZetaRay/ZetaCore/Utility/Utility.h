#pragma once

#include "Span.h"

namespace ZetaRay::Util
{
    // performs binary seach in the range [beg, end]
    template<typename T, typename Key, typename Accessor>
    int64_t BinarySearch(Span<T> data, Key key, Accessor getMember, int64_t beg = 0, int64_t end = -1)
    {
        if (data.empty())
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

        if (getMember(data[beg]) == key)
            return beg;

        return -1;
    }

    template<typename T>
    int64_t BinarySearch(Span<T> data, T key, int64_t beg = 0, int64_t end = -1)
    {
        if (data.empty())
            return -1;

        end = end == -1 ? (int64_t)data.size() - 1 : end;

        while (beg != end)
        {
            int64_t mid = 1 + ((beg + end - 1) >> 1);

            if (data[mid] > key)
                end = mid - 1;
            else
                beg = mid;
        }

        if (data[beg] == key)
            return beg;

        return -1;
    }
}