#ifndef LIGHT_SOURCE_H
#define LIGHT_SOURCE_H

#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/Volumetric.hlsli"
#include "../Common/RT.hlsli"

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

    float3 SampleSunDirection(float3 sunDir, float sunCosAngularRadius, float3 normal, 
        BSDF::ShadingData surface, out float3 f, out float pdf, inout RNG rng)
    {
        pdf = 1.0f;

        // Essentially a mirror, no point in light sampling
        if(surface.specular && surface.IsMetallic())
        {
            float3 wi = reflect(-surface.wo, normal);
            surface.SetWi_Refl(wi, normal);
            f = BSDF::UnifiedBRDF(surface);

            return f;
        }

        // Light sampling
        float pdf_light = 1;
        float3 wi_light;

        // Sample the cone subtended by sun
        {
            float3 sampleLocal = Sampling::UniformSampleCone(rng.Uniform2D(), sunCosAngularRadius, pdf_light);
            
            float3 T;
            float3 B;
            Math::revisedONB(sunDir, T, B);
            wi_light = sampleLocal.x * T + sampleLocal.y * B + sampleLocal.z * sunDir;
        }

        // Since sun is a very small area light, area sampling should be fine unless the surface is specular
        if(!surface.specular)
        {
            surface.SetWi(wi_light, normal);
            f = BSDF::UnifiedBSDF(surface);

            return wi_light;
        }

        // At this point, we know the surface is specular

        // Use the refracted direction only if light hit the backside and surface
        // is transmissive
        if(dot(sunDir, normal) < 0)
        {
            if(surface.HasSpecularTransmission())
            {
                float3 wi = refract(-surface.wo, normal, 1 / surface.eta);
                surface.SetWi_Tr(wi, normal);
                float fr = surface.Fresnel_Dielectric();
                f = surface.transmission * (1 - fr) * surface.diffuseReflectance_Fr0_Metal;

                return wi;
            }

            // Sun hit the backside and surface isn't transmissive
            f = 0;

            // Doesn't matter as f is 0
            return sunDir;
        }

        float3 wi_refl = reflect(-surface.wo, normal);
        bool insideCone = dot(wi_refl, sunDir) >= sunCosAngularRadius;

        if(!insideCone)
        {
            surface.SetWi_Refl(wi_light, normal);
            f = BSDF::UnifiedBRDF(surface);

            return wi_light;
        }

        // Streaming RIS with MIS -- light sample and mirror reflection both result in
        // a nonzero BSDF, e.g. dielectric with 0 roughness
        float3 wi;

        // Feed light sample
        surface.SetWi_Refl(wi_light, normal);
        float3 target = BSDF::DielectricBRDF(surface);
        float targetLum = Math::Luminance(target);
        // rate of change of reflection is twice the rate of change of half vector
        // 0.99996 = 1.0f - (1.0f - MIN_N_DOT_H_SPECULAR) * 2
        float w_sum = RT::BalanceHeuristic(pdf_light, dot(wi_light, wi_refl) >= 0.99996, targetLum);
        wi = wi_light;
        f = target;

        // Feed BSDF sample
        surface.SetWi_Refl(wi_refl, normal);
        float mis_pdf_light = insideCone * pdf_light;
        float3 target_bsdf = BSDF::DielectricBRDF(surface) * insideCone;
        float targetLum_bsdf = Math::Luminance(target_bsdf);
        float w = RT::BalanceHeuristic(1, mis_pdf_light, targetLum_bsdf);
        w_sum += w;

        if (rng.Uniform() < (w / max(1e-6f, w_sum)))
        {
            wi = wi_refl;
            f = target_bsdf;
            targetLum = targetLum_bsdf;
        }

        pdf = w_sum == 0 ? 0 : targetLum / w_sum;

        return wi;
    }

    float3 SampleSunDirection(uint2 DTid, uint frameNum, float3 sunDir, float sunCosAngularRadius, 
        float3 normal, BSDF::ShadingData surface, out float3 f, out float pdf)
    {
        RNG rng = RNG::Init(DTid, frameNum);
        return SampleSunDirection(sunDir, sunCosAngularRadius, normal, surface, f, pdf, rng);
    }
}

#endif