#pragma once

#include "../Utility/Error.h"
#include <float.h>
#include <string.h>
#include <immintrin.h>    // AVX intrinsics
#include <cmath>

namespace ZetaRay::Util
{
    template<typename T>
    struct Span;

    template<typename T>
    struct MutableSpan;
}

namespace ZetaRay::Math
{
    struct float3;
}

namespace ZetaRay::Math
{
    static constexpr float PI = 3.141592654f;
    static constexpr float TWO_PI = 6.283185307f;
    static constexpr float PI_OVER_2 = 1.570796327f;
    static constexpr float PI_OVER_4 = 0.7853981635f;
    static constexpr float ONE_OVER_PI = 0.318309886f;
    static constexpr float ONE_OVER_2_PI = 0.159154943f;
    static constexpr float ONE_OVER_4_PI = 0.079577472f;

    // Returns the smallest power of 2 that is larger than x
    ZetaInline constexpr size_t NextPow2(size_t x)
    {
        x--;
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        x |= x >> 8;
        x |= x >> 16;
        x |= x >> 32;

        return ++x;
    }

    // Returns whether x is a power of 2
    ZetaInline constexpr bool IsPow2(size_t x)
    {
        return (x != 0) && ((x & (x - 1)) == 0);
    }

    // Aligns to nearest largest multiple of alignment
    ZetaInline constexpr size_t AlignDown(size_t size, size_t alignment)
    {
        if (alignment > 0)
        {
            //assert(((alignment - 1) & alignment) == 0);
            size_t mask = alignment - 1;
            return size & ~mask;
        }

        return size;
    }

    // Aligns to nearest smallest multiple of alignment
    template<typename T>
    ZetaInline constexpr T AlignUp(T x, T alignment)
    {
        if (alignment > 0)
        {
            T mask = alignment - 1;
            return (x + mask) & ~mask;
        }

        return x;
    }

    template<typename T>
    ZetaInline constexpr T Max(T a, T b)
    {
        return a < b ? b : a;
    }

    template<typename T>
    ZetaInline constexpr T Min(T a, T b)
    {
        return a < b ? a : b;
    }

    ZetaInline bool IsNaN(float f)
    {
        // For NaN:
        //  - sign bit could be 0 or 1
        //  - all the exponent bits must be 1 and the fraction must be non-zero

        // bit-cast
        uint32_t temp;
        memcpy(&temp, &f, sizeof(float));

        return (temp & 0x7fffffff) > 0x7f800000;
    }

    // Solves quadratic equation a * x^2 + b * x + c = 0
    bool SolveQuadratic(float a, float b, float c, float& x1, float& x2);

    ZetaInline constexpr float DegreesToRadians(float d)
    {
        return d * TWO_PI / 360.0f;
    }

    ZetaInline constexpr float RadiansToDegrees(float r)
    {
        return r * 360.0f * ONE_OVER_2_PI;
    }

    void SphericalFromCartesian(const Math::float3& w, float& theta, float& phi);
    Math::float3 SphericalToCartesian(float theta, float phi);

    // Returns x / y, where x and y are unsigned integers
    template<typename T>
    ZetaInline constexpr T CeilUnsignedIntDiv(T x, T y)
    {
        return 1 + ((x - 1) / y);
    }

    // For x \in [0, 2^24] (there may be round-off errors for larger integers)
    ZetaInline constexpr uint32_t Ceil(float x)
    {
        return (static_cast<float>(static_cast<uint32_t>(x)) == x)
            ? static_cast<uint32_t>(x)
            : static_cast<uint32_t>(x) + 1;
    }

    // Subdivides range [0, n) into at most maxNumGroups subsets where each subset has
    // at least minNumElems elements
    size_t SubdivideRangeWithMin(size_t n,
        size_t maxNumGroups,
        Util::MutableSpan<size_t> offsets,
        Util::MutableSpan<size_t> sizes,
        size_t minNumElems = 0);

    // Ref: https://walbourn.github.io/directxmath-f16c-and-fma/
    ZetaInline float HalfToFloat(uint16_t value)
    {
        __m128i V1 = _mm_cvtsi32_si128(static_cast<uint32_t>(value));
        __m128 V2 = _mm_cvtph_ps(V1);
        return _mm_cvtss_f32(V2);
    }

    ZetaInline uint16_t FloatToHalf(float value)
    {
        __m128 V1 = _mm_set_ss(value);
        __m128i V2 = _mm_cvtps_ph(V1, 0);
        return static_cast<uint16_t>(_mm_cvtsi128_si32(V2));
    }

    ZetaInline uint8_t FloatToUNorm8(float value)
    {
        return (uint8_t)std::fmaf(value, 255.0f, 0.5f);
    }

    ZetaInline uint16_t FloatToUNorm16(float value)
    {
        return (uint16_t)std::fmaf(value, float((1 << 16) - 1), 0.5f);
    }

    ZetaInline float UNorm8ToFloat(uint8_t value)
    {
        return value / 255.0f;
    }

    ZetaInline float UNorm16ToFloat(uint8_t value)
    {
        return value / float((1 << 16) - 1);
    }

    // A summation algorithm that guards against the worst-case loss of precision when summing
    // a large sequence of floating-point numbers.
    float KahanSum(Util::Span<float> data);
}