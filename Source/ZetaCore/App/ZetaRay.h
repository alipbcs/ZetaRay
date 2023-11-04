#pragma once

#ifndef _M_X64
#error 64-bit platform is required
#endif

#define _HAS_EXCEPTIONS 0

#include <cstdint>

#pragma warning(disable : 4996) // _CRT_SECURE_NO_WARNINGS
#pragma warning(disable : 4101) // unreferenced local variable
#pragma warning(disable : 4100) // unreferenced formal parameter
#pragma warning(disable : 4189) // local variable is initialized but not referenced
#pragma warning(disable : 4324) // structure was padded due to alignment specifier

#define ZetaMove(x) static_cast<std::remove_reference_t<decltype(x)>&&>(x)
#define ZetaForward(x) static_cast<decltype(x)&&>(x)
#define ZetaInline __forceinline
#define ZetaArrayLen(x) sizeof(x) / sizeof(x[0])

#define ZETA_MAX_NUM_THREADS 16
#define ZETA_THREAD_ID_TYPE uint32_t
