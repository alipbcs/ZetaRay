#pragma once

#include "Common.h"

#define V_SHUFFLE_XYZW(fp0, fp1, fp2, fp3) (((fp3) << 6) | ((fp2) << 4) | ((fp1) << 2) | ((fp0)))
#define V_BLEND_XYZW(fp0, fp1, fp2, fp3) (((fp3) << 3) | ((fp2) << 2) | ((fp1) << 1) | ((fp0)))

namespace ZetaRay::Math
{
	struct half2;
	struct half3;
	struct half4;

	struct float2
	{
		constexpr float2() = default;

		constexpr explicit float2(float x)
			: x(x),
			y(x)
		{}

		constexpr float2(float x, float y)
			: x(x),
			y(y)
		{}

		float2(const half2& h);

		constexpr float2& operator+=(const float2& other)
		{
			x += other.x;
			y += other.y;

			return *this;
		}

		constexpr float2& operator-=(const float2& other)
		{
			x -= other.x;
			y -= other.y;

			return *this;
		}

		constexpr float2& operator*=(float f)
		{
			x *= f;
			y *= f;

			return *this;
		}		
		
		constexpr float2& operator*=(float2 f)
		{
			x *= f.x;
			y *= f.y;

			return *this;
		}

		constexpr float dot(const float2& other)
		{
			return x * other.x + y * other.y;
		}

		float x;
		float y;
	};

	struct float3
	{
		constexpr float3() = default;

		constexpr explicit float3(float x)
			: x(x),
			y(x),
			z(x)
		{}

		constexpr float3(float x, float y, float z)
			: x(x),
			y(y),
			z(z)
		{}

		constexpr float3(const float2& xy, float z = 0.0f)
			: x(xy.x),
			y(xy.y),
			z(z)
		{}

		float3(const half3& h);

		constexpr float3& operator+=(const float3& other)
		{
			x += other.x;
			y += other.y;
			z += other.z;

			return *this;
		}

		constexpr float3& operator-=(const float3& other)
		{
			x -= other.x;
			y -= other.y;
			z -= other.z;

			return *this;
		}

		constexpr float3& operator*=(float3 f)
		{
			x *= f.x;
			y *= f.y;
			z *= f.z;

			return *this;
		}

		constexpr float3& operator*=(float f)
		{
			x *= f;
			y *= f;
			z *= f;

			return *this;
		}

		float length()
		{
			return sqrtf(dot(*this));
		}

		constexpr float dot(const float3& other)
		{
			return x * other.x + y * other.y + z * other.z;
		}

		constexpr float3 cross(const float3 other)
		{
			return float3(y * other.z - z * other.y, z * other.x - x * other.z, x * other.y - y * other.x);
		}

		void normalize()
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
		float4() = default;

		constexpr explicit float4(float f)
			: x(f),
			y(f),
			z(f),
			w(f)
		{}

		constexpr float4(float fx, float fy, float fz, float fw)
			: x(fx),
			y(fy),
			z(fz),
			w(fw)
		{}

		constexpr float4(const float3& xyz, float fw = 0.0f)
			: x(xyz.x),
			y(xyz.y),
			z(xyz.z),
			w(fw)
		{}

		constexpr float4(const float2& xy, const float2& zw)
			: x(xy.x),
			y(xy.y),
			z(zw.x),
			w(zw.y)
		{}

		float4(const half4& h);

		constexpr float4& operator+=(const float4& other)
		{
			x += other.x;
			y += other.y;
			z += other.z;
			w += other.w;

			return *this;
		}

		constexpr float4& operator-=(const float4& other)
		{
			x -= other.x;
			y -= other.y;
			z -= other.z;
			w -= other.w;

			return *this;
		}

		constexpr float4& operator*=(float4 f)
		{
			x *= f.x;
			y *= f.y;
			z *= f.z;
			w *= f.w;

			return *this;
		}

		constexpr float4& operator*=(float f)
		{
			x *= f;
			y *= f;
			z *= f;
			w *= f;

			return *this;
		}

		float length()
		{
			__m128 vV = _mm_loadu_ps(&this->x);
			__m128 vLength = _mm_dp_ps(vV, vV, 0xff);
			vLength = _mm_sqrt_ps(vLength);

			return _mm_cvtss_f32(vLength);
		}

		constexpr float dot(const float4& other)
		{
			return x * other.x + y * other.y + z * other.z + w * other.w;
		}

