#pragma once

#include "../Utility/Span.h"
#include <string.h>

namespace ZetaRay::App::Common
{
    namespace SIMD_Intrinsic
    {
        constexpr uint8_t SSE3 = 0x1;
        constexpr uint8_t SSE4 = 0x2;
        constexpr uint8_t AVX = 0x4;
        constexpr uint8_t AVX2 = 0x8;
        constexpr uint8_t F16C = 0xf;
    };

    int WideToCharStr(const wchar_t* wideStr, Util::Span<char> str) noexcept;
    int CharToWideStr(const char* str, Util::Span<wchar_t> wideStr) noexcept;

    uint8_t CheckSIMDSupport() noexcept;
}