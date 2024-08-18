#pragma once

#include "VectorFuncs.h"

namespace ZetaRay::Math
{
    // 3D unit vector encoded using octahedral encoding with two 16-bit UNORMs
    struct oct32
    {
        constexpr oct32() = default;
        explicit oct32(float3 u)
        {
            __m128 vU = loadFloat3(u);
            vU = encode_octahedral(vU);
            v = unorm2::FromNormalized(vU);
        }

        oct32(float x, float y, float z)
        {
            float4a f(x, y, z, 0);
            __m128 vU = load(f);
            vU = encode_octahedral(vU);
            v = unorm2::FromNormalized(vU);
        }

        float3 decode()
        {
            __m128 vV = loadUNorm2(v);
            __m128 vTwo = _mm_set1_ps(2.0f);
            __m128 vMinOne = _mm_set1_ps(-1.0f);
            // [0, 1] -> [-1, 1] 
            vV = _mm_fmadd_ps(vV, vTwo, vMinOne);
            vV = decode_octahedral(vV);

            return storeFloat3(vV);
        }

        unorm2 v;
    };
}