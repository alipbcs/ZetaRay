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
    TRANSMISSION,
    DEPTH
};

#define GBUFFER_BASE_COLOR Texture2D<float4>
#define GBUFFER_NORMAL Texture2D<float2>
#define GBUFFER_METALLIC_ROUGHNESS Texture2D<float2>
#define GBUFFER_MOTION_VECTOR Texture2D<float2> 
#define GBUFFER_EMISSIVE_COLOR Texture2D<float3>
#define GBUFFER_TRANSMISSION Texture2D<float2>
#define GBUFFER_DEPTH Texture2D<float> 

namespace GBuffer
{
    float EncodeMetallic(float metalness, bool isTransmissive, float3 emissive)
    {
        bool isMetal = metalness >= MIN_METALNESS_METAL;
        bool isEmissive = dot(1, emissive) != 0;

        uint ret = isTransmissive;
        ret |= uint(isEmissive) << 1;
        ret |= uint(isMetal) << 7;

        return float(ret) / 255.0f;
    }

    void DecodeMetallic(float encoded, out bool isMetallic, out bool isTransmissive, 
        out bool isEmissive)
    {
        uint v = (uint) round(encoded * 255.0f);

        isTransmissive = (v & 0x1) != 0;
        isEmissive = (v & (1 << 1)) != 0;
        isMetallic = (v & (1 << 7)) != 0;
    }

    void DecodeMetallic(float encoded, out bool isMetallic, out bool isTransmissive, 
        out bool isEmissive, out bool invalid)
    {
        uint v = (uint) round(encoded * 255.0f);

        isTransmissive = (v & 0x1) != 0;
        isEmissive = (v & (1 << 1)) != 0;
        invalid = (v & (1 << 2)) != 0;
        isMetallic = (v & (1 << 7)) != 0;
    }

    void DecodeMetallicTr(float encoded, out bool isMetallic, out bool isTransmissive)
    {
        uint v = (uint) round(encoded * 255.0f);

        isTransmissive = (v & 0x1) != 0;
        isMetallic = (v & (1 << 7)) != 0;
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

    void DecodeEmissiveInvalid(float encoded, out bool isEmissive, out bool isInvalid)
    {
        uint v = (uint) round(encoded * 255.0f);
        isEmissive = (v & (1 << 1)) != 0;
        isInvalid = (v & (1 << 2)) != 0;
    }

    bool DecodeMetallic(float encoded)
    {
        uint v = (uint) round(encoded * 255.0f);
        return (v & (1 << 7)) != 0;
    }

    bool DecodeIsEmissive(float encoded)
    {
        uint v = (uint) round(encoded * 255.0f);
        return (v & (1 << 1)) != 0;
    }

    void DecodeMetallicEmissive(float encoded, out bool isMetallic, out bool isEmissive)
    {
        uint v = (uint) round(encoded * 255.0f);
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
}

#endif