#pragma once

#include "Vector.h"
#include <math.h>

namespace ZetaRay::Math
{
    struct float4x4a;
    struct float3x3;

    //--------------------------------------------------------------------------------------
    // Matrix types
    //--------------------------------------------------------------------------------------

    struct float4x3
    {
        float4x3()
        {}
        float4x3(const float3& row0, const float3& row1, const float3& row2, const float3& row3)
        {
            m[0] = row0;
            m[1] = row1;
            m[2] = row2;
            m[3] = row3;
        }
        explicit float4x3(const float4x4a& M);

        float3 m[4];
    };

    struct float3x4
    {
        float3x4()
        {}
        float3x4(const float4& row0, const float4& row1, const float4& row2)
        {
            m[0] = row0;
            m[1] = row1;
            m[2] = row2;
        }
        explicit float3x4(const float4x3& M)
        {
            m[0] = float4(M.m[0].x, M.m[1].x, M.m[2].x, M.m[3].x);
            m[1] = float4(M.m[0].y, M.m[1].y, M.m[2].y, M.m[3].y);
            m[2] = float4(M.m[0].z, M.m[1].z, M.m[2].z, M.m[3].z);
        }
        explicit float3x4(const float4x4a& M);

        float4 m[3];
    };

    struct alignas(16) float4x4a
    {
        float4x4a()
        {}
        float4x4a(const float4& row0, const float4& row1, const float4& row2, const float4& row3)
        {
            m[0] = row0;
            m[1] = row1;
            m[2] = row2;
            m[3] = row3;
        }
        explicit float4x4a(const float* arr)
        {
            m[0] = float4a(arr[0], arr[1], arr[2], arr[3]);
            m[1] = float4a(arr[4], arr[5], arr[6], arr[7]);
            m[2] = float4a(arr[8], arr[9], arr[10], arr[11]);
            m[3] = float4a(arr[12], arr[13], arr[14], arr[15]);
        }
        explicit float4x4a(const double* arr)
        {
            m[0] = float4a((float)arr[0], (float)arr[1], (float)arr[2], (float)arr[3]);
            m[1] = float4a((float)arr[4], (float)arr[5], (float)arr[6], (float)arr[7]);
            m[2] = float4a((float)arr[8], (float)arr[9], (float)arr[10], (float)arr[11]);
            m[3] = float4a((float)arr[12], (float)arr[13], (float)arr[14], (float)arr[15]);
        }
        explicit float4x4a(const float4x3& M)
        {
            m[0] = float4a(M.m[0], 0.0f);
            m[1] = float4a(M.m[1], 0.0f);
            m[2] = float4a(M.m[2], 0.0f);
            m[3] = float4a(M.m[3], 1.0f);
        }
        explicit float4x4a(const float3x3& M);

        float4a m[4];
    };

    struct float3x3
    {
        float3x3()
        {}
        float3x3(const float3& row0, const float3& row1, const float3& row2)
        {
            m[0] = row0;
            m[1] = row1;
            m[2] = row2;
        }
        explicit float3x3(const float4x4a& M)
        {
            m[0] = float3(M.m[0].x, M.m[0].y, M.m[0].z);
            m[1] = float3(M.m[1].x, M.m[1].y, M.m[1].z);
            m[2] = float3(M.m[2].x, M.m[2].y, M.m[2].z);
        }
        explicit float3x3(const float4x3& M)
        {
            m[0] = float3(M.m[0].x, M.m[0].y, M.m[0].z);
            m[1] = float3(M.m[1].x, M.m[1].y, M.m[1].z);
            m[2] = float3(M.m[2].x, M.m[2].y, M.m[2].z);
        }

        float3 m[3];
    };

    struct v_float4x4
    {
        v_float4x4() = default;
        v_float4x4(__m128 row0, __m128 row1, __m128 row2, __m128 row3)
            : vRow{ row0, row1, row2, row3 }
        {}

        __m128 vRow[4];
    };

    inline float4x3::float4x3(const float4x4a& M)
    {
        m[0] = float3(M.m[0].x, M.m[0].y, M.m[0].z);
        m[1] = float3(M.m[1].x, M.m[1].y, M.m[1].z);
        m[2] = float3(M.m[2].x, M.m[2].y, M.m[2].z);
        m[3] = float3(M.m[3].x, M.m[3].y, M.m[3].z);
    }

    inline float3x4::float3x4(const float4x4a& M)
    {
        m[0] = float4(M.m[0].x, M.m[1].x, M.m[2].x, M.m[3].x);
        m[1] = float4(M.m[0].y, M.m[1].y, M.m[2].y, M.m[3].y);
        m[2] = float4(M.m[0].z, M.m[1].z, M.m[2].z, M.m[3].z);
    }

    inline float4x4a::float4x4a(const float3x3& M)
    {
        m[0] = float4(M.m[0].x, M.m[0].y, M.m[0].z, 0);
        m[1] = float4(M.m[1].x, M.m[1].y, M.m[1].z, 0);
        m[2] = float4(M.m[2].x, M.m[2].y, M.m[2].z, 0);
        m[3] = float4(0, 0, 0, 1);
    }

    struct AffineTransformation
    {
        static AffineTransformation GetIdentity()
        {
            AffineTransformation M;

            M.Scale = float3(1.0f, 1.0f, 1.0f);
            M.Rotation = float4(0.0f, 0.0f, 0.0f, 1.0f);
            M.Translation = float3(0.0f, 0.0f, 0.0f);

            return M;
        }

        Math::float3 Scale;
        Math::float4 Rotation;
        Math::float3 Translation;
    };
}