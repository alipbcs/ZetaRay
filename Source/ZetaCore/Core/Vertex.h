#pragma once

#include "../Math/OctahedralVector.h"

namespace ZetaRay::Core
{
    // 28 bytes
    struct Vertex
    {
        Math::float3 Position;
        Math::float2 TexUV;
        Math::oct16 Normal;
        Math::oct16 Tangent;
    };
}