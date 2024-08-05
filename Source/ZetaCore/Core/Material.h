#ifndef MATERIAL_H
#define MATERIAL_H

#include "HLSLCompat.h"

// Metallic is treated as a binary parameter - everything with a lower "metalness" 
// value is considered dielectric.
#define MIN_METALNESS_METAL 0.9
// Transmission weight is also treated as binary - everything with a lower
// weight is considered opaque.
#define MIN_SPEC_TR_TRANSMISSIVE 0.9

#define MIN_IOR 1.0f
#define MAX_IOR 2.5f
#define DEFAULT_ETA_I 1.5f
#define DEFAULT_ETA_T 1.0f
#define ETA_AIR 1.0f

#ifdef __cplusplus
#include "../Math/Color.h"
#else
#include "../../ZetaRenderPass/Common/Math.hlsli"
#endif

#ifdef __cplusplus
namespace ZetaRay
{
#endif
    struct Material
    {
        enum FLAG_BITS
        {
            METALLIC = 20,
            DOUBLE_SIDED = 21,
            TRANSMISSIVE = 22,
            ALPHA_1 = 23,
            ALPHA_2 = 24
        };

        static const uint32_t NUM_MATERIAL_BITS = 20;
        static const uint32_t NUM_TEXTURE_BITS = NUM_MATERIAL_BITS;
        // Reserve largest value for invalid materials
        static const uint32_t INVALID_ID = (1 << NUM_MATERIAL_BITS) - 1;
        static const uint32_t MAX_NUM_MATERIALS = (1 << NUM_MATERIAL_BITS) - 1;
        static const uint32_t MAX_NUM_TEXTURES = MAX_NUM_MATERIALS;
        static const uint32_t MATERIAL_MASK = (1 << NUM_MATERIAL_BITS) - 1;
        static const uint32_t TEXTURE_MASK = (1 << NUM_MATERIAL_BITS) - 1;
        static const uint32_t LOWER_16_BITS_MASK = 0xffff;
        static const uint32_t LOWER_24_BITS_MASK = 0xffffff;
        static const uint32_t LOWER_28_BITS_MASK = 0xfffffff;
        static const uint32_t UPPER_8_BITS_MASK = 0xff000000;
        static const uint32_t UPPER_12_BITS_MASK = 0xfff00000;
        static const uint32_t UPPER_16_BITS_MASK = 0xffff0000;

#ifdef __cplusplus

        enum class ALPHA_MODE : uint8_t
        {
            // "OPAQUE" is defined in wingdi.h!
            OPAQUE_ = 0,
            // Output is either fully opaque or fully transparent depending on the alpha value
            MASK,
            // [Not supported] Alpha value is used to composite source and destination areas
            BLEND,
            COUNT
        };

        Material()
        {
            memset(this, 0, sizeof(Material));

            SetBaseColorFactor(Math::float4(1.0f, 1.0f, 1.0f, 1.0f));
            SetRoughnessFactor(1.0f);
            SetMetallic(1.0f);
            SetTransmission(0.0f);
            SetIOR(1.5f);
            SetEmissiveFactor(Math::float3(0));
            SetEmissiveStrength(1.0f);
            SetDoubleSided(false);
            SetAlphaMode(ALPHA_MODE::OPAQUE_);
            SetAlphaCutoff(0.5f);
            SetNormalScale(1.0f);
            SetBaseColorTex(INVALID_ID);
            SetNormalTex(INVALID_ID);
            SetMetallicRoughnessTex(INVALID_ID);
            SetEmissiveTex(INVALID_ID);
            // Should match SceneCore::DEFAULT_MATERIAL_IDX
            SetGpuBufferIndex(0);
        }

        // Passing INVALID_ID is invalid
        ZetaInline void SetGpuBufferIndex(uint32_t idx)
        {
            Assert(idx < MAX_NUM_MATERIALS, "At most MAX_NUM_MATERIALS different materials are supported.");
            Packed = (Packed & UPPER_12_BITS_MASK) | idx;
        }

