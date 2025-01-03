#pragma once

#define _HAS_EXCEPTIONS 0

#include <cstdint>

// Ref: https://www.foonathan.net/2020/09/move-forward/
#define ZetaMove(x) static_cast<std::remove_reference_t<decltype(x)>&&>(x)
#define ZetaForward(x) static_cast<decltype(x)&&>(x)

#define ZetaInline __forceinline
#define ZetaArrayLen(x) sizeof(x) / sizeof(x[0])

#define uint8 uint8_t
#define uint16 uint16_t
#define uint32 uint32_t
#define uint64 uint64_t
#define int8 int8_t
#define int16 int16_t
#define int32 int32_t
#define int64 int64_t