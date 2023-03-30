#pragma once

#include "Common.h"
#include <immintrin.h>	// AVX intrinsics

#define V_SHUFFLE_XYZW(fp0, fp1, fp2, fp3) (((fp3) << 6) | ((fp2) << 4) | ((fp1) << 2) | ((fp0)))
#define _MM_BLEND_XYZW(fp0, fp1, fp2, fp3) (((fp3) << 3) | ((fp2) << 2) | ((fp1) << 1) | ((fp0)))

namespace ZetaRay::Math
{
	struct float2
	{
		constexpr float2() noexcept = default;

		constexpr explicit float2(float x) noexcept
			: x(x),
			y(x)
		{}

		constexpr float2(float x, float y) noexcept
			: x(x),
			y(y)
		{}

		constexpr float2& operator+=(const float2& other) noexcept
		{
			x += other.x;
			y += other.y;

			return *this;
		}

		constexpr float2& operator-=(const float2& other) noexcept
		{
			x -= other.x;
			y -= other.y;

			return *this;
		}

		constexpr float2& operator*=(float f) noexcept
		{
			x *= f;
			y *= f;

			return *this;
		}		
		
		constexpr float2& operator*=(float2 f) noexcept
		{
			x *= f.x;
			y *= f.y;

			return *this;
		}

		constexpr float dot(const float2& other) noexcept
		{
			return x * other.x + y * other.y;
		}

		float x;
		float y;
	};

	struct float3
	{
		constexpr float3() noexcept = default;

		constexpr explicit float3(float x) noexcept
			: x(x),
			y(x),
			z(x)
		{}

		constexpr float3(float x, float y, float z) noexcept
			: x(x),
			y(y),
			z(z)
		{}

		constexpr float3(const float2& xy, float z = 0.0f) noexcept
			: x(xy.x),
			y(xy.y),
			z(z)
		{}

		constexpr float3& operator+=(const float3& other) noexcept
		{
			x += other.x;
			y += other.y;
			z += other.z;

			return *this;
		}

		constexpr float3& operator-=(const float3& other) noexcept
		{
			x -= other.x;
			y -= other.y;
			z -= other.z;

			return *this;
		}

		constexpr float3& operator*=(float3 f) noexcept
		{
			x *= f.x;
			y *= f.y;
			z *= f.z;

			return *this;
		}

		constexpr float3& operator*=(float f) noexcept
		{
			x *= f;
			y *= f;
			z *= f;

			return *this;
		}

		float length() noexcept
		{
			return sqrtf(dot(*this));
		}

		constexpr float dot(const float3& other) noexcept
		{
			return x * other.x + y * other.y + z * other.z;
		}

		constexpr float3 cross(const float3 other) noexcept
		{
			return float3(y * other.z - z * other.y, z * other.x - x * other.z, x * other.y - y * other.x);
		}

		void normalize() noexcept
		{
			float norm = length();
			if (norm <= FLT_EPSILON)
				return;
			float oneDivNorm = 1.0f / norm;

			x *= oneDivNorm;
			y *= oneDivNorm;
			z *= oneDivNorm;
		}
				
		float x;
		float y;
		float z;
	};

	struct float4
	{
		float4() noexcept = default;

		constexpr explicit float4(float f) noexcept
			: x(f),
			y(f),
			z(f),
			w(f)
		{}

		constexpr float4(float fx, float fy, float fz, float fw) noexcept
			: x(fx),
			y(fy),
			z(fz),
			w(fw)
		{}

		constexpr float4(const float3& xyz, float fw = 0.0f) noexcept
			: x(xyz.x),
			y(xyz.y),
			z(xyz.z),
			w(fw)
		{}

		constexpr float4(const float2& xy, const float2& zw) noexcept
			: x(xy.x),
			y(xy.y),
			z(zw.x),
			w(zw.y)
		{}

		constexpr float4& operator+=(const float4& other) noexcept
		{
			x += other.x;
			y += other.y;
			z += other.z;
			w += other.w;

			return *this;
		}