		void normalize()
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
		float4a() = default;

		constexpr explicit float4a(float f)
			: x(f),
			y(f),
			z(f),
			w(f)
		{}

		constexpr float4a(float fx, float fy, float fz, float fw)
			: x(fx),
			y(fy),
			z(fz),
			w(fw)
		{}

		constexpr float4a(float4 f)
			: x(f.x),
			y(f.y),
			z(f.z),
			w(f.w)
		{}

		constexpr float4a(float3 f, float w = 0.0f)
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

	struct half
	{
		half() = default;
		explicit half(float f)
			: x(FloatToHalf(f))
		{}

		uint16_t x;
	};

	struct half2
	{
		half2() = default;

		explicit half2(float f)
		{
			uint16_t h = FloatToHalf(f);
			x = h;
			y = h;
		}

		half2(float fx, float fy)
			: x(FloatToHalf(fx)),
			y(FloatToHalf(fy))
		{}

		half2(float2 f)
			: x(FloatToHalf(f.x)),
			y(FloatToHalf(f.y))
		{}

		uint16_t x;
		uint16_t y;
	};

	struct half3
	{
		half3() = default;

		explicit half3(float f)
		{
			uint16_t h = FloatToHalf(f);
			x = h;
			y = h;
			z = h;
		}

		half3(float fx, float fy, float fz)
			: x(FloatToHalf(fx)),
			y(FloatToHalf(fy)),
			z(FloatToHalf(fz))
		{}

		half3(float3 f)
		{
			// &v does not need to be aligned and the last two elements are set to 0
			__m128 vXY = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&f)));
			// &v.z does not need to be aligned
			__m128 vZ = _mm_load_ss(&f.z);
			__m128 vF = _mm_insert_ps(vXY, vZ, 0x20);

			__m128i vH = _mm_cvtps_ph(vF, 0);
			vH = _mm_unpacklo_epi16(vH, _mm_castps_si128(_mm_setzero_ps()));

