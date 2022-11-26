#pragma once

#include "../Utility/Error.h"
#include <math.h>
#include <float.h>
#include <string.h>

namespace ZetaRay::Math
{
	static constexpr float PI = 3.141592654f;
	static constexpr float TWO_PI = 6.283185307f;
	static constexpr float PI_DIV_2 = 1.570796327f;
	static constexpr float PI_DIV_4 = 0.7853981635f;
	static constexpr float ONE_DIV_PI = 0.318309886f;
	static constexpr float ONE_DIV_TWO_PI = 0.159154943f;
	static constexpr float ONE_DIV_FOUR_PI = 0.079577472f;
	static constexpr float ONE_DIV_180 = 1.0f / 180.0f;

	struct float3;

	// Returns the smallest power of 2 that is bigger than x
	inline constexpr size_t NextPow2(size_t x) noexcept
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
	inline constexpr bool IsPow2(size_t x) noexcept
	{
		return (x != 0) && ((x & (x - 1)) == 0);
	}

	// Aligns to nearest biggest multiple of alignment
	inline constexpr size_t AlignDown(size_t size, size_t alignment) noexcept
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
	inline constexpr size_t AlignUp(size_t x, size_t alignment) noexcept
	{
		if (alignment > 0)
		{
			//assert(((alignment - 1) & alignment) == 0);
			size_t mask = alignment - 1;
			return (x + mask) & ~mask;
		}

		return x;
	}

	template<typename T>
	inline constexpr T max(T a, T b) noexcept
	{
		return a < b ? b : a;
	}

	template<typename T>
	inline constexpr T min(T a, T b) noexcept
	{
		return a < b ? a : b;
	}

	inline bool IsNaN(float f) noexcept
	{
		// for NaN:
		//  - sign bit could be 0 or 1
		//  - all the exponent bits must be 1 and the fraction must be non-zero

		// manual bit-cast
		uint32_t temp;
		memcpy(&temp, &f, sizeof(float));

		return (temp & 0x7fffffff) > 0x7f800000;
	}

	// Solves quadratic equation a * x^2 + b * x + c = 0
	bool SolveQuadratic(float a, float b, float c, float& x1, float& x2) noexcept;

	inline float DegreeToRadians(float d) noexcept
	{
		return d * PI * ONE_DIV_180;
	}

	inline float RadiansToDegrees(float r) noexcept
	{
		return r * 180.0f * ONE_DIV_PI;
	}

	void SphericalFromCartesian(const Math::float3& w, float& theta, float& phi) noexcept;
	Math::float3 SphericalToCartesian(float theta, float phi) noexcept;

	inline size_t CeilUnsignedIntDiv(size_t x, size_t y) noexcept
	{
		Assert(x != 0, "Input must be greater than 0");
		return 1 + ((x - 1) / y);
	}

	size_t SubdivideRangeWithMin(size_t n,
		size_t maxNumGroups,
		size_t* offsets,
		size_t* sizes,
		size_t minNumElems = 0) noexcept;
}