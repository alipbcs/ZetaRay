#pragma once

#include "VectorFuncs.h"

namespace ZetaRay::Math
{
	ZetaInline uint16_t __vectorcall Float2ToRG8(Math::float2 v) noexcept
	{
		// last two elements are set to zero
		__m128 vRG = Math::loadFloat2(v);
		vRG = _mm_mul_ps(vRG, _mm_set1_ps(255.0f));

		__m128i vTemp = _mm_cvtps_epi32(vRG);
		vTemp = _mm_packus_epi32(vTemp, vTemp);
		vTemp = _mm_packus_epi16(vTemp, vTemp);
		uint32_t ret = _mm_cvtsi128_si32(vTemp);

		return uint16_t(ret);
	}

	ZetaInline uint32_t __vectorcall Float3ToRGB8(Math::float3 v) noexcept
	{
		__m128 vRGB = Math::loadFloat3(v);
		vRGB = _mm_mul_ps(vRGB, _mm_set1_ps(255.0f));

		// Ref: https://stackoverflow.com/questions/29856006/sse-intrinsics-convert-32-bit-floats-to-unsigned-8-bit-integers
		__m128i vTemp = _mm_cvtps_epi32(vRGB);
		vTemp = _mm_packus_epi32(vTemp, vTemp);
		vTemp = _mm_packus_epi16(vTemp, vTemp);
		uint32_t ret = _mm_cvtsi128_si32(vTemp);

		return ret;
	}

	ZetaInline uint32_t __vectorcall Float4ToRGBA8(Math::float4 v) noexcept
	{
		__m128 vRGBA = Math::loadFloat4(v);
		vRGBA = _mm_mul_ps(vRGBA, _mm_set1_ps(255.0f));

		__m128i vTemp = _mm_cvtps_epi32(vRGBA);
		vTemp = _mm_packus_epi32(vTemp, vTemp);
		vTemp = _mm_packus_epi16(vTemp, vTemp);
		uint32_t ret = _mm_cvtsi128_si32(vTemp);

		return ret;
	}
}