			x = static_cast<uint16_t>(_mm_cvtsi128_si32(vH));
			y = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x1)));
			z = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x2)));
		}

		half3(float4a f)
		{
			__m128 vF = _mm_load_ps(reinterpret_cast<float*>(&f));
			__m128i vH = _mm_cvtps_ph(vF, 0);
			vH = _mm_unpacklo_epi16(vH, _mm_castps_si128(_mm_setzero_ps()));

			x = static_cast<uint16_t>(_mm_cvtsi128_si32(vH));
			y = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x1)));
			z = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x2)));
		}

		uint16_t x;
		uint16_t y;
		uint16_t z;
	};

	struct half4
	{
		half4() = default;

		explicit half4(float f)
		{
			uint16_t h = FloatToHalf(f);
			x = h;
			y = h;
			z = h;
			w = h;
		}

		half4(float fx, float fy, float fz, float fw)
			: x(FloatToHalf(fx)),
			y(FloatToHalf(fy)),
			z(FloatToHalf(fz)),
			w(FloatToHalf(fw))
		{}

		half4(float4 f)
		{
			__m128 vF = _mm_loadu_ps(reinterpret_cast<float*>(&f));
			__m128i vH = _mm_cvtps_ph(vF, 0);
			vH = _mm_unpacklo_epi16(vH, _mm_castps_si128(_mm_setzero_ps()));

			x = static_cast<uint16_t>(_mm_cvtsi128_si32(vH));
			y = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x1)));
			z = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x2)));
			w = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x3)));
		}

		half4(float4a f)
		{
			__m128 vF = _mm_load_ps(reinterpret_cast<float*>(&f));
			__m128i vH = _mm_cvtps_ph(vF, 0);
			vH = _mm_unpacklo_epi16(vH, _mm_castps_si128(_mm_setzero_ps()));

			x = static_cast<uint16_t>(_mm_cvtsi128_si32(vH));
			y = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x1)));
			z = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x2)));
			w = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x3)));
		}

		uint16_t x;
		uint16_t y;
		uint16_t z;
		uint16_t w;
	};

	inline float2::float2(const half2& h)
		: x(HalfToFloat(h.x)),
		y(HalfToFloat(h.y))
	{}

	inline float3::float3(const half3& h)
		: x(HalfToFloat(h.x)),
		y(HalfToFloat(h.y)),
		z(HalfToFloat(h.z))
	{}

	inline float4::float4(const half4& h)
		: x(HalfToFloat(h.x)),
		y(HalfToFloat(h.y)),
		z(HalfToFloat(h.z)),
		w(HalfToFloat(h.w))
	{}

	//--------------------------------------------------------------------------------------
	// Operator Overloading
	//--------------------------------------------------------------------------------------

	inline constexpr float2 operator-(const float2& v1, const float2& v0)
	{
		return float2(v1.x - v0.x, v1.y - v0.y);
	}		
		
	inline constexpr float2 operator+(const float2& v1, const float2& v0)
	{
		return float2(v1.x + v0.x, v1.y + v0.y);
	}

	inline constexpr float2 operator*(const float2& v0, const float2& v1)
	{
		return float2(v0.x * v1.x, v0.y * v1.y);
	}

	inline constexpr float2 operator*(const float2& v0, float f)
	{
		return float2(v0.x * f, v0.y * f);
	}

	inline constexpr float2 operator*(float f, const float2& v0)
	{
		return float2(v0.x * f, v0.y * f);
	}

	inline constexpr float2 operator/(const float2& v0, const float2& v1)
	{
		return float2(v0.x / v1.x, v0.y / v1.y);
	}

	inline constexpr float2 operator/(const float2& v0, float f)
	{
		return float2(v0.x / f, v0.y / f);
	}

	inline constexpr float2 operator/(float f, const float2& v0)
	{
		return float2(f / v0.x, f / v0.y);
	}

	inline constexpr float2 operator-(const float2& v0)
	{
		return float2(-v0.x, -v0.y);
	}

	inline constexpr float3 operator+(const float3& v0, const float3& v1)
	{
		return float3(v0.x + v1.x, v0.y + v1.y, v0.z + v1.z);
	}

	inline constexpr float3 operator-(const float3& v1, const float3& v0)
	{
		return float3(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z);
	}

	inline constexpr float3 operator*(const float3& v0, const float3& v1)
	{
		return float3(v0.x * v1.x, v0.y * v1.y, v0.z * v1.z);
	}

	inline constexpr float3 operator*(const float3& v0, float f)
	{
		return float3(v0.x * f, v0.y * f, v0.z * f);
	}

	inline constexpr float3 operator*(float f, const float3& v0)
	{
		return float3(v0.x * f, v0.y * f, v0.z * f);
	}

	inline constexpr float3 operator/(const float3& v0, const float3& v1)
	{
		return float3(v0.x / v1.x, v0.y / v1.y, v0.z / v1.z);
	}

	inline constexpr float3 operator/(const float3& v0, float f)
	{
		return float3(v0.x / f, v0.y / f, v0.z / f);
	}

	inline constexpr float3 operator/(float f, const float3& v0)
	{
		return float3(f / v0.x, f / v0.y, f / v0.z);
	}

	inline constexpr float3 operator-(const float3& v0)
	{
		return float3(-v0.x, -v0.y, -v0.z);
	}

	inline constexpr float4 operator+(const float4& v0, const float4& v1)
	{
		return float4(v0.x + v1.x, v0.y + v1.y, v0.z + v1.z, v0.w + v1.w);
	}

	inline constexpr float4 operator-(const float4& v1, const float4& v0)
	{
		return float4(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z, v1.w - v0.w);
	}

	inline constexpr float4 operator*(const float4& v0, const float4& v1)
	{
		return float4(v0.x * v1.x, v0.y * v1.y, v0.z * v1.z, v0.w * v1.w);
	}

	inline constexpr float4 operator*(const float4& v0, float f)
	{
		return float4(v0.x * f, v0.y * f, v0.z * f, v0.w * f);
	}

	inline constexpr float4 operator*(float f, const float4& v0)
	{
		return float4(v0.x * f, v0.y * f, v0.z * f, v0.w * f);
	}

	inline constexpr float4 operator/(const float4& v0, const float4& v1)
	{
		return float4(v0.x / v1.x, v0.y / v1.y, v0.z / v1.z, v0.w / v1.w);
	}

	inline constexpr float4 operator/(const float4& v0, float f)
	{
		return float4(v0.x / f, v0.y / f, v0.z / f, v0.w / f);
	}

	inline constexpr float4 operator/(float f, const float4& v0)
	{
		return float4(f / v0.x, f / v0.y, f / v0.z, f / v0.w);
	}

	inline constexpr float4 operator-(const float4& v0)
	{
		return float4(-v0.x, -v0.y, -v0.z, -v0.w);
	}
}