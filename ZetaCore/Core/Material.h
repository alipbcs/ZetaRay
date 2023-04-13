#ifndef MATERIAL_H
#define MATERIAL_H

#include "HLSLCompat.h"

#ifdef __cplusplus
namespace ZetaRay
{
#endif
    struct Material
    {
#ifdef __cplusplus
        
        // "MASK": Output is either fully opaque or fully transparent depending on the alpha value and alpha cutoff value
        // "BLEND" The alpha value is used to composite the source and destination areas
        enum class ALPHA_MODE : uint8_t
        {
            // OPAQUE is defined in wingdi.h!
            OPAQUE = 0,
            MASK,
            BLEND,
            COUNT
        };
        
        Material()
            : MetallicFactor(1.0f),
            RoughnessFactor(1.0f),
            NormalScale(1.0f),
            AlphaCuttoff(0.5f),
            BaseColorTexture(uint32_t(-1)),
            MetalnessRoughnessTexture(uint32_t(-1)),
            NormalTexture(uint32_t(-1)),
            EmissiveTexture(uint32_t(-1)),
            Packed(0),
            BaseColorFactor(1.0f, 1.0f, 1.0f, 1.0f),
            EmissiveFactor(0.0f, 0.0f, 0.0f)
        {
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

        void SetDoubleSided(bool b)
        {
            Packed |= (uint32_t)(b) << 30;
        }

        uint32_t GpuBufferIndex() const
        {
            return Packed & 0x0fffffff;
        }

#endif // __cplusplus
        bool IsDoubleSided() CONST
        {
            return Packed & (1 << 30);
        }

        half4_ BaseColorFactor;
        half3_ EmissiveFactor;
        half_ MetallicFactor;
        half_ RoughnessFactor;
        half_ NormalScale;
        half_ AlphaCuttoff;
        uint32_t BaseColorTexture;
        uint32_t NormalTexture;
        uint32_t MetalnessRoughnessTexture;
        uint32_t EmissiveTexture;

        // last 4 bits encode alpha and double-sided
        // first 28 bits encode material buffer index
        uint32_t Packed;
    };
#ifdef __cplusplus
}
#endif

#ifndef __cplusplus
#define BASE_COLOR_MAP Texture2D<half4>
#define NORMAL_MAP Texture2D<float2>
#define METALNESS_ROUGHNESS_MAP Texture2D<half2>
#define EMISSIVE_MAP Texture2D<half3>
#endif

#endif // MATERIAL_H