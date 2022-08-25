#include "Win32Common.h"
#include "Win32.h"
#include "../Utility/Error.h"
#include <intrin.h>

using namespace ZetaRay;
using namespace ZetaRay::Util;

void Win32::WideToCharStr(const wchar_t* wideStr, Span<char> str) noexcept
{
	int size = WideCharToMultiByte(CP_UTF8, 0, wideStr, -1, nullptr, 0, nullptr, nullptr);
	Assert(str.size() > size, "buffer overflow");

	WideCharToMultiByte(CP_UTF8, 0, wideStr, -1, str.data(), size, nullptr, nullptr);
}

uint8_t Win32::CheckSIMDSupport(uint8_t query) noexcept
{
    uint8_t ret = 0;

    // All x64 processors support SSE2 support,
    // following code checks for SSE3, SSE4, AVX and AVX2 support.

    // EAX, EBX, ECX, EDX
    int cpuInfo[4] = { 0 };
    __cpuid(cpuInfo, 1);

    if ((cpuInfo[2] & (0x1 | (1 << 9))) == (0x1 | (1 << 9)))
    {
        ret |= SIMD_Intrinsic::SSE3;
    }
    if ((cpuInfo[2] & ((1 << 20) | (1 << 19))) == ((1 << 20) | (1 << 19)))
    {
        ret |= SIMD_Intrinsic::SSE4;
    }
    if (cpuInfo[2] & (1 << 28))
    {
        ret |= SIMD_Intrinsic::AVX;
    }

    memset(cpuInfo, 0, 4 * sizeof(0));
    __cpuid(cpuInfo, 0x7);
    if (cpuInfo[1] & (1 << 5))
    {
        ret |= SIMD_Intrinsic::AVX2;
    }

    return ret;
}