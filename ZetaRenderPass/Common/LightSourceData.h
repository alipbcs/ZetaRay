#ifndef LIGHTSOURCE_H
#define LIGHTSOURCE_H

#include "HLSLCompat.h"

#ifdef __cplusplus
namespace ZetaRay
{
#endif
    enum class LightSourceType : uint32_t
    {
        DIRECTIONAL = 0,
        POINT,
        SPOT,
        RECTANGLE,
        DISK
    };

    // Coordinate systems:
    // Dir, Point, Spot: World Space
    // Rectangle: At Origin with +Y as normal
    // Disk: Same as Rectangle
    struct AnalyticalLightSource
    {
        LightSourceType Type;

        // Translation to World position
        float3_ Translation;

        // Additionally store rotation quaternion so that normals can be tranafomed
        // without the scale (alternative would've been soring inverse tranpose of SR)
        float4_ RotQuat;

        // Scaling transformation S multiplied by rotation transformation R
//        float3x3_ SR;

        // Dir Light + Spot light
        float3_ Dir;

        // Dir Light
        // luminous power incident on an area. Unit is lux (lx)
        float Illuminance;
        float CosAngularRadius;

        // RGB color of light
        float3_ Color;

        // Total power of light source. Unit is lumen (lm), which is equal to
        // luminous energy per unit time
        float LuminousPower;

        // Spot Light
        float CosFalloffStart;
        float CosTotalWidth;

        // Disk
        float Radius;

        // Rectangle
        float2_ Dim;

        float2_ pad2;
    };

    struct EmissiveTriangle
    {
        uint32_t DescHeapIdx;
        uint32_t PrimitiveIdx;
        uint32_t EmissiveMapIdx;
        float Lumen;

        // Scaling transformation S multiplied by rotation transformation R
        float3x3_ SR;
        float3_ Translation;
    };


    struct AliasTableEntry
    {
#ifdef __cplusplus
        AliasTableEntry() noexcept
            : P(0.0f),
            Alias(-1),
            OriginalProb(0.0f)
        {}
#endif

        float P;
        uint32_t Alias;
        float OriginalProb;

        float pad;
    };


    struct EnvLightDesc
    {
        float Pdf;
        uint32_t NumPatches;
        float dPhi;
        //const float dPhi = TWO_PI / NUM_PATCHES_U;

        float pad;
    };

    struct EnvMapPatch
    {
        float Prob;     // Lumen / sigma (Lumen)

        float CosTheta1;
        float CosTheta2;

        float Phi1;
        //   half_ Phi2;
    };

#ifdef __cplusplus
    // Ref: Understanding Structured Buffer Performance
    // https://developer.nvidia.com/content/understanding-structured-buffer-performance
    static_assert((sizeof(AnalyticalLightSource) & (16 - 1)) == 0);
    static_assert((sizeof(EmissiveTriangle) & (16 - 1)) == 0);
    static_assert((sizeof(AliasTableEntry) & (16 - 1)) == 0);
    static_assert((sizeof(EnvLightDesc) & (16 - 1)) == 0);
    static_assert((sizeof(EnvMapPatch) & (16 - 1)) == 0);
}
#endif

#endif