        // For Set*Tex() calls, passing INVALID_ID (= MAX_NUM_TEXTURES) is valid
        ZetaInline void SetBaseColorTex(uint32_t idx)
        {
            Assert(idx <= MAX_NUM_TEXTURES, "Invalid texture index.");
            BaseColorTexture = idx;
        }

        ZetaInline void SetNormalTex(uint32_t idx)
        {
            Assert(idx <= MAX_NUM_TEXTURES, "Invalid texture index.");
            NormalTexture = idx;
        }

        ZetaInline void SetMetallicRoughnessTex(uint32_t idx)
        {
            Assert(idx <= MAX_NUM_TEXTURES, "Invalid texture index.");
            MRTexture_RoughnessFactor = idx | 
                (MRTexture_RoughnessFactor & UPPER_12_BITS_MASK);
        }

        ZetaInline void SetEmissiveTex(uint32_t idx)
        {
            Assert(idx <= MAX_NUM_TEXTURES, "Invalid texture index.");
            EmissiveTexture_AlphaCutoff = idx | 
                (EmissiveTexture_AlphaCutoff & UPPER_12_BITS_MASK);
        }

        ZetaInline void SetBaseColorFactor(Math::float3 color)
        {
            BaseColorFactor = Float3ToRGB8(color) | (BaseColorFactor & UPPER_8_BITS_MASK);
        }

        ZetaInline void SetBaseColorFactor(Math::float4 color)
        {
            BaseColorFactor = Float4ToRGBA8(color);
        }

        ZetaInline void SetRoughnessFactor(float r)
        {
            MRTexture_RoughnessFactor = (MRTexture_RoughnessFactor & TEXTURE_MASK) |
                (Math::FloatToUNorm8(r) << NUM_MATERIAL_BITS);
        }

        ZetaInline void SetAlphaCutoff(float c)
        {
            EmissiveTexture_AlphaCutoff = (EmissiveTexture_AlphaCutoff & TEXTURE_MASK) |
                (Math::FloatToUNorm8(c) << NUM_TEXTURE_BITS);
        }

        ZetaInline void SetNormalScale(float s)
        {
            EmissiveFactor_NormalScale = (EmissiveFactor_NormalScale & LOWER_24_BITS_MASK) |
                (Math::FloatToUNorm8(s) << 24);
        }

        ZetaInline void SetEmissiveFactor(Math::float3 color)
        {
            EmissiveFactor_NormalScale = Math::Float3ToRGB8(color) | 
                (EmissiveFactor_NormalScale & UPPER_8_BITS_MASK);
        }

        ZetaInline void SetEmissiveStrength(float s)
        {
            Math::half h(s);
            EmissiveStrength_IOR = uint32_t(h.x) | (EmissiveStrength_IOR & UPPER_16_BITS_MASK);
        }

        ZetaInline void SetIOR(float ior)
        {
            Assert(ior >= MIN_IOR && ior < MAX_IOR, "IOR is assumed to be in the range [1, 2.5)");

            float normalized = (ior - MIN_IOR) / (MAX_IOR - MIN_IOR);
            normalized = std::fmaf(normalized, (1 << 16) - 1, 0.5f);
            EmissiveStrength_IOR = (EmissiveStrength_IOR & LOWER_16_BITS_MASK) | 
                (uint32_t(normalized) << 16);
        }

        ZetaInline void SetAlphaMode(ALPHA_MODE mode)
        {
            // Clear two alpha bits
            Packed &= ~(0x3 << FLAG_BITS::ALPHA_1);
            Packed |= (uint32_t)(mode) << FLAG_BITS::ALPHA_1;
        }

        ZetaInline void SetDoubleSided(bool b)
        {
            if (b)
                Packed |= (1 << FLAG_BITS::DOUBLE_SIDED);
            else
                Packed &= ~(1 << FLAG_BITS::DOUBLE_SIDED);
        }

        ZetaInline void SetTransmission(float t)
        {
            bool transmissive = t >= MIN_SPEC_TR_TRANSMISSIVE;
            if (transmissive)
                Packed |= (1 << FLAG_BITS::TRANSMISSIVE);
            else
                Packed &= ~(1 << FLAG_BITS::TRANSMISSIVE);
        }

