#ifndef LIGHT_SOURCE_H
#define LIGHT_SOURCE_H

#include "FrameConstants.h"
#include "StaticTextureSamplers.hlsli"
#include "Volumetric.hlsli"
#include "RT.hlsli"
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
        const float3 sigma_t_rayleigh = g_frame.RayleighSigmaSColor * 
            g_frame.RayleighSigmaSScale;
        const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
        const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * 
            g_frame.OzoneSigmaAScale;

        float3 temp = pos;
        temp.y += g_frame.PlanetRadius;
    
        const float t = Volume::IntersectRayAtmosphere(
            g_frame.PlanetRadius + g_frame.AtmosphereAltitude, temp, -g_frame.SunDir);
        const float3 tr = Volume::EstimateTransmittance(g_frame.PlanetRadius, temp, 
            -g_frame.SunDir, t, sigma_t_rayleigh, sigma_t_mie, sigma_t_ozone, 6);

        return tr * g_frame.SunIlluminance;
    }

    float3 Le_Sky(float3 wi, uint skyViewDescHeapOffset)
    {
        const float2 thetaPhi = Math::SphericalFromCartesian(wi);
        float2 uv = float2(thetaPhi.y * ONE_OVER_2_PI, thetaPhi.x * ONE_OVER_PI);

        // Inverse non-linear map
        const float sn = thetaPhi.x >= PI_OVER_2 ? 1.0f : -1.0f;
        uv.y = mad(0.5f, thetaPhi.x, -PI_OVER_4);
        uv.y = 0.5f + sn * sqrt(abs(uv.y) * ONE_OVER_PI);

        Texture2D<float4> g_envMap = ResourceDescriptorHeap[skyViewDescHeapOffset];
        // Linear sampler prevents banding artifacts
        const float3 le = g_envMap.SampleLevel(g_samLinearWrap, uv, 0.0f).rgb;

        return le;
    }

    float3 Le_SkyWithSunDisk(uint2 DTid, ConstantBuffer<cbFrameConstants> g_frame)
    {
        float3 wc = RT::GeneratePinholeCameraRay(DTid, float2(g_frame.RenderWidth, g_frame.RenderHeight), 
            g_frame.AspectRatio, g_frame.TanHalfFOV, g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, 
            g_frame.CurrView[2].xyz, g_frame.CurrCameraJitter);

        float3 rayOrigin = float3(0, 1e-1, 0);
        rayOrigin.y += g_frame.PlanetRadius;

        float3 wTemp = wc;
        // cos(a - b) = cos a cos b + sin a sin b
        wTemp.y = wTemp.y * g_frame.SunCosAngularRadius + sqrt(1 - wc.y * wc.y) * 
            g_frame.SunSinAngularRadius;

        float t;
        bool intersectedPlanet = Volume::IntersectRayPlanet(g_frame.PlanetRadius, rayOrigin, 
            wTemp, t);

        // a disk that's supposed to be the sun
        if (dot(-wc, g_frame.SunDir) >= g_frame.SunCosAngularRadius && !intersectedPlanet)
            return g_frame.SunIlluminance;
        // sample the sky texture
        else
            return Light::Le_Sky(wc, g_frame.EnvMapDescHeapOffset);
    }

    // Assumes area light is diffuse
    float3 Le_EmissiveTriangle(RT::EmissiveTriangle tri, float2 bary, uint emissiveMapsDescHeapOffset, 
        SamplerState s = g_samPointWrap)
    {
        const float3 emissiveFactor = tri.GetFactor();
        const float emissiveStrength = (float)tri.GetStrength();
        float3 le = emissiveFactor * emissiveStrength;

        if (Math::Luminance(le) == 0)
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
        static SunSample Init()
        {
            SunSample ret;
            ret.f = 0;
            ret.wi = 0;
            ret.pdf = 0;

            return ret;
        }

        static SunSample get(float3 sunDir, float sunCosAngularRadius, float3 normal, 
            BSDF::ShadingData surface, inout RNG rng)
        {
            SunSample ret = SunSample::Init();

            // Light hits the backside and surfaces isn't transmissive
            const float ndotSunDir = dot(sunDir, normal);
            if(ndotSunDir < 0 && !surface.Transmissive())
                return ret;

            // Light sampling - since sun is a very small area light, area sampling 
            // should be fine
            float pdf_light;
            float3 sampleLocal = Sampling::UniformSampleCone(rng.Uniform2D(), 
                sunCosAngularRadius, pdf_light);
            
            Math::CoordinateSystem onb = Math::CoordinateSystem::Build(sunDir);
            float3 wi_light = mad(sampleLocal.x, onb.b1, mad(sampleLocal.y, onb.b2, sampleLocal.z * sunDir));

            surface.SetWi(wi_light, normal);
            ret.f = BSDF::Unified(surface).f;
            ret.wi = wi_light;
            ret.pdf = pdf_light;

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