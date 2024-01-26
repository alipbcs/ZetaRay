#ifndef LIGHT_SOURCE_H
#define LIGHT_SOURCE_H

#include "../../ZetaCore/RayTracing/RtCommon.h"
#include "../../ZetaCore/Core/Material.h"
#include "../Common/FrameConstants.h"
#include "../Common/Math.hlsli"
#include "../Common/Sampling.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/Volumetric.hlsli"

namespace Light
{
    struct EmissiveTriAreaSample
    {
        float3 pos;
        float3 normal;
        float2 bary;
        float pdf;
    };

    float3 DecodeEmissiveTriV1(RT::EmissiveTriangle tri)
    {
#if ENCODE_EMISSIVE_POS == 1
        float3 v0v1 = float3(tri.V0V1 / float((1 << 15) - 1), 0);
        v0v1 = Math::DecodeUnitVector(v0v1.xy);

        return mad(v0v1, tri.EdgeLengths.x, tri.Vtx0);
#else
        return Vtx1;
#endif
    }

    float3 DecodeEmissiveTriV2(RT::EmissiveTriangle tri)
    {
#if ENCODE_EMISSIVE_POS == 1
        float3 v0v2 = float3(tri.V0V2 / float((1 << 15) - 1), 0);
        v0v2 = Math::DecodeUnitVector(v0v2.xy);
        
        return mad(v0v2, tri.EdgeLengths.y, tri.Vtx0);
#else
        return Vtx2;
#endif
    }

    uint SampleAliasTable(StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable, uint numEmissiveTriangles, 
        inout RNG rng, out float pdf)
    {
        uint u0 = rng.UniformUintBounded(numEmissiveTriangles);
        RT::EmissiveLumenAliasTableEntry s = g_aliasTable[u0];

        float u1 = rng.Uniform();
        if (u1 <= s.P_Curr)
        {
            pdf = s.CachedP_Orig;
            return u0;
        }

        pdf = s.CachedP_Alias;
        return s.Alias;
    }

    
    RT::PresampledEmissiveTriangle UnformSampleSampleSet(uint sampleSetIdx, StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets, 
        uint sampleSetSize, inout RNG rng)
    {
        uint u = rng.UniformUintBounded_Faster(sampleSetSize);
        uint idx = sampleSetIdx * sampleSetSize + u;
        return g_sampleSets[idx];
    }

    EmissiveTriAreaSample SampleEmissiveTriangleSurface(float3 posW, RT::EmissiveTriangle tri, inout RNG rng, 
        bool reverseNormalIfTwoSided = true)
    {
        EmissiveTriAreaSample ret;

        float2 u = rng.Uniform2D();
        ret.bary = Sampling::UniformSampleTriangle(u);

        const float3 vtx1 = Light::DecodeEmissiveTriV1(tri);
        const float3 vtx2 = Light::DecodeEmissiveTriV2(tri);
        ret.pos = (1.0f - ret.bary.x - ret.bary.y) * tri.Vtx0 + ret.bary.x * vtx1 + ret.bary.y * vtx2;
        ret.normal = cross(vtx1 - tri.Vtx0, vtx2 - tri.Vtx0);
        bool normalIs0 = dot(ret.normal, ret.normal) == 0;
        float twoArea = length(ret.normal);
        twoArea = max(twoArea, 1e-6);
        ret.pdf = normalIs0 ? 1.0f : 1.0f / (0.5f * twoArea);

        ret.normal = normalIs0 ? ret.normal : ret.normal / twoArea;
        ret.normal = reverseNormalIfTwoSided && tri.IsDoubleSided() && dot(posW - ret.pos, ret.normal) < 0 ? 
            ret.normal * -1.0f : 
            ret.normal;

        return ret;
    }

    float3 Le_Sun(float3 pos, ConstantBuffer<cbFrameConstants> g_frame)
    {
        const float3 sigma_t_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
        const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
        const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;

        float3 temp = pos;
        temp.y += g_frame.PlanetRadius;
    
        const float t = Volumetric::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, temp, -g_frame.SunDir);
        const float3 tr = Volumetric::EstimateTransmittance(g_frame.PlanetRadius, temp, -g_frame.SunDir, t,
            sigma_t_rayleigh, sigma_t_mie, sigma_t_ozone, 6);

        return tr * g_frame.SunIlluminance;
    }

    float3 Le_Sky(float3 wi, uint skyViewDescHeapOffset, SamplerState s = g_samPointWrap)
    {
        Texture2D<float3> g_envMap = ResourceDescriptorHeap[skyViewDescHeapOffset];
        const float2 thetaPhi = Math::SphericalFromCartesian(wi);
        float2 uv = float2(thetaPhi.y * ONE_OVER_2_PI, thetaPhi.x * ONE_OVER_PI);

        // undo non-linear sampling
        const float sn = thetaPhi.x >= PI_OVER_2 ? 1.0f : -1.0f;
        uv.y = (thetaPhi.x - PI_OVER_2) * 0.5f;
        uv.y = 0.5f + sn * sqrt(abs(uv.y) * ONE_OVER_PI);
        
        const float3 Le = g_envMap.SampleLevel(s, uv, 0.0f).rgb;

        return Le;
    }

    // Assumes area light is diffuse.
    float3 Le_EmissiveTriangle(RT::EmissiveTriangle tri, float2 bary, uint emissiveMapsDescHeapOffset, 
        SamplerState s = g_samPointWrap)
    {
        const float3 emissiveFactor = Math::UnpackRGB(tri.EmissiveFactor);
        const float emissiveStrength = tri.GetEmissiveStrength();
        float3 L_e = emissiveFactor * emissiveStrength;

        if (Math::Luminance(L_e) < 1e-5)
            return 0.0.xxx;

        uint16_t emissiveTex = tri.GetEmissiveTex();
        if (emissiveTex != uint16_t(-1))
        {
            const uint offset = NonUniformResourceIndex(emissiveMapsDescHeapOffset + emissiveTex);
            EMISSIVE_MAP g_emissiveMap = ResourceDescriptorHeap[offset];

            float2 texUV = (1.0f - bary.x - bary.y) * tri.UV0 + bary.x * tri.UV1 + bary.y * tri.UV2;
            L_e *= g_emissiveMap.SampleLevel(s, texUV, 0).rgb;
        }

        return L_e;
    }
}

#endif