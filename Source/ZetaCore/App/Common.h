#pragma once

#include "../Utility/Span.h"
#include <string.h>

namespace ZetaRay::App::Common
{
    namespace CPU_Intrinsic
    {
        constexpr uint16_t SSE3 = 0x1;
        constexpr uint16_t SSE4 = 0x2;
        constexpr uint16_t AVX = 0x4;
        constexpr uint16_t AVX2 = 0x8;
        constexpr uint16_t F16C = 0x10;
        constexpr uint16_t BMI1 = 0x20;
    };

    int WideToCharStr(const wchar_t* wideStr, Util::MutableSpan<char> str);
    int CharToWideStr(const char* str, Util::MutableSpan<wchar_t> wideStr);

    uint32_t CheckIntrinsicSupport();
}