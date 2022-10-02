#ifndef MATERIAL_H
#define MATERIAL_H

#include "HLSLCompat.h"

#ifdef __cplusplus
namespace ZetaRay
{
#endif
    static const float MIN_ALPHA_CUTOFF = 0.01f;

    struct Material
    {
#ifdef __cplusplus
        
        // "MASK": Output is either fully opaque or fully transparent depending on the alpha value and alpha cutoff value
        // "BLEND" The alpha value is used to composite the source and destination areas
        enum class ALPHA_MODE : uint8_t
        {
            OPAQUE = 0,
            MASK,
            BLEND,
            COUNT
        };
        
        Material()
        {
            BaseColorFactor = float4_(1.0f, 1.0f, 1.0f, 1.0f);
            MetallicFactor = 1.0f;
            RoughnessFactor = 1.0f;
            NormalScale = 1.0f;
            AlphaCuttoff = 0.5f;
            BaseColorTexture = -1;
            MetallicRoughnessTexture = -1;
            NormalTexture = -1;
            EmissiveTexture = -1;
            Packed = 0;
        }

        void SetGpuBufferIndex(uint32_t idx)
        {
            Assert(idx < 1'000'000, "At most 1'000'000 different materials are supported.");
            Packed |= idx;
        }

        void SetAlphaMode(ALPHA_MODE mode)
        {
            Packed |= (uint32_t)(mode) << 28;
        }

        void SetTwoSided(bool isTwoSided)
        {
            Packed |= (uint32_t)(isTwoSided) << 30;
        }

        uint32_t GpuBufferIndex() const
        {
            return Packed & 0x0fffffff;
        }

#endif // __cplusplus

        float4_ BaseColorFactor;
        float3_ EmissiveFactor;
        float MetallicFactor;
        float RoughnessFactor;
        float NormalScale;
        float AlphaCuttoff;
        uint32_t BaseColorTexture;
        uint32_t NormalTexture;
        uint32_t MetallicRoughnessTexture;
        uint32_t EmissiveTexture;

        // last 4 bits encode alpha and two-sided
        // first 28 bits encode material buffer index
        uint32_t Packed;
    };
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
// Ref: Understanding Structured Buffer Performance
// https://developer.nvidia.com/content/understanding-structured-buffer-performance
static_assert((sizeof(ZetaRay::Material)& (16 - 1)) == 0);
#endif

#ifndef __cplusplus
#define BASE_COLOR_MAP Texture2D<half4>
#define NORMAL_MAP Texture2D<float2>
#define METALNESS_ROUGHNESS_MAP Texture2D<half3>
#define EMISSIVE_MAP Texture2D<half3>
#endif

#endif // MATERIAL_H