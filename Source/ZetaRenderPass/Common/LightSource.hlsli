#ifndef LIGHT_SOURCE_H
#define LIGHT_SOURCE_H

#include "FrameConstants.h"
#include "StaticTextureSamplers.hlsli"
#include "Volumetric.hlsli"
#include "RT.hlsli"
#include "GBuffers.hlsli"
#include "BSDF.hlsli"

namespace Light
{
    enum class TYPE : uint16_t
    {
        NONE = 0,
        SUN = 1,
        SKY = 2,
        EMISSIVE = 3
    };

    uint16_t TypeToValue(TYPE t)
    {
        switch(t)
        {
            case TYPE::NONE:
                return 0;
            case TYPE::SUN:
                return 1;
            case TYPE::SKY:
                return 2;
            case TYPE::EMISSIVE:
            default:
                return 3;
        }
    }

    TYPE TypeFromValue(uint x)
    {
        if(x == 0)
            return TYPE::NONE;
        if(x == 1)
            return TYPE::SUN;
        if(x == 2)
            return TYPE::SKY;

        return TYPE::EMISSIVE;
    }

    float3 DecodeEmissiveTriV1(RT::EmissiveTriangle tri)
    {
#if ENCODE_EMISSIVE_POS == 1
        float2 v0v1 = (float2)tri.V0V1 / float((1 << 16) - 1);
        float3 decoded = Math::DecodeUnitVector(v0v1.xy);

        return mad(decoded, tri.EdgeLengths.x, tri.Vtx0);
#else
        return Vtx1;
#endif
    }

    float3 DecodeEmissiveTriV2(RT::EmissiveTriangle tri)
    {
#if ENCODE_EMISSIVE_POS == 1
        float2 v0v2 = (float2)tri.V0V2 / float((1 << 16) - 1);
        float3 decoded = Math::DecodeUnitVector(v0v2.xy);
        
        return mad(decoded, tri.EdgeLengths.y, tri.Vtx0);
#else
        return Vtx2;
#endif
    }

    struct AliasTableSample
    {
        static AliasTableSample get(StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable, 
            uint numEmissiveTriangles, inout RNG rng)
        {
            AliasTableSample ret;
            uint u0 = rng.UniformUintBounded(numEmissiveTriangles);
            RT::EmissiveLumenAliasTableEntry s = g_aliasTable[u0];

            if (rng.Uniform() < s.P_Curr)
            {
                ret.pdf = s.CachedP_Orig;
                ret.idx = u0;

                return ret;
            }

            ret.pdf = s.CachedP_Alias;
            ret.idx = s.Alias;

            return ret;
        }

        uint idx;
        float pdf;
    };

    RT::PresampledEmissiveTriangle SamplePresampledSet(uint sampleSetIdx, 
        StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets, 
        uint sampleSetSize, inout RNG rng)
    {
        uint u = rng.UniformUintBounded_Faster(sampleSetSize);
        uint idx = sampleSetIdx * sampleSetSize + u;
        return g_sampleSets[idx];
    }

    struct EmissiveTriSample
    {
        static EmissiveTriSample get(float3 pos, RT::EmissiveTriangle tri, inout RNG rng, 
            bool reverseNormalIfTwoSided = true)
        {
            EmissiveTriSample ret;
            float2 u = rng.Uniform2D();
            ret.bary = Sampling::UniformSampleTriangle(u);

            const float3 vtx1 = Light::DecodeEmissiveTriV1(tri);
            const float3 vtx2 = Light::DecodeEmissiveTriV2(tri);
            ret.pos = (1.0f - ret.bary.x - ret.bary.y) * tri.Vtx0 + ret.bary.x * vtx1 + ret.bary.y * vtx2;
            ret.normal = cross(vtx1 - tri.Vtx0, vtx2 - tri.Vtx0);
            bool normalIs0 = dot(ret.normal, ret.normal) == 0;
            float twoArea = length(ret.normal);
            ret.pdf = normalIs0 ? 0.0f : 2.0f / twoArea;

            ret.normal = normalIs0 ? ret.normal : ret.normal / twoArea;
            ret.normal = reverseNormalIfTwoSided && tri.IsDoubleSided() && dot(pos - ret.pos, ret.normal) < 0 ? 
                -ret.normal : 
                ret.normal;

            return ret;
        }

        float3 pos;
        float3 normal;
        float2 bary;
        float pdf;
    };

    float3 Le_Sun(float3 pos, ConstantBuffer<cbFrameConstants> g_frame)
    {
        const float3 sigma_t_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
        const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
        const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;

        float3 temp = pos;
        temp.y += g_frame.PlanetRadius;
    
        const float t = Volume::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, 
            temp, -g_frame.SunDir);
        const float3 tr = Volume::EstimateTransmittance(g_frame.PlanetRadius, temp, -g_frame.SunDir, t,
            sigma_t_rayleigh, sigma_t_mie, sigma_t_ozone, 6);

