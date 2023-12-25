#include "../App/Common.h"
#include "Win32.h"
#include "../Utility/Error.h"
#include <intrin.h>

using namespace ZetaRay;
using namespace ZetaRay::Util;
using namespace ZetaRay::App;

int Common::WideToCharStr(const wchar_t* wideStr, MutableSpan<char> str)
{
    int size = WideCharToMultiByte(CP_UTF8, 0, wideStr, -1, nullptr, 0, nullptr, nullptr);
    Assert(str.size() > size, "buffer overflow");

    WideCharToMultiByte(CP_UTF8, 0, wideStr, -1, str.data(), Math::Min(size, (int)str.size()), nullptr, nullptr);

    return size;
}

int Common::CharToWideStr(const char* str, Util::MutableSpan<wchar_t> wideStr)
{
    // #size includes terminating null character
    const int size = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
    Assert(wideStr.size() >= size, "Provided buffer is too small.");

    MultiByteToWideChar(CP_UTF8, 0, str, -1, wideStr.data(), (int)wideStr.size());

    return size;
}

uint32_t Common::CheckIntrinsicSupport()
{
    uint32_t ret = 0;

    // All x64 processors support SSE2,
    // following code checks for SSE3, SSE4, AVX, F16C, AVX2 and BMI1 support

    // EAX, EBX, ECX, EDX
    int cpuInfo[4] = { 0 };
    __cpuid(cpuInfo, 1);

    if ((cpuInfo[2] & (0x1 | (1 << 9))) == (0x1 | (1 << 9)))
        ret |= CPU_Intrinsic::SSE3;
    if ((cpuInfo[2] & ((1 << 20) | (1 << 19))) == ((1 << 20) | (1 << 19)))
        ret |= CPU_Intrinsic::SSE4;
    if (cpuInfo[2] & (1 << 28))
        ret |= CPU_Intrinsic::AVX;
    if (cpuInfo[2] & (1 << 29))
        ret |= CPU_Intrinsic::F16C;

    memset(cpuInfo, 0, 4 * sizeof(0));
    __cpuid(cpuInfo, 0x7);

    if (cpuInfo[1] & (1 << 5))
        ret |= CPU_Intrinsic::AVX2;
    if (cpuInfo[1] & (1 << 3))
        ret |= CPU_Intrinsic::BMI1;

    return ret;
}