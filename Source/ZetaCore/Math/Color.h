#pragma once

#include "VectorFuncs.h"

namespace ZetaRay::Math
{
    ZetaInline uint16_t __vectorcall Float2ToRG8(Math::float2 v)
    {
        // Last two elements are set to zero
        __m128 vRG = Math::loadFloat2(v);
        vRG = _mm_mul_ps(vRG, _mm_set1_ps(255.0f));
        // cvtps_epi32 uses the default rounding mode (round to nearest)
        __m128i vTemp = _mm_cvtps_epi32(vRG);
        vTemp = _mm_packus_epi32(vTemp, vTemp);
        vTemp = _mm_packus_epi16(vTemp, vTemp);
        uint32_t ret = _mm_cvtsi128_si32(vTemp);

        return uint16_t(ret);
    }

    ZetaInline uint32_t __vectorcall Float3ToRGB8(Math::float3 v)
    {
        __m128 vRGB = Math::loadFloat3(v);
        vRGB = _mm_mul_ps(vRGB, _mm_set1_ps(255.0f));
        // cvtps_epi32 uses the default rounding mode (round to nearest)
        __m128i vTemp = _mm_cvtps_epi32(vRGB);
        // Ref: https://stackoverflow.com/questions/29856006/sse-intrinsics-convert-32-bit-floats-to-unsigned-8-bit-integers
        vTemp = _mm_packus_epi32(vTemp, vTemp);
        vTemp = _mm_packus_epi16(vTemp, vTemp);
        uint32_t ret = _mm_cvtsi128_si32(vTemp);

        return ret;
    }

    ZetaInline uint32_t __vectorcall Float4ToRGBA8(Math::float4 v)
    {
        __m128 vRGBA = Math::loadFloat4(v);
        vRGBA = _mm_mul_ps(vRGBA, _mm_set1_ps(255.0f));
        // cvtps_epi32 uses the default rounding mode (round to nearest)
        __m128i vTemp = _mm_cvtps_epi32(vRGBA);
        vTemp = _mm_packus_epi32(vTemp, vTemp);
        vTemp = _mm_packus_epi16(vTemp, vTemp);
        uint32_t ret = _mm_cvtsi128_si32(vTemp);

        return ret;
    }

    ZetaInline Math::float3 UnpackRGB8(uint32_t rgb)
    {
        Math::float3 ret;
        ret.x = float(rgb & 0xff) / 255.0f;
        ret.y = float((rgb >> 8) & 0xff) / 255.0f;
        ret.z = float((rgb >> 16) & 0xff) / 255.0f;

        return ret;
    }

    ZetaInline Math::float4 UnpackRGBA8(uint32_t rgba)
    {
        Math::float4 ret;
        ret.x = float(rgba & 0xff) / 255.0f;
        ret.y = float((rgba >> 8) & 0xff) / 255.0f;
        ret.z = float((rgba >> 16) & 0xff) / 255.0f;
        ret.w = float((rgba >> 24) & 0xff) / 255.0f;

        return ret;
    }

    Math::float3 sRGBToLinear(const Math::float3& color);
    Math::float3 ColorTemperatureTosRGB(float temperature);
}