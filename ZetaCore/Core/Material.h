#ifndef MATERIAL_H
#define MATERIAL_H

#include "HLSLCompat.h"

#ifdef __cplusplus
#include "../Math/Color.h"

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
            // "OPAQUE" is defined in wingdi.h!
            OPAQUE_ = 0,
            MASK,
            BLEND,
            COUNT
        };
        
        Material() noexcept
            : MetallicFactorAlphaCuttoff(Math::Float2ToRG8(Math::float2(1.0f, 0.5f))),
            RoughnessFactor(1.0f),
            BaseColorTexture(uint32_t(-1)),
            MetalnessRoughnessTexture(uint32_t(-1)),
            NormalTexture(uint32_t(-1)),
            EmissiveTexture_Strength(uint32_t(-1)),
            Packed(0),
            BaseColorFactor(Math::Float4ToRGBA8(Math::float4(1.0f, 1.0f, 1.0f, 1.0f))),
            EmissiveFactorNormalScale(Math::Float4ToRGBA8(Math::float4(0.0f, 0.0f, 0.0f, 1.0f)))
        {
            SetEmissiveStrength(1.0f);
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
#endif
        bool IsDoubleSided() CONST
        {
            return Packed & (1 << 30);
        }

        float GetAlphaCuttoff() CONST
        {
            return ((MetallicFactorAlphaCuttoff >> 8) & 0xff) / 255.0f;
        }

        float GetNormalScale() CONST
        {
            return ((EmissiveFactorNormalScale >> 24) & 0xff) / 255.0f;
        }

        float GetMetalness() CONST
        {
            return (MetallicFactorAlphaCuttoff & 0xff) / 255.0f;
        }

        uint16_t GetEmissiveTex() CONST
        {
            return uint16_t(EmissiveTexture_Strength & 0xffff);
        }

#ifdef __cplusplus
        void SetEmissiveTex(uint32_t idx)
        {
            Assert(idx == -1 || idx < UINT16_MAX, "Invalid emissive index.");
            EmissiveTexture_Strength = (idx & 0xffff) | (EmissiveTexture_Strength & 0xffff0000);
        }

        void SetEmissiveStrength(float s)
        {
            Math::half h(s);
            EmissiveTexture_Strength = (uint32_t(h.x) << 16) | (EmissiveTexture_Strength & 0xffff);
        }

#else
        half GetEmissiveStrength()
        {
            return asfloat16(uint16_t(EmissiveTexture_Strength >> 16));
        }
#endif // __cplusplus

        uint32_t BaseColorFactor;
        uint32_t EmissiveFactorNormalScale;
        uint32_t BaseColorTexture;
        uint32_t NormalTexture;
        uint32_t MetalnessRoughnessTexture;
        uint32_t EmissiveTexture_Strength;

        // last 4 bits encode alpha and double sided
        // first 28 bits encode material buffer index
        uint32_t Packed;

        uint16_t MetallicFactorAlphaCuttoff;
        half_ RoughnessFactor;
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