        return tr * g_frame.SunIlluminance;
    }

    float3 Le_Sky(float3 wi, uint skyViewDescHeapOffset, SamplerState s = g_samPointWrap)
    {
        const float2 thetaPhi = Math::SphericalFromCartesian(wi);
        float2 uv = float2(thetaPhi.y * ONE_OVER_2_PI, thetaPhi.x * ONE_OVER_PI);

        // Inverse non-linear map
        const float sn = thetaPhi.x >= PI_OVER_2 ? 1.0f : -1.0f;
        uv.y = (thetaPhi.x - PI_OVER_2) * 0.5f;
        uv.y = 0.5f + sn * sqrt(abs(uv.y) * ONE_OVER_PI);

        Texture2D<float3> g_envMap = ResourceDescriptorHeap[skyViewDescHeapOffset];
        const float3 le = g_envMap.SampleLevel(s, uv, 0.0f).rgb;

        return le;
    }

    // Assumes area light is diffuse.
    float3 Le_EmissiveTriangle(RT::EmissiveTriangle tri, float2 bary, uint emissiveMapsDescHeapOffset, 
        SamplerState s = g_samPointWrap)
    {
        const float3 emissiveFactor = tri.GetFactor();
        const float emissiveStrength = (float)tri.GetStrength();
        float3 le = emissiveFactor * emissiveStrength;

        if (Math::Luminance(le) < 1e-5)
            return 0.0;

        uint32_t emissiveTex = tri.GetTex();
        if (emissiveTex != Material::INVALID_ID)
        {
            const uint offset = NonUniformResourceIndex(emissiveMapsDescHeapOffset + emissiveTex);
            EMISSIVE_MAP g_emissiveMap = ResourceDescriptorHeap[offset];

            float2 texUV = (1.0f - bary.x - bary.y) * tri.UV0 + bary.x * tri.UV1 + bary.y * tri.UV2;
            le *= g_emissiveMap.SampleLevel(s, texUV, 0).rgb;
        }

        return le;
    }

    struct SunSample
    {
        static SunSample get(float3 sunDir, float sunCosAngularRadius, float3 normal, 
            BSDF::ShadingData surface, inout RNG rng)
        {
            SunSample ret;

            // Essentially a mirror, no point in light sampling
            if(surface.specular && surface.metallic)
            {
                float3 wi = reflect(-surface.wo, normal);
                surface.SetWi_Refl(wi, normal);
                ret.f = BSDF::Unified(surface).f;
                ret.wi = wi;
                ret.pdf = 1.0f;

                return ret;
            }

            // Light sampling - since sun is a very small area light, area sampling 
            // should be fine unless the surface is specular
            float pdf_light;
            float3 sampleLocal = Sampling::UniformSampleCone(rng.Uniform2D(), 
                sunCosAngularRadius, pdf_light);
            
            float3 T;
            float3 B;
            Math::revisedONB(sunDir, T, B);
            float3 wi_light = mad(sampleLocal.x, T, mad(sampleLocal.y, B, sampleLocal.z * sunDir));

            if(!surface.specular || (surface.ThinWalled() && (dot(wi_light, normal) < 0)))
            {
                surface.SetWi(wi_light, normal);
                ret.f = BSDF::Unified(surface).f;
                ret.wi = wi_light;
                ret.pdf = pdf_light;

                return ret;
            }

            // At this point, we know the surface is specular

            // Use the refracted direction only if light hits the backside and surface
            // is transmissive (thin walled was handled above)
            if(dot(sunDir, normal) < 0)
            {
                if(surface.specTr)
                {
                    float3 wi = refract(-surface.wo, normal, 1 / surface.eta);
                    surface.SetWi_Tr(wi, normal);
                    ret.f = BSDF::DielectricSpecularTr(surface);
                    ret.wi = wi;
                    ret.pdf = 1;

                    return ret;
                }

                // Sun hit the backside and surface isn't transmissive
                ret.f = 0;
                // Doesn't matter as f = 0
                ret.wi = sunDir;
                ret.pdf = 0;

                return ret;
            }

            // Specular dielectric and sun hits the front side

            float3 wi_refl = reflect(-surface.wo, normal);
            bool insideCone = dot(wi_refl, sunDir) >= sunCosAngularRadius;

            if(!insideCone)
            {
                surface.SetWi_Refl(wi_light, normal);
                ret.f = BSDF::Unified(surface).f;
                ret.wi = wi_light;
                ret.pdf = pdf_light;

                return ret;
            }

            // Streaming RIS -- light sample and mirror reflection are both viable

            // Feed light sample
            surface.SetWi_Refl(wi_light, normal);
            float3 target = BSDF::Unified(surface).f;
            float targetLum = Math::Luminance(target);
            // rate of change of reflection vector is twice the rate of change of half vector
            // 0.99996 = 1.0f - (1.0f - MIN_N_DOT_H_SPECULAR) * 2
            float w_sum = RT::BalanceHeuristic(pdf_light, dot(wi_light, wi_refl) >= 0.99996, targetLum);
            ret.wi = wi_light;
            ret.f = target;

            // Feed BSDF sample
            surface.SetWi_Refl(wi_refl, normal);
            float3 target_bsdf = BSDF::Unified(surface).f * insideCone;
            float targetLum_bsdf = Math::Luminance(target_bsdf);
            float w = RT::BalanceHeuristic(1, pdf_light, targetLum_bsdf);
            w_sum += w;

            if ((w > 0) && (rng.Uniform() < (w / w_sum)))
            {
                ret.wi = wi_refl;
                ret.f = target_bsdf;
                targetLum = targetLum_bsdf;
            }

            // TODO pdf calculation seems wrong, all pdfs should be in area measure
            ret.pdf = w_sum == 0 ? 0 : targetLum / w_sum;

            return ret;
        }

        float3 wi;
        float3 f;
        float pdf;
    };

    SunSample SampleSunDirection(uint2 DTid, uint frameNum, float3 sunDir, float sunCosAngularRadius, 
        float3 normal, BSDF::ShadingData surface)
    {
        RNG rng = RNG::Init(DTid, frameNum);
        return SunSample::get(sunDir, sunCosAngularRadius, normal, surface, rng);
    }
}

#endif