		constexpr float4& operator-=(const float4& other) noexcept
		{
			x -= other.x;
			y -= other.y;
			z -= other.z;
			w -= other.w;

			return *this;
		}

		constexpr float4& operator*=(float4 f) noexcept
		{
			x *= f.x;
			y *= f.y;
			z *= f.z;
			w *= f.w;

			return *this;
		}

		constexpr float4& operator*=(float f) noexcept
		{
			x *= f;
			y *= f;
			z *= f;
			w *= f;

			return *this;
		}

		float length() noexcept
		{
			__m128 vV = _mm_loadu_ps(&this->x);
			__m128 vLength = _mm_dp_ps(vV, vV, 0xff);
			vLength = _mm_sqrt_ps(vLength);

			return _mm_cvtss_f32(vLength);
		}

		constexpr float dot(const float4& other) noexcept
		{
			return x * other.x + y * other.y + z * other.z + w * other.w;
		}

		void normalize() noexcept
		{
			float norm = length();
			if (norm <= FLT_EPSILON)
				return;
			float oneDivNorm = 1.0f / norm;

			x *= oneDivNorm;
			y *= oneDivNorm;
			z *= oneDivNorm;
			w *= oneDivNorm;
		}

		float x;
		float y;
		float z;
		float w;
	};

	struct alignas(16) float4a
	{
		float4a() noexcept = default;

		constexpr explicit float4a(float f) noexcept
			: x(f),
			y(f),
			z(f),
			w(f)
		{}

		constexpr float4a(float fx, float fy, float fz, float fw) noexcept
			: x(fx),
			y(fy),
			z(fz),
			w(fw)
		{}

		constexpr float4a(float4 f) noexcept
			: x(f.x),
			y(f.y),
			z(f.z),
			w(f.w)
		{}

		constexpr float4a(float3 f, float w = 0.0f) noexcept
			: x(f.x),
			y(f.y),
			z(f.z),
			w(w)
		{}

		float x;
		float y;
		float z;
		float w;
	};

	using half = uint16_t;

	struct half2
	{
		half2() noexcept = default;

		explicit half2(float f) noexcept
		{
			half h = FloatToHalf(f);
			x = h;
			y = h;
		}

		half2(float fx, float fy) noexcept
			: x(FloatToHalf(fx)),
			y(FloatToHalf(fy))
		{}

		half x;
		half y;
	};

	struct half3
	{
		half3() noexcept = default;

		explicit half3(float f) noexcept
		{
			half h = FloatToHalf(f);
			x = h;
			y = h;
			z = h;
		}

		half3(float fx, float fy, float fz) noexcept
			: x(FloatToHalf(fx)),
			y(FloatToHalf(fy)),
			z(FloatToHalf(fz))
		{}

		half x;
		half y;
		half z;
	};

	struct half4
	{
		half4() noexcept = default;

		explicit half4(float f) noexcept
		{
			half h = FloatToHalf(f);
			x = h;
			y = h;
			z = h;
			w = h;
		}

		half4(float fx, float fy, float fz, float fw) noexcept
			: x(FloatToHalf(fx)),
			y(FloatToHalf(fy)),
			z(FloatToHalf(fz)),
			w(FloatToHalf(fw))
		{}

		half x;
		half y;
		half z;
		half w;
	};

	//--------------------------------------------------------------------------------------
	// Operator Overloading
	//--------------------------------------------------------------------------------------

	inline constexpr float2 operator-(const float2& v1, const float2& v0) noexcept
	{
		return float2(v1.x - v0.x, v1.y - v0.y);
	}		
		
	inline constexpr float2 operator+(const float2& v1, const float2& v0) noexcept
	{
		return float2(v1.x + v0.x, v1.y + v0.y);
	}

	inline constexpr float2 operator*(const float2& v0, const float2& v1) noexcept
	{
		return float2(v0.x * v1.x, v0.y * v1.y);
	}

