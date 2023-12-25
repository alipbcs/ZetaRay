#pragma once

#include "VectorFuncs.h"

namespace ZetaRay::Math
{
    // 3D unit vector encoded using octahedral encoding using two 16-bit SNORMs.
    struct oct16
    {
        constexpr oct16() = default;

        explicit oct16(float3 u)
        {
            __m128 vU = loadFloat3(u);
            vU = encode_octahedral(vU);

            v = snorm2(vU);
        }

        oct16(float x, float y, float z)
        {
            float4a f(x, y, z, 0);
            __m128 vU = load(f);
            vU = encode_octahedral(vU);

            v = snorm2(vU);
        }

        float3 decode()
        {
            __m128 vV = loadSNorm2(v);
            vV = decode_octahedral(vV);

            return storeFloat3(vV);
        }

        snorm2 v;
    };
}