        ZetaInline void SetMetallic(float m)
        {
            bool metallic = m >= MIN_METALNESS_METAL;
            if (metallic)
                Packed |= (1 << FLAG_BITS::METALLIC);
            else
                Packed &= ~(1 << FLAG_BITS::METALLIC);
        }

        ZetaInline uint32_t GpuBufferIndex() const
        {
            return Packed & MATERIAL_MASK;
        }

        bool IsEmissive() const
        {
            if (GetEmissiveTex() != INVALID_ID)
                return true;

            Math::float3 f = GetEmissiveFactor();
            return f.dot(f) > 0;
        }
#endif
        bool IsDoubleSided() CONST
        {
            return Packed & (1 << FLAG_BITS::DOUBLE_SIDED);
        }

        bool IsMetallic() CONST
        {
            return Packed & (1 << FLAG_BITS::METALLIC);
        }

        bool IsTransmissive() CONST
        {
            return Packed & (1 << FLAG_BITS::TRANSMISSIVE);
        }

        float3_ GetBaseColorFactor() CONST
        {
            return Math::UnpackRGB8(BaseColorFactor);
        }

        float3_ GetEmissiveFactor() CONST
        {
            return Math::UnpackRGB8(EmissiveFactor_NormalScale);
        }

        float GetNormalScale() CONST
        {
            return Math::UNorm8ToFloat(EmissiveFactor_NormalScale >> 24);
        }

        uint32_t GetEmissiveTex() CONST
        {
            return EmissiveTexture_AlphaCutoff & TEXTURE_MASK;
        }

        float GetAlphaCutoff() CONST
        {
#ifdef __cplusplus
            return Math::UNorm8ToFloat((uint8_t)(EmissiveTexture_AlphaCutoff >> NUM_MATERIAL_BITS));
#else
            return Math::UNorm8ToFloat(EmissiveTexture_AlphaCutoff >> NUM_MATERIAL_BITS);
#endif
        }

        uint32_t GetMetallicRoughnessTex() CONST
        {
            return MRTexture_RoughnessFactor & TEXTURE_MASK;
        }

        float GetRoughnessFactor() CONST
        {
#ifdef __cplusplus
            return Math::UNorm8ToFloat((uint8_t)(MRTexture_RoughnessFactor >> NUM_TEXTURE_BITS));
#else
            return Math::UNorm8ToFloat(MRTexture_RoughnessFactor >> NUM_TEXTURE_BITS);
#endif
        }

        half_ GetEmissiveStrength() CONST
        {
#ifdef __cplusplus
            return Math::half::asfloat16(uint16_t(EmissiveStrength_IOR & LOWER_16_BITS_MASK));
#else
            return asfloat16(uint16_t(EmissiveStrength_IOR & LOWER_16_BITS_MASK));
#endif
        }

        float GetIOR() CONST
        {
            uint16_t encoded = uint16_t(EmissiveStrength_IOR >> 16);

#ifdef __cplusplus
            return std::fmaf(1.5f / float(((1 << 16) - 1)), encoded, MIN_IOR);
#else
            return mad(1.5f / float(((1 << 16) - 1)), encoded, MIN_IOR);
#endif
        }

        uint32_t BaseColorFactor;
        uint32_t BaseColorTexture;
        uint32_t NormalTexture;
        // MR stands for metallic roughness
        uint32_t MRTexture_RoughnessFactor;
        uint32_t EmissiveFactor_NormalScale;
        uint32_t EmissiveStrength_IOR;
        uint32_t EmissiveTexture_AlphaCutoff;

        // Last 12 bits encode various flags, first 20 bits encode 
        // material buffer index.
        uint32_t Packed;
    };
#ifdef __cplusplus
}
#endif

#ifndef __cplusplus
#define BASE_COLOR_MAP Texture2D<float4>
#define NORMAL_MAP Texture2D<float2>
#define METALLIC_ROUGHNESS_MAP Texture2D<float2>
#define EMISSIVE_MAP Texture2D<float3>
#endif

#endif