	inline constexpr float2 operator*(const float2& v0, float f) noexcept
	{
		return float2(v0.x * f, v0.y * f);
	}

	inline constexpr float2 operator*(float f, const float2& v0) noexcept
	{
		return float2(v0.x * f, v0.y * f);
	}

	inline constexpr float2 operator/(const float2& v0, const float2& v1) noexcept
	{
		return float2(v0.x / v1.x, v0.y / v1.y);
	}

	inline constexpr float2 operator/(const float2& v0, float f) noexcept
	{
		return float2(v0.x / f, v0.y / f);
	}

	inline constexpr float2 operator/(float f, const float2& v0) noexcept
	{
		return float2(f / v0.x, f / v0.y);
	}

	inline constexpr float2 operator-(const float2& v0) noexcept
	{
		return float2(-v0.x, -v0.y);
	}

	inline constexpr float3 operator+(const float3& v0, const float3& v1) noexcept
	{
		return float3(v0.x + v1.x, v0.y + v1.y, v0.z + v1.z);
	}

	inline constexpr float3 operator-(const float3& v1, const float3& v0) noexcept
	{
		return float3(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z);
	}

	inline constexpr float3 operator*(const float3& v0, const float3& v1) noexcept
	{
		return float3(v0.x * v1.x, v0.y * v1.y, v0.z * v1.z);
	}

	inline constexpr float3 operator*(const float3& v0, float f) noexcept
	{
		return float3(v0.x * f, v0.y * f, v0.z * f);
	}

	inline constexpr float3 operator*(float f, const float3& v0) noexcept
	{
		return float3(v0.x * f, v0.y * f, v0.z * f);
	}

	inline constexpr float3 operator/(const float3& v0, const float3& v1) noexcept
	{
		return float3(v0.x / v1.x, v0.y / v1.y, v0.z / v1.z);
	}

	inline constexpr float3 operator/(const float3& v0, float f) noexcept
	{
		return float3(v0.x / f, v0.y / f, v0.z / f);
	}

	inline constexpr float3 operator/(float f, const float3& v0) noexcept
	{
		return float3(f / v0.x, f / v0.y, f / v0.z);
	}

	inline constexpr float3 operator-(const float3& v0) noexcept
	{
		return float3(-v0.x, -v0.y, -v0.z);
	}

	inline constexpr float4 operator+(const float4& v0, const float4& v1) noexcept
	{
		return float4(v0.x + v1.x, v0.y + v1.y, v0.z + v1.z, v0.w + v1.w);
	}

	inline constexpr float4 operator-(const float4& v1, const float4& v0) noexcept
	{
		return float4(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z, v1.w - v0.w);
	}

	inline constexpr float4 operator*(const float4& v0, const float4& v1) noexcept
	{
		return float4(v0.x * v1.x, v0.y * v1.y, v0.z * v1.z, v0.w * v1.w);
	}

	inline constexpr float4 operator*(const float4& v0, float f) noexcept
	{
		return float4(v0.x * f, v0.y * f, v0.z * f, v0.w * f);
	}

	inline constexpr float4 operator*(float f, const float4& v0) noexcept
	{
		return float4(v0.x * f, v0.y * f, v0.z * f, v0.w * f);
	}

	inline constexpr float4 operator/(const float4& v0, const float4& v1) noexcept
	{
		return float4(v0.x / v1.x, v0.y / v1.y, v0.z / v1.z, v0.w / v1.w);
	}

	inline constexpr float4 operator/(const float4& v0, float f) noexcept
	{
		return float4(v0.x / f, v0.y / f, v0.z / f, v0.w / f);
	}

	inline constexpr float4 operator/(float f, const float4& v0) noexcept
	{
		return float4(f / v0.x, f / v0.y, f / v0.z, f / v0.w);
	}

	inline constexpr float4 operator-(const float4& v0) noexcept
	{
		return float4(-v0.x, -v0.y, -v0.z, -v0.w);
	}
}