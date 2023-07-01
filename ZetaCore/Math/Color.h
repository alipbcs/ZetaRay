#pragma once

#include "Vector.h"

namespace ZetaRay::Math
{
	ZetaInline uint16_t Float2ToRG(float u, float v)
	{
		uint16_t r = uint16_t(u * 255.0f);
		uint16_t g = uint16_t(v * 255.0f);

		uint16_t ret = r | (g << 8);

		return ret;
	}

	ZetaInline uint16_t Float2ToRG(Math::float2 v)
	{
		uint16_t r = uint16_t(v.x * 255.0f);
		uint16_t g = uint16_t(v.y * 255.0f);

		uint16_t ret = r | (g << 8);

		return ret;
	}

	ZetaInline uint32_t Float3ToRGB(Math::float3 v)
	{
		uint32_t r = uint32_t(v.x * 255.0f);
		uint32_t g = uint32_t(v.y * 255.0f);
		uint32_t b = uint32_t(v.z * 255.0f);

		uint32_t ret = r | (g << 8) | (b << 16);

		return ret;
	}

	ZetaInline uint32_t Float3ToRGB(float fr, float fg, float fb)
	{
		uint32_t r = uint32_t(fr * 255.0f);
		uint32_t g = uint32_t(fg * 255.0f);
		uint32_t b = uint32_t(fb * 255.0f);

		uint32_t ret = r | (g << 8) | (b << 16);

		return ret;
	}

	ZetaInline uint32_t Float4ToRGBA(Math::float4 v)
	{
		uint32_t r = uint32_t(v.x * 255.0f);
		uint32_t g = uint32_t(v.y * 255.0f);
		uint32_t b = uint32_t(v.z * 255.0f);
		uint32_t a = uint32_t(v.w * 255.0f);

		uint32_t ret = r | (g << 8) | (b << 16) | (a << 24);

		return ret;
	}

	ZetaInline uint32_t Float4ToRGBA(Math::float3 fv, float fa)
	{
		uint32_t r = uint32_t(fv.x * 255.0f);
		uint32_t g = uint32_t(fv.y * 255.0f);
		uint32_t b = uint32_t(fv.z * 255.0f);
		uint32_t a = uint32_t(fa * 255.0f);

		uint32_t ret = r | (g << 8) | (b << 16) | (a << 24);

		return ret;
	}

	ZetaInline uint32_t Float4ToRGBA(float fr, float fg, float fb, float fa)
	{
		uint32_t r = uint32_t(fr * 255.0f);
		uint32_t g = uint32_t(fg * 255.0f);
		uint32_t b = uint32_t(fb * 255.0f);
		uint32_t a = uint32_t(fa * 255.0f);

		uint32_t ret = r | (g << 8) | (b << 16) | (a << 24);

		return ret;
	}
}