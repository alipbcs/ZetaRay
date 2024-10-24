#pragma once

#include "Common.h"

#define V_SHUFFLE_XYZW(fp0, fp1, fp2, fp3) (((fp3) << 6) | ((fp2) << 4) | ((fp1) << 2) | ((fp0)))
#define V_BLEND_XYZW(fp0, fp1, fp2, fp3) (((fp3) << 3) | ((fp2) << 2) | ((fp1) << 1) | ((fp0)))

namespace ZetaRay::Math
{
    struct half2;
    struct half3;
    struct half4;
    struct alignas(16) float4a;

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

        constexpr float dot(const float2& other) const
        {
            return x * other.x + y * other.y;
        }

        float length() const
        {
            return sqrtf(dot(*this));
        }

        void normalize()
        {
            float norm = length();
            if (norm <= FLT_EPSILON)
                return;
            float oneDivNorm = 1.0f / norm;

            x *= oneDivNorm;
            y *= oneDivNorm;
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
        explicit float3(const half3& h);

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
        constexpr float2 xy() const
        {
            return float2(x, y);
        }
        constexpr float2 yz() const
        {
            return float2(y, z);
        }

        float length() const
        {
            return sqrtf(dot(*this));
        }

        constexpr float dot(const float3& other) const
        {
            return x * other.x + y * other.y + z * other.z;
        }

        constexpr float3 cross(const float3 other) const
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
        constexpr float4() = default;
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
        float4(const float4a& f);

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
        constexpr float2 xy() const
        {
            return float2(x, y);
        }
        constexpr float2 yz() const
        {
            return float2(y, z);
        }
        constexpr float2 zw() const
        {
            return float2(z, w);
        }
        constexpr float3 xyz() const
        {
            return float3(x, y, z);
        }
        constexpr float3 yzw() const
        {
            return float3(y, z, w);
        }

        float length() const
        {
            __m128 vV = _mm_loadu_ps(&this->x);
            __m128 vLength = _mm_dp_ps(vV, vV, 0xff);
            vLength = _mm_sqrt_ps(vLength);

            return _mm_cvtss_f32(vLength);
        }

        constexpr float dot(const float4& other) const
        {
            return x * other.x + y * other.y + z * other.z + w * other.w;
        }

        void normalize()
        {
            float norm = length();
            Assert(norm > 1e-7, "Divide-by-zero.");
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
        constexpr float2 xy() const
        {
            return float2(x, y);
        }
        constexpr float2 yz() const
        {
            return float2(y, z);
        }
        constexpr float2 zw() const
        {
            return float2(z, w);
        }
        constexpr float3 xyz() const
        {
            return float3(x, y, z);
        }
        constexpr float3 yzw() const
        {
            return float3(y, z, w);
        }

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

        static half asfloat16(uint16_t v)
        {
            half h;
            h.x = v;

            return h;
        }

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
        {
            float2 f(fx, fy);

            // &f does not need to be aligned and the last two elements are set to 0
            __m128 vF = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&f)));
            __m128i vH = _mm_cvtps_ph(vF, 0);

            // Doesn't violate strict aliasing: https://stackoverflow.com/questions/13257166/print-a-m128i-variable
            alignas(16) uint16_t v[8];
            _mm_store_si128((__m128i*)v, vH);

