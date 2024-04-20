#ifndef MATERIAL_H
#define MATERIAL_H

#include "HLSLCompat.h"

// Metallic factor shoud be binary, treat everything with a lower "metalness" value as dielectric.
#define MIN_METALNESS_METAL 0.92

#define MIN_IOR 1.0f
#define MAX_IOR 2.5f
#define DEFAULT_ETA_I 1.5f
#define DEFAULT_ETA_T 1.0f
#define DEFAULT_SPECULAR_TRANSMISSION 0.0f

#ifdef __cplusplus
#include "../Math/Color.h"

namespace ZetaRay
{
#endif
    struct Material
    {
#ifdef __cplusplus

        enum class ALPHA_MODE : uint8_t
        {
            // "OPAQUE" is defined in wingdi.h!
            OPAQUE_ = 0,
            // Output is either fully opaque or fully transparent depending on the alpha value
            MASK,
            // The alpha value is used to composite the source and destination areas
            BLEND,
            COUNT
        };

        Material()
            : MetallicFactorAlphaCuttoff(Math::Float2ToRG8(Math::float2(1.0f, 0.5f))),
            RoughnessFactor((1 << 16) - 1),
            BaseColorTexture(uint32_t(-1)),
            MetallicRoughnessTexture_Transmission(0xffff),
            NormalTexture_IOR(uint32_t(-1)),
            EmissiveTexture_Strength(uint32_t(-1)),
            Packed(0),
            BaseColorFactor(Math::Float4ToRGBA8(Math::float4(1.0f, 1.0f, 1.0f, 1.0f))),
            EmissiveFactorNormalScale(Math::Float4ToRGBA8(Math::float4(0.0f, 0.0f, 0.0f, 1.0f)))
        {
            SetEmissiveStrength(1.0f);
            SetIOR(1.5f);
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

        float GetMetallic() CONST
        {
            return (MetallicFactorAlphaCuttoff & 0xff) / 255.0f;
        }

        uint16_t GetEmissiveTex() CONST
        {
            return uint16_t(EmissiveTexture_Strength & 0xffff);
        }

        uint16_t GetNormalTex() CONST
        {
            return uint16_t(NormalTexture_IOR & 0xffff);
        }

        uint16_t GetMetallicRoughnessTex() CONST
        {
            return uint16_t(MetallicRoughnessTexture_Transmission & 0xffff);
        }

        float GetRoughnessFactor() CONST
        {
            return RoughnessFactor / float((1 << 16) - 1);
        }

#ifdef __cplusplus
        void SetNormalTex(uint32_t idx)
        {
            Assert(idx == uint32_t(-1) || idx < UINT16_MAX, "Invalid texture index.");
            NormalTexture_IOR = (idx & 0xffff) | (NormalTexture_IOR & 0xffff0000);
        }

        void SetMetallicRoughnessTex(uint32_t idx)
        {
            Assert(idx == uint32_t(-1) || idx < UINT16_MAX, "Invalid texture index.");
            MetallicRoughnessTexture_Transmission = (idx & 0xffff) | (MetallicRoughnessTexture_Transmission & 0xff0000);
        }

        void SetEmissiveTex(uint32_t idx)
        {
            Assert(idx == uint32_t(-1) || idx < UINT16_MAX, "Invalid texture index.");
            EmissiveTexture_Strength = (idx & 0xffff) | (EmissiveTexture_Strength & 0xffff0000);
        }

        void SetEmissiveStrength(float s)
        {
            Math::half h(s);
            EmissiveTexture_Strength = (uint32_t(h.x) << 16) | (EmissiveTexture_Strength & 0xffff);
        }

        void SetIOR(float ior)
        {
            Assert(ior >= MIN_IOR && ior < MAX_IOR, "IOR is assumed to be in the range [1, 2.5)");
            float normalized = (ior - 1.0f) / 1.5f;
            normalized = roundf(normalized * ((1 << 16) - 1));
            NormalTexture_IOR = (uint32_t(normalized) << 16) | (NormalTexture_IOR & 0xffff);
        }

        void SetTransmission(float t)
        {
            uint32_t encoded = uint32_t(roundf(t * float((1 << 8) - 1)));
            MetallicRoughnessTexture_Transmission = (encoded << 16) | (MetallicRoughnessTexture_Transmission & 0xffff);
        }

#else
        half GetEmissiveStrength()
        {
            return asfloat16(uint16_t(EmissiveTexture_Strength >> 16));
        }

        float GetIOR()
        {
            uint16_t encoded = uint16_t(NormalTexture_IOR >> 16);
            return mad(1.5f / float(((1 << 16) - 1)), encoded, 1.0f);
        }

        float GetTransmission()
        {
            float t = float((MetallicRoughnessTexture_Transmission >> 16) & 0xff);
            return t / 255.0f;
        }

#endif // __cplusplus

        uint32_t BaseColorFactor;
        uint32_t EmissiveFactorNormalScale;
        uint32_t BaseColorTexture;
        uint32_t NormalTexture_IOR;
        uint32_t MetallicRoughnessTexture_Transmission;
        uint32_t EmissiveTexture_Strength;

        // Last 4 bits encode alpha and double sided, first 20 bits encode 
        // material buffer index.
        uint32_t Packed;

        uint16_t MetallicFactorAlphaCuttoff;
        uint16_t RoughnessFactor;
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

#endif // MATERIAL_H