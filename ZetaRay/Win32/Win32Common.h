#pragma once

#include "../Utility/Span.h"
#include <concepts>
#include <string.h>

namespace ZetaRay::Win32
{
    namespace SIMD_Intrinsic
    {
        constexpr uint8_t SSE3 = 0x1;
        constexpr uint8_t SSE4 = 0x2;
        constexpr uint8_t AVX = 0x4;
        constexpr uint8_t AVX2 = 0x8;
    };

    void WideToCharStr(const wchar_t* wideStr, Span<char> str) noexcept;

    uint8_t CheckSIMDSupport(uint8_t query) noexcept;

    /*
    template<typename T>
    requires std::is_same_v<T, uint16_t> || std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t>
    inline uint32_t popcnt(T v) noexcept
    {
        if constexpr (std::is_same_v<T, uint16_t>)
            return __popcnt16(v);
        else if constexpr (std::is_same_v<T, uint32_t>)
            return __popcnt(v);
        else
            return __popcnt64(v);
    }

    // Returns number of bits that are set in the given mask up to (and including) pos'th bit
    template<typename T>
    requires std::is_same_v<T, uint16_t> || std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t>
    inline uint32_t prefixsum(T mask, uint32_t pos) noexcept
    {
        mask &= (1 << (pos + 1)) - 1;

        if constexpr (std::is_same_v<T, uint16_t>)
            return __popcnt16(mask) - 1;
        else if constexpr (std::is_same_v<T, uint32_t>)
            return __popcnt(mask) - 1;
        else
            return __popcnt64(mask) - 1;
    }
    */
}