            x = v[0];
            y = v[1];
        }

        explicit half2(const float2& f)
        {
            // &f does not need to be aligned and the last two elements are set to 0
            __m128 vF = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&(const_cast<float2&>(f)))));
            __m128i vH = _mm_cvtps_ph(vF, 0);

            // Doesn't violate strict aliasing: https://stackoverflow.com/questions/13257166/print-a-m128i-variable
            alignas(16) uint16_t v[8];
            _mm_store_si128((__m128i*)v, vH);

            x = v[0];
            y = v[1];
        }

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
        {
            float4a f(fx, fy, fz, 0);
            __m128 vF = _mm_load_ps(reinterpret_cast<float*>(&f));
            __m128i vH = _mm_cvtps_ph(vF, 0);

            // Doesn't violate strict aliasing: https://stackoverflow.com/questions/13257166/print-a-m128i-variable
            alignas(16) uint16_t v[8];
            _mm_store_si128((__m128i*)v, vH);

            x = v[0];
            y = v[1];
            z = v[2];
        }

        explicit half3(const float3& f)
        {
            // &v does not need to be aligned and the last two elements are set to 0
            __m128 vXY = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&(const_cast<float3&>(f)))));
            // &v.z does not need to be aligned
            __m128 vZ = _mm_load_ss(&f.z);
            __m128 vF = _mm_insert_ps(vXY, vZ, 0x20);

            __m128i vH = _mm_cvtps_ph(vF, 0);

            // Doesn't violate strict aliasing: https://stackoverflow.com/questions/13257166/print-a-m128i-variable
            alignas(16) uint16_t v[8];
            _mm_store_si128((__m128i*)v, vH);

            x = v[0];
            y = v[1];
            z = v[2];
        }

        explicit half3(const float4a& f)
        {
            __m128 vF = _mm_load_ps(reinterpret_cast<float*>(&(const_cast<float4a&>(f))));
            __m128i vH = _mm_cvtps_ph(vF, 0);

            // Doesn't violate strict aliasing: https://stackoverflow.com/questions/13257166/print-a-m128i-variable
            alignas(16) uint16_t v[8];
            _mm_store_si128((__m128i*)v, vH);

            x = v[0];
            y = v[1];
            z = v[2];
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
        {
            float4a f(fx, fy, fz, fw);
            __m128 vF = _mm_load_ps(reinterpret_cast<float*>(&f));
            __m128i vH = _mm_cvtps_ph(vF, 0);

            // Doesn't violate strict aliasing: https://stackoverflow.com/questions/13257166/print-a-m128i-variable
            alignas(16) uint16_t v[8];
            _mm_store_si128((__m128i*)v, vH);

            x = v[0];
            y = v[1];
            z = v[2];
            w = v[3];
        }

        explicit half4(const float4& f)
        {
            __m128 vF = _mm_loadu_ps(reinterpret_cast<float*>(&(const_cast<float4&>(f))));
            __m128i vH = _mm_cvtps_ph(vF, 0);

            // Doesn't violate strict aliasing: https://stackoverflow.com/questions/13257166/print-a-m128i-variable
            alignas(16) uint16_t v[8];
            _mm_store_si128((__m128i*)v, vH);

            x = v[0];
            y = v[1];
            z = v[2];
            w = v[3];
        }

        uint16_t x;
        uint16_t y;
        uint16_t z;
        uint16_t w;
    };

    struct uint3
    {
        uint3() = default;
        constexpr explicit uint3(uint32_t x)
            : x(x),
            y(x),
            z(x)
        {}
        constexpr uint3(uint32_t e0, uint32_t e1, uint32_t e2)
            : x(e0), y(e1), z(e2)
        {}

        uint32_t x;
        uint32_t y;
        uint32_t z;
    };

    struct unorm2
    {
        unorm2() = default;
        explicit unorm2(uint16_t u)
            : x(u),
            y(u)
        {}
        unorm2(uint16_t u, uint16_t v)
            : x(u),
            y(v)
        {}

        // Encodes float2 in [-1, 1]
        static unorm2 FromNormalized(float2 u)
        {
            unorm2 ret;

            __m128 vV = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&u)));
            __m128 vHalf = _mm_set1_ps(0.5f);
            // [-1, 1] -> [0, 1] 
            vV = _mm_fmadd_ps(vV, vHalf, vHalf);

            __m128 vMax = _mm_set1_ps((1 << 16) - 1);
            __m128 vTemp = _mm_mul_ps(vV, vMax);
            // cvtps_epi32 uses the default rounding mode (round to nearest)
            __m128i vEncoded = _mm_cvtps_epi32(vTemp);

            // Doesn't violate strict aliasing: https://stackoverflow.com/questions/13257166/print-a-m128i-variable
            alignas(16) uint32_t a[4];
            _mm_store_si128(reinterpret_cast<__m128i*>(a), vEncoded);

            ret.x = static_cast<uint16_t>(a[0]);
            ret.y = static_cast<uint16_t>(a[1]);

            return ret;
        }

        // Encodes float2 in [-1, 1]
        static unorm2 FromNormalized(__m128 vV)
        {
            unorm2 ret;

            __m128 vHalf = _mm_set1_ps(0.5f);
            // [-1, 1] -> [0, 1] 
            vV = _mm_fmadd_ps(vV, vHalf, vHalf);

            __m128 vMax = _mm_set1_ps((1 << 16) - 1);
            __m128 vTemp = _mm_mul_ps(vV, vMax);
            // cvtps_epi32 uses the default rounding mode (round to nearest)
            __m128i vEncoded = _mm_cvtps_epi32(vTemp);

            // Doesn't violate strict aliasing: https://stackoverflow.com/questions/13257166/print-a-m128i-variable
            alignas(16) uint32_t a[4];
            _mm_store_si128(reinterpret_cast<__m128i*>(a), vEncoded);

            ret.x = static_cast<uint16_t>(a[0]);
            ret.y = static_cast<uint16_t>(a[1]);

            return ret;
        }

        uint16_t x;
        uint16_t y;
    };

    struct unorm3
    {
        unorm3() = default;
        explicit unorm3(uint16_t u)
            : x(u),
            y(u),
            z(u)
        {}
        unorm3(uint16_t u0, uint16_t u1, uint16_t u2)
            : x(u0),
            y(u1),
            z(u2)
        {}

        // Encodes float3 in [-1, 1]
        static unorm3 FromNormalized(float u0, float u1, float u2)
        {
            unorm3 ret;

            float4a v(u0, u1, u2, 0);
            __m128 vV = _mm_load_ps(reinterpret_cast<float*>(&v));
            __m128 vHalf = _mm_set1_ps(0.5f);
            // [-1, 1] -> [0, 1]
            vV = _mm_fmadd_ps(vV, vHalf, vHalf);

            __m128 vMax = _mm_set1_ps((1 << 16) - 1);
            __m128 vTemp = _mm_mul_ps(vV, vMax);
            // cvtps_epi32 uses the default rounding mode (round to nearest)
            __m128i vEncoded = _mm_cvtps_epi32(vTemp);

            // Doesn't violate strict aliasing: https://stackoverflow.com/questions/13257166/print-a-m128i-variable
            alignas(16) uint32_t a[4];
            _mm_store_si128(reinterpret_cast<__m128i*>(a), vEncoded);

            ret.x = static_cast<uint16_t>(a[0]);
            ret.y = static_cast<uint16_t>(a[1]);
            ret.z = static_cast<uint16_t>(a[2]);

            return ret;
        }

        // Encodes float3 in [-1, 1]
        static unorm3 FromNormalized(float3 v)
        {
            unorm3 ret;

            __m128 vXY = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&v)));
            // &v.z does not need to be aligned
            __m128 vZ = _mm_load_ss(&v.z);
            __m128 vV = _mm_insert_ps(vXY, vZ, 0x20);
            __m128 vHalf = _mm_set1_ps(0.5f);
            // [-1, 1] -> [0, 1]
            vV = _mm_fmadd_ps(vV, vHalf, vHalf);

            __m128 vMax = _mm_set1_ps((1 << 16) - 1);
            __m128 vTemp = _mm_mul_ps(vV, vMax);
            // cvtps_epi32 uses the default rounding mode (round to nearest)
            __m128i vEncoded = _mm_cvtps_epi32(vTemp);

            // Doesn't violate strict aliasing: https://stackoverflow.com/questions/13257166/print-a-m128i-variable
            alignas(16) uint32_t a[4];
            _mm_store_si128(reinterpret_cast<__m128i*>(a), vEncoded);

            ret.x = static_cast<uint16_t>(a[0]);
            ret.y = static_cast<uint16_t>(a[1]);
            ret.z = static_cast<uint16_t>(a[2]);

            return ret;
        }

        uint16_t x;
        uint16_t y;
        uint16_t z;
    };

    struct unorm4
    {
        unorm4() = default;
        explicit unorm4(uint16_t u)
            : x(u),
            y(u),
            z(u),
            w(u)
        {}
        unorm4(uint16_t u0, uint16_t u1, uint16_t u2, uint16_t u3)
            : x(u0),
            y(u1),
            z(u2),
            w(u3)
        {}

        // Encodes float4 in [-1, 1]
        static unorm4 FromNormalized(float4a& v)
        {
            unorm4 ret;
            
            __m128 vV = _mm_load_ps(reinterpret_cast<float*>(&v));
            __m128 vHalf = _mm_set1_ps(0.5f);
            // [-1, 1] -> [0, 1]
            vV = _mm_fmadd_ps(vV, vHalf, vHalf);

            __m128 vMax = _mm_set1_ps((1 << 16) - 1);
            __m128 vTemp = _mm_mul_ps(vV, vMax);
            // cvtps_epi32 uses the default rounding mode (round to nearest)
            __m128i vEncoded = _mm_cvtps_epi32(vTemp);

            // Doesn't violate strict aliasing: https://stackoverflow.com/questions/13257166/print-a-m128i-variable
            alignas(16) uint32_t a[4];
            _mm_store_si128(reinterpret_cast<__m128i*>(a), vEncoded);

            ret.x = static_cast<uint16_t>(a[0]);
            ret.y = static_cast<uint16_t>(a[1]);
            ret.z = static_cast<uint16_t>(a[2]);
            ret.w = static_cast<uint16_t>(a[3]);

            return ret;
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

    inline float4::float4(const float4a& f)
        : x(f.x),
        y(f.y),
        z(f.z),
        w(f.w)
    {}

    //--------------------------------------------------------------------------------------
    // Operator Overloading
    //--------------------------------------------------------------------------------------

    ZetaInline constexpr float2 operator+(const float2& v1, const float2& v0)
    {
        return float2(v1.x + v0.x, v1.y + v0.y);
    }

    ZetaInline constexpr float2 operator+(const float2& v0, float f)
    {
        return float2(v0.x + f, v0.y + f);
    }
    
    ZetaInline constexpr float2 operator+(float f, const float2& v0)
    {
        return float2(v0.x + f, v0.y + f);
    }

    ZetaInline constexpr float2 operator*(const float2& v0, const float2& v1)
    {
        return float2(v0.x * v1.x, v0.y * v1.y);
    }

    ZetaInline constexpr float2 operator*(const float2& v0, float f)
    {
        return float2(v0.x * f, v0.y * f);
    }

    ZetaInline constexpr float2 operator*(float f, const float2& v0)
    {
        return float2(v0.x * f, v0.y * f);
    }

    ZetaInline constexpr float2 operator/(const float2& v0, const float2& v1)
    {
        return float2(v0.x / v1.x, v0.y / v1.y);
    }

    ZetaInline constexpr float2 operator/(const float2& v0, float f)
    {
        return float2(v0.x / f, v0.y / f);
    }

    ZetaInline constexpr float2 operator/(float f, const float2& v0)
    {
        return float2(f / v0.x, f / v0.y);
    }

    ZetaInline constexpr float2 operator-(const float2& v1, const float2& v0)
    {
        return float2(v1.x - v0.x, v1.y - v0.y);
    }

    ZetaInline constexpr float2 operator-(const float2& v0, float f)
    {
        return float2(v0.x - f, v0.y - f);
    }

    ZetaInline constexpr float2 operator-(float f, const float2& v0)
    {
        return float2(f - v0.x, f - v0.y);
    }

    ZetaInline constexpr float2 operator-(const float2& v0)
    {
        return float2(-v0.x, -v0.y);
    }

    ZetaInline constexpr float3 operator+(const float3& v0, const float3& v1)
    {
        return float3(v0.x + v1.x, v0.y + v1.y, v0.z + v1.z);
    }

    ZetaInline constexpr float3 operator+(const float3& v0, float f)
    {
        return float3(v0.x + f, v0.y + f, v0.z + f);
    }

    ZetaInline constexpr float3 operator+(float f, const float3& v0)
    {
        return float3(v0.x + f, v0.y + f, v0.z + f);
    }

    ZetaInline constexpr float3 operator-(const float3& v1, const float3& v0)
    {
        return float3(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z);
    }

    ZetaInline constexpr float3 operator-(const float3& v0, float f)
    {
        return float3(v0.x - f, v0.y - f, v0.z - f);
    }

    ZetaInline constexpr float3 operator-(float f, const float3& v0)
    {
        return float3(f - v0.x, f - v0.y, f - v0.z);
    }

    ZetaInline constexpr float3 operator-(const float3& v0)
    {
        return float3(-v0.x, -v0.y, -v0.z);
    }

    ZetaInline constexpr float3 operator*(const float3& v0, const float3& v1)
    {
        return float3(v0.x * v1.x, v0.y * v1.y, v0.z * v1.z);
    }

    ZetaInline constexpr float3 operator*(const float3& v0, float f)
    {
        return float3(v0.x * f, v0.y * f, v0.z * f);
    }

    ZetaInline constexpr float3 operator*(float f, const float3& v0)
    {
        return float3(v0.x * f, v0.y * f, v0.z * f);
    }

    ZetaInline constexpr float3 operator/(const float3& v0, const float3& v1)
    {
        return float3(v0.x / v1.x, v0.y / v1.y, v0.z / v1.z);
    }

    ZetaInline constexpr float3 operator/(const float3& v0, float f)
    {
        return float3(v0.x / f, v0.y / f, v0.z / f);
    }

    ZetaInline constexpr float3 operator/(float f, const float3& v0)
    {
        return float3(f / v0.x, f / v0.y, f / v0.z);
    }

    ZetaInline constexpr float4 operator+(const float4& v0, const float4& v1)
    {
        return float4(v0.x + v1.x, v0.y + v1.y, v0.z + v1.z, v0.w + v1.w);
    }

    ZetaInline constexpr float4 operator+(const float4& v0, float f)
    {
        return float4(v0.x + f, v0.y + f, v0.z + f, v0.w + f);
    }

    ZetaInline constexpr float4 operator+(float f, const float4& v0)
    {
        return float4(v0.x + f, v0.y + f, v0.z + f, v0.w + f);
    }

    ZetaInline constexpr float4 operator-(const float4& v1, const float4& v0)
    {
        return float4(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z, v1.w - v0.w);
    }

    ZetaInline constexpr float4 operator-(const float4& v0, float f)
    {
        return float4(v0.x - f, v0.y - f, v0.z - f, v0.w - f);
    }

    ZetaInline constexpr float4 operator-(float f, const float4& v0)
    {
        return float4(f - v0.x, f - v0.y, f - v0.z, f - v0.w);
    }

    ZetaInline constexpr float4 operator-(const float4& v0)
    {
        return float4(-v0.x, -v0.y, -v0.z, -v0.w);
    }

    ZetaInline constexpr float4 operator*(const float4& v0, const float4& v1)
    {
        return float4(v0.x * v1.x, v0.y * v1.y, v0.z * v1.z, v0.w * v1.w);
    }

    ZetaInline constexpr float4 operator*(const float4& v0, float f)
    {
        return float4(v0.x * f, v0.y * f, v0.z * f, v0.w * f);
    }

    ZetaInline constexpr float4 operator*(float f, const float4& v0)
    {
        return float4(v0.x * f, v0.y * f, v0.z * f, v0.w * f);
    }

    ZetaInline constexpr float4 operator/(const float4& v0, const float4& v1)
    {
        return float4(v0.x / v1.x, v0.y / v1.y, v0.z / v1.z, v0.w / v1.w);
    }

    ZetaInline constexpr float4 operator/(const float4& v0, float f)
    {
        return float4(v0.x / f, v0.y / f, v0.z / f, v0.w / f);
    }

    ZetaInline constexpr float4 operator/(float f, const float4& v0)
    {
        return float4(f / v0.x, f / v0.y, f / v0.z, f / v0.w);
    }

    ZetaInline constexpr uint3 operator+(uint3 v, uint32_t m)
    {
        return uint3(v.x + m, v.y + m, v.z + m);
    }

    ZetaInline constexpr uint3 operator*(uint3 v, uint32_t m)
    {
        return uint3(v.x * m, v.y * m, v.z * m);
    }

    ZetaInline constexpr uint3 operator>>(uint3 v, uint32_t m)
    {
        return uint3(v.x >> m, v.y >> m, v.z >> m);
    }

    ZetaInline constexpr uint3 operator^(uint3 v, uint3 m)
    {
        return uint3(v.x ^ m.x, v.y ^ m.y, v.z ^ m.z);
    }
}