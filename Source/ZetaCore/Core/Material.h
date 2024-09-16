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
#define DEFAULT_ETA_MAT 1.5f
#define DEFAULT_ETA_COAT 1.6f
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
            METALLIC = 24,
            DOUBLE_SIDED = 25,
            TRANSMISSIVE = 26,
            ALPHA_1 = 27,
            ALPHA_2 = 28,
            THIN_WALLED = 29
        };

        static const uint32_t NUM_MATERIAL_BITS = 16;
        static const uint32_t NUM_TEXTURE_BITS = NUM_MATERIAL_BITS;
        // Reserve largest value for invalid materials
        static const uint32_t INVALID_ID = (1u << NUM_MATERIAL_BITS) - 1;
        static const uint32_t MAX_NUM_MATERIALS = (1u << NUM_MATERIAL_BITS) - 1;
        static const uint32_t MAX_NUM_TEXTURES = MAX_NUM_MATERIALS;
        static const uint32_t MATERIAL_MASK = (1u << NUM_MATERIAL_BITS) - 1;
        static const uint32_t TEXTURE_MASK = (1u << NUM_MATERIAL_BITS) - 1;
        static const uint32_t LOWER_16_BITS_MASK = 0xffff;
        static const uint32_t LOWER_24_BITS_MASK = 0xffffff;
        static const uint32_t UPPER_8_BITS_MASK = 0xff000000;
        static const uint32_t UPPER_16_BITS_MASK = 0xffff0000;
        // Exlucdes bits [16-24)
        static const uint32_t ONES_COMP_BITS_16_24 = 0xff00ffff;

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
            SetMetallic(0.0f);
            SetSpecularRoughness(0.3f);
            SetSpecularIOR(1.5f);
            SetTransmission(0.0f);
            SetEmissiveFactor(Math::float3(0));
            SetEmissiveStrength(1.0f);
            SetCoatWeight(0.0f);
            SetCoatColor(Math::float3(0.8f));
            SetCoatRoughness(0.0f);
            SetCoatIOR(DEFAULT_ETA_COAT);
            SetNormalScale(1.0f);
            SetAlphaMode(ALPHA_MODE::OPAQUE_);
            SetAlphaCutoff(0.5f);
            SetDoubleSided(false);
            SetThinWalled(false);
            SetBaseColorTex(INVALID_ID);
            SetNormalTex(INVALID_ID);
            SetMetallicRoughnessTex(INVALID_ID);
            SetEmissiveTex(INVALID_ID);
        }

        // For Set*Tex() calls, passing INVALID_ID (= MAX_NUM_TEXTURES) is valid
        ZetaInline void SetBaseColorTex(uint32_t idx)
        {
            Assert(idx <= MAX_NUM_TEXTURES, "Invalid texture index.");
            BaseColorTex_Subsurf_CoatWeight = idx |
                (BaseColorTex_Subsurf_CoatWeight & UPPER_16_BITS_MASK);
        }

        ZetaInline void SetNormalTex(uint32_t idx)
        {
            Assert(idx <= MAX_NUM_TEXTURES, "Invalid texture index.");
            NormalTex_TrDepth = idx | 
                (NormalTex_TrDepth & UPPER_16_BITS_MASK);
        }

        ZetaInline void SetMetallicRoughnessTex(uint32_t idx)
        {
            Assert(idx <= MAX_NUM_TEXTURES, "Invalid texture index.");
            MRTex_SpecRoughness_CoatRoughness = idx | 
                (MRTex_SpecRoughness_CoatRoughness & UPPER_16_BITS_MASK);
        }

        ZetaInline void SetEmissiveTex(uint32_t idx)
        {
            Assert(idx <= MAX_NUM_TEXTURES, "Invalid texture index.");
            EmissiveTex_AlphaCutoff_CoatIOR = idx |
                (EmissiveTex_AlphaCutoff_CoatIOR & UPPER_16_BITS_MASK);
        }

        ZetaInline void SetBaseColorFactor(const Math::float3& color)
        {
            BaseColorFactor = Float3ToRGB8(color) | (BaseColorFactor & UPPER_8_BITS_MASK);
        }

        ZetaInline void SetBaseColorFactor(const Math::float4& color)
        {
            BaseColorFactor = Float4ToRGBA8(color);
        }

        ZetaInline void SetSpecularRoughness(float r)
        {
            MRTex_SpecRoughness_CoatRoughness = 
                (MRTex_SpecRoughness_CoatRoughness & ONES_COMP_BITS_16_24) |
                (Math::FloatToUNorm8(r) << NUM_TEXTURE_BITS);
        }

        ZetaInline void SetCoatRoughness(float r)
        {
            MRTex_SpecRoughness_CoatRoughness = 
                (MRTex_SpecRoughness_CoatRoughness & LOWER_24_BITS_MASK) |
                (Math::FloatToUNorm8(r) << (NUM_TEXTURE_BITS + 8));
        }

        ZetaInline void SetAlphaCutoff(float c)
        {
            EmissiveTex_AlphaCutoff_CoatIOR = 
                (EmissiveTex_AlphaCutoff_CoatIOR & ONES_COMP_BITS_16_24) |
                (Math::FloatToUNorm8(c) << NUM_TEXTURE_BITS);
        }

        ZetaInline void SetCoatIOR(float ior)
        {
            Assert(ior >= MIN_IOR && ior < MAX_IOR, "IOR is assumed to be in the range [1, 2.5).");
            float normalized = (ior - MIN_IOR) / (MAX_IOR - MIN_IOR);

            EmissiveTex_AlphaCutoff_CoatIOR =
                (EmissiveTex_AlphaCutoff_CoatIOR & LOWER_24_BITS_MASK) |
                (Math::FloatToUNorm8(normalized) << (NUM_TEXTURE_BITS + 8));
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

        ZetaInline void SetSpecularIOR(float ior)
        {
            Assert(ior >= MIN_IOR && ior < MAX_IOR, "IOR is assumed to be in the range [1, 2.5).");
            float normalized = (ior - MIN_IOR) / (MAX_IOR - MIN_IOR);

            EmissiveStrength_IOR = (EmissiveStrength_IOR & LOWER_16_BITS_MASK) | 
                (Math::FloatToUNorm16(normalized) << 16);
        }

        ZetaInline void SetSubsurface(float s)
        {
            BaseColorTex_Subsurf_CoatWeight = 
                (BaseColorTex_Subsurf_CoatWeight & ONES_COMP_BITS_16_24) |
                (Math::FloatToUNorm8(s) << NUM_TEXTURE_BITS);
        }

        ZetaInline void SetCoatWeight(float w)
        {
            BaseColorTex_Subsurf_CoatWeight =
                (BaseColorTex_Subsurf_CoatWeight & LOWER_24_BITS_MASK) |
                (Math::FloatToUNorm8(w) << (NUM_TEXTURE_BITS + 8));
        }

        ZetaInline void SetTransmissionDepth(float depth)
        {
            Math::half dh(depth);
            NormalTex_TrDepth = (NormalTex_TrDepth & TEXTURE_MASK) |
                (uint32(dh.x) << NUM_TEXTURE_BITS);
        }

        ZetaInline void SetCoatColor(const Math::float3& color)
        {
            CoatColor_Flags = Float3ToRGB8(color) | (CoatColor_Flags & UPPER_8_BITS_MASK);
        }

        ZetaInline void SetAlphaMode(ALPHA_MODE mode)
        {
            // Clear two alpha bits
            CoatColor_Flags &= ~(0x3 << FLAG_BITS::ALPHA_1);
            CoatColor_Flags |= (uint32_t)(mode) << FLAG_BITS::ALPHA_1;
        }

        ZetaInline void SetDoubleSided(bool b)
        {
            if (b)
                CoatColor_Flags |= (1 << FLAG_BITS::DOUBLE_SIDED);
            else
                CoatColor_Flags &= ~(1 << FLAG_BITS::DOUBLE_SIDED);
        }

        ZetaInline void SetTransmission(float t)
        {
            bool transmissive = t >= MIN_SPEC_TR_TRANSMISSIVE;
            if (transmissive)
                CoatColor_Flags |= (1 << FLAG_BITS::TRANSMISSIVE);
            else
                CoatColor_Flags &= ~(1 << FLAG_BITS::TRANSMISSIVE);
        }

        ZetaInline void SetMetallic(float m)
        {
            bool metallic = m >= MIN_METALNESS_METAL;
            if (metallic)
                CoatColor_Flags |= (1 << FLAG_BITS::METALLIC);
            else
                CoatColor_Flags &= ~(1 << FLAG_BITS::METALLIC);
        }

        ZetaInline void SetThinWalled(bool b)
        {
            if (b)
                CoatColor_Flags |= (1 << FLAG_BITS::THIN_WALLED);
            else
                CoatColor_Flags &= ~(1 << FLAG_BITS::THIN_WALLED);
        }

        bool Emissive() const
        {
            if (GetEmissiveTex() != INVALID_ID)
                return true;

            Math::float3 f = GetEmissiveFactor();
            return f.dot(f) > 0;
        }
#endif
        bool DoubleSided() CONST
        {
            return CoatColor_Flags & (1u << FLAG_BITS::DOUBLE_SIDED);
        }

        bool Metallic() CONST
        {
            return CoatColor_Flags & (1u << FLAG_BITS::METALLIC);
        }

        bool Transmissive() CONST
        {
            return CoatColor_Flags & (1u << FLAG_BITS::TRANSMISSIVE);
        }

        bool ThinWalled() CONST
        {
            return CoatColor_Flags & (1u << FLAG_BITS::THIN_WALLED);
        }

        float3_ GetBaseColorFactor() CONST
        {
            return Math::UnpackRGB8(BaseColorFactor);
        }

        float3_ GetCoatColor() CONST
        {
            return Math::UnpackRGB8(CoatColor_Flags);
        }

        float3_ GetEmissiveFactor() CONST
        {
            return Math::UnpackRGB8(EmissiveFactor_NormalScale);
        }

        float GetNormalScale() CONST
        {
            return Math::UNorm8ToFloat(EmissiveFactor_NormalScale >> 24);
        }

        uint32_t GetBaseColorTex() CONST
        {
            return BaseColorTex_Subsurf_CoatWeight & TEXTURE_MASK;
        }

        uint32_t GetNormalTex() CONST
        {
            return NormalTex_TrDepth & TEXTURE_MASK;
        }

        uint32_t GetMetallicRoughnessTex() CONST
        {
            return MRTex_SpecRoughness_CoatRoughness & TEXTURE_MASK;
        }

        uint32_t GetEmissiveTex() CONST
        {
            return EmissiveTex_AlphaCutoff_CoatIOR & TEXTURE_MASK;
        }

        float GetAlphaCutoff() CONST
        {
            uint32_t bits16To24 = (EmissiveTex_AlphaCutoff_CoatIOR >> NUM_MATERIAL_BITS) & 0xff;
#ifdef __cplusplus
            return Math::UNorm8ToFloat((uint8_t)bits16To24);
#else
            return Math::UNorm8ToFloat(bits16To24);
#endif
        }

        float GetCoatIOR() CONST
        {
            uint32_t bits24To32 = (EmissiveTex_AlphaCutoff_CoatIOR >> (NUM_MATERIAL_BITS + 8)) & 0xff;
#ifdef __cplusplus
            return std::fmaf(1.5f / float(((1 << 8) - 1)), (float)bits24To32, MIN_IOR);
#else
            return mad(1.5f / float(((1 << 8) - 1)), (float)bits24To32, MIN_IOR);
#endif
        }

        float GetSpecularRoughness() CONST
        {
            uint32_t bits16To24 = (MRTex_SpecRoughness_CoatRoughness >> NUM_MATERIAL_BITS) & 0xff;
#ifdef __cplusplus
            return Math::UNorm8ToFloat((uint8_t)bits16To24);
#else
            return Math::UNorm8ToFloat(bits16To24);
#endif
        }

        float GetCoatRoughness() CONST
        {
            uint32_t bits24To32 = (MRTex_SpecRoughness_CoatRoughness >> (NUM_MATERIAL_BITS + 8)) & 0xff;
#ifdef __cplusplus
            return Math::UNorm8ToFloat((uint8_t)bits24To32);
#else
            return Math::UNorm8ToFloat(bits24To32);
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

        float GetSpecularIOR() CONST
        {
            uint16_t encoded = uint16_t(EmissiveStrength_IOR >> 16);
#ifdef __cplusplus
            return std::fmaf(1.5f / float(((1 << 16) - 1)), encoded, MIN_IOR);
#else
            return mad(1.5f / float(((1 << 16) - 1)), encoded, MIN_IOR);
#endif
        }

        half_ GetTransmissionDepth()
        {
            uint16_t upper16Bits = uint16_t(NormalTex_TrDepth >> NUM_TEXTURE_BITS);
#ifdef __cplusplus
            return Math::half::asfloat16(upper16Bits);
#else
            return asfloat16(upper16Bits);
#endif
        }

        float GetSubsurface() CONST
        {
            uint32_t bits16To24 = (BaseColorTex_Subsurf_CoatWeight >> NUM_MATERIAL_BITS) & 0xff;
#ifdef __cplusplus
            return Math::UNorm8ToFloat((uint8_t)bits16To24);
#else
            return Math::UNorm8ToFloat(bits16To24);
#endif
        }

        float GetCoatWeight() CONST
        {
            uint32_t bits24To32 = (BaseColorTex_Subsurf_CoatWeight >> (NUM_MATERIAL_BITS + 8)) & 0xff;
#ifdef __cplusplus
            return Math::UNorm8ToFloat((uint8_t)bits24To32);
#else
            return Math::UNorm8ToFloat(bits24To32);
#endif
        }

        uint32_t BaseColorFactor;
        uint32_t BaseColorTex_Subsurf_CoatWeight;
        uint32_t NormalTex_TrDepth;
        // MR stands for metallic roughness
        uint32_t MRTex_SpecRoughness_CoatRoughness;
        uint32_t EmissiveFactor_NormalScale;
        uint32_t EmissiveStrength_IOR;
        uint32_t EmissiveTex_AlphaCutoff_CoatIOR;

        uint32_t CoatColor_Flags;
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