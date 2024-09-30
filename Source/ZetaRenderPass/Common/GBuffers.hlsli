#ifndef GBUFFERS_H
#define GBUFFERS_H

#include "../../ZetaCore/Core/Material.h"

enum GBUFFER_OFFSET
{
    BASE_COLOR = 0,
    NORMAL,
    METALLIC_ROUGHNESS,
    MOTION_VECTOR,
    EMISSIVE_COLOR,
    IOR,
    COAT,
    DEPTH,
    TRI_DIFF_GEO_A,
    TRI_DIFF_GEO_B
};

#define GBUFFER_BASE_COLOR Texture2D<float4>
#define GBUFFER_NORMAL Texture2D<float2>
#define GBUFFER_METALLIC_ROUGHNESS Texture2D<float2>
#define GBUFFER_MOTION_VECTOR Texture2D<float2> 
#define GBUFFER_EMISSIVE_COLOR Texture2D<float3>
#define GBUFFER_IOR Texture2D<float>
#define GBUFFER_COAT Texture2D<uint4>
#define GBUFFER_DEPTH Texture2D<float> 
#define GBUFFER_TRI_DIFF_GEO_A Texture2D<uint4> 
#define GBUFFER_TRI_DIFF_GEO_B Texture2D<uint2> 

namespace GBuffer
{
    struct Flags
    {
        bool metallic;
        bool transmissive;
        bool emissive;
        bool invalid;
        bool trDepthGt0;
        bool subsurface;
        bool coated;
    };

    struct Coat
    {
        float weight;
        float3 color;
        float roughness;
        float ior;
    };

    float EncodeMetallic(float metalness, bool isTransmissive, float3 emissive, float trDepth,
        float subsurface, float coat_weight)
    {
        bool isMetal = metalness >= MIN_METALNESS_METAL;
        bool isEmissive = dot(emissive, emissive) > 0;

        uint ret = isTransmissive;
        ret |= (uint(isEmissive) << 1);
        ret |= (uint(trDepth > 0) << 3);
        ret |= (uint(subsurface > 0) << 4);
        ret |= (uint(coat_weight > 0) << 5);
        ret |= (uint(isMetal) << 7);

        return float(ret) / 255.0f;
    }

    GBuffer::Flags DecodeMetallic(float encoded)
    {
        uint v = (uint)mad(encoded, 255.0f, 0.5f);

        Flags ret;
        ret.transmissive = (v & 0x1) != 0;
        ret.emissive = (v & (1 << 1)) != 0;
        ret.invalid = (v & (1 << 2)) != 0;
        ret.trDepthGt0 = (v & (1 << 3)) != 0;
        ret.subsurface = (v & (1 << 4)) != 0;
        ret.coated = (v & (1 << 5)) != 0;
        ret.metallic = (v & (1 << 7)) != 0;

        return ret;
    }

    bool4 DecodeMetallic(float4 encoded)
    {
        uint4 v = (uint4) round(encoded * 255.0f);
        return (v & (1 << 7)) != 0;
    }

    void DecodeMetallicEmissive(float4 encoded, out bool4 isMetallic, out bool4 isEmissive)
    {
        uint4 v = (uint4) round(encoded * 255.0f);
        isEmissive = (v & (1 << 1)) != 0;
        isMetallic = (v & (1 << 7)) != 0;
    }

    float EncodeIOR(float ior)
    {
        return (ior - MIN_IOR) / (MAX_IOR - MIN_IOR);
    }

    float DecodeIOR(float encoded)
    {
        return mad(encoded, MAX_IOR - MIN_IOR, MIN_IOR);
    }

    GBuffer::Coat UnpackCoat(uint3 packed)
    {
        GBuffer::Coat ret;
        ret.weight = Math::UNorm8ToFloat((packed.y >> 8) & 0xff);
        ret.roughness = Math::UNorm8ToFloat(packed.z & 0xff);

        uint c = packed.x | ((packed.y & 0xff) << 16);
        ret.color = Math::UnpackRGB8(c);

        float normalized = Math::UNorm8ToFloat(packed.z >> 8);
        ret.ior = DecodeIOR(normalized);

        return ret;
    }
}

#endif