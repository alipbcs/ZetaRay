#ifndef RESTIR_PT_NEE_H
#define RESTIR_PT_NEE_H

#include "Params.hlsli"
#include "../NEE.hlsli"
#include "../../Common/BSDFSampling.hlsli"

namespace RPT_Util
{
    struct SkyIncidentRadiance 
    {
        static SkyIncidentRadiance Init(uint descHeapOffset)
        {
            SkyIncidentRadiance ret;
            ret.skyViewDescHeapOffset = descHeapOffset;
            return ret;
        }

        float3 operator()(float3 w)
        {
            return Light::Le_Sky(w, skyViewDescHeapOffset);
        }

        uint skyViewDescHeapOffset;
    };

    ReSTIR_Util::DirectLightingEstimate NEE_NonEmissive(float3 pos, float3 normal, 
        BSDF::ShadingData surface, ConstantBuffer<cbFrameConstants> g_frame, 
        RaytracingAccelerationStructure g_bvh, inout RNG rng)
    {
        float p_sun = rng.Uniform();

        // Sun and sky can be skipped if sun is below the horizon.
        if(-g_frame.SunDir.y > 0)
        {
            // Skip the sun if the opaque surface is oriented away.
            float q = (surface.HasSpecularTransmission() ? 1 : 
                dot(-g_frame.SunDir, normal) > 0) * P_SUN_VS_SKY;

            if(p_sun < q)
            {
                ReSTIR_Util::DirectLightingEstimate ls = ReSTIR_Util::NEE_Sun<true>(pos, normal, surface, 
                    g_bvh, g_frame, rng);
                ls.ld /= q;
                ls.pdf_solidAngle *= q;

                return ls;
            }

            ReSTIR_Util::DirectLightingEstimate ls = ReSTIR_Util::NEE_Sky(pos, normal, surface, 
                g_bvh, g_frame.EnvMapDescHeapOffset, rng);
            ls.ld /= (1 - q);
            ls.pdf_solidAngle *= (1 - q);

            return ls;
        }

        return ReSTIR_Util::DirectLightingEstimate::Init();
    }

    ReSTIR_Util::DirectLightingEstimate NEE_Bsdf(float3 pos, float3 normal, 
        BSDF::ShadingData surface, int nextBounce, ReSTIR_Util::Globals globals, 
        uint emissiveMapsDescHeapOffset, out BSDF::BSDFSample bsdfSample, 
        out ReSTIR_RT::Hit_Emissive hitInfo, inout RNG rng)
    {
        ReSTIR_Util::DirectLightingEstimate ret = ReSTIR_Util::DirectLightingEstimate::Init();

        // No point in light sampling for specular surfaces
        const int numLightSamples = surface.specular ? 0 : 1;

        // In the last iteration of path tracing loop, BSDF sampling is just used for direct 
        // lighting. As a convention, use non-diffuse sampling there. This happens automatically 
        // as in that iteration, the first if condition below always evaluates to false.
        if(nextBounce < globals.maxDiffuseBounces)
            bsdfSample = BSDF::SampleBSDF(normal, surface, rng);
        else
            bsdfSample = BSDF::SampleBSDF_NoDiffuse(normal, surface, rng);

        const float wiPdf = bsdfSample.pdf;
        const float3 wi = bsdfSample.wi;
        const float3 f = bsdfSample.f;

        // Check if closest hit is a light source
        hitInfo = ReSTIR_RT::Hit_Emissive::FindClosest(pos, normal, wi, globals.bvh, 
            globals.frameMeshData, surface.HasSpecularTransmission());

        if (hitInfo.HitWasEmissive())
        {
            RT::EmissiveTriangle emissive = globals.emissives[hitInfo.emissiveTriIdx];
            const float3 le = Light::Le_EmissiveTriangle(emissive, hitInfo.bary, 
                emissiveMapsDescHeapOffset);

            const float3 vtx1 = Light::DecodeEmissiveTriV1(emissive);
            const float3 vtx2 = Light::DecodeEmissiveTriV2(emissive);
            float3 lightNormal = cross(vtx1 - emissive.Vtx0, vtx2 - emissive.Vtx0);
            float twoArea = length(lightNormal);
            lightNormal = dot(lightNormal, lightNormal) == 0 ? 0.0 : lightNormal / twoArea;
            lightNormal = emissive.IsDoubleSided() && (dot(-wi, lightNormal) < 0) ? 
                -lightNormal : lightNormal;

            const float lightSourcePdf = numLightSamples > 0 ?
                globals.aliasTable[hitInfo.emissiveTriIdx].CachedP_Orig : 
                0;
            const float lightPdf = twoArea > 0 ? lightSourcePdf * (2.0f / twoArea) : 0;

            // Solid angle measure to area measure
            float dwdA = saturate(dot(lightNormal, -wi)) / (hitInfo.t * hitInfo.t);
            float wiPdf_area = wiPdf * dwdA;
            float3 ld = le * f * dwdA;

            ret.ld = surface.specular ? (wiPdf_area > 0 ? ld / wiPdf_area : 0) : 
                RT::PowerHeuristic(wiPdf_area, lightPdf, ld);
            ret.le = le;
            ret.wi = wi;
            ret.pdf_solidAngle = wiPdf;
            ret.dwdA = dwdA;
            ret.ID = emissive.ID;
            ret.pos = mad(hitInfo.t, wi, pos);
            ret.normal = lightNormal;
            ret.pdf_light = lightPdf;
            ret.lobe = bsdfSample.lobe;
            ret.lt = Light::TYPE::EMISSIVE;
            ret.twoSided = emissive.IsDoubleSided();
        }

        // Last iteration -- set bsdfSample = null so that we early exit from path tracing loop
        const bool sampleNonDiffuse = (nextBounce < globals.maxGlossyBounces_NonTr) ||
            (surface.HasSpecularTransmission() && (nextBounce < globals.maxGlossyBounces_Tr));

        if((nextBounce >= globals.maxDiffuseBounces) && !sampleNonDiffuse)
            bsdfSample.bsdfOverPdf = 0;

        return ret;
    }

    ReSTIR_Util::DirectLightingEstimate NEE_Emissive(float3 pos, float3 normal, BSDF::ShadingData surface, 
        uint sampleSetIdx, uint numEmissives, int nextBounce, ReSTIR_Util::Globals globals, 
        uint emissiveMapsDescHeapOffset, inout RNG rng)
    {
        ReSTIR_Util::DirectLightingEstimate ret = ReSTIR_Util::DirectLightingEstimate::Init();
        ret.lt = Light::TYPE::EMISSIVE;
        ret.lobe = BSDF::LOBE::ALL;

#if defined(USE_PRESAMPLED_SETS)
        RT::PresampledEmissiveTriangle tri = Light::SamplePresampledSet(sampleSetIdx, globals.sampleSets, 
            globals.sampleSetSize, rng);

        Light::EmissiveTriSample lightSample;
        lightSample.pos = tri.pos;
        lightSample.normal = Math::DecodeOct32(tri.normal);
        lightSample.bary = Math::DecodeUNorm2(tri.bary);

        float3 le = tri.le;
        const float lightPdf = tri.pdf;
        const uint emissiveIdx = tri.idx;
        const uint lightID = tri.ID;
        const bool twoSided = tri.twoSided;

        if(tri.twoSided && dot(pos - tri.pos, lightSample.normal) < 0)
            lightSample.normal *= -1;

        // Deterministic RNG state regardless of USE_PRESAMPLED_SETS
        rng.Uniform2D();
#else
        Light::AliasTableSample entry = Light::AliasTableSample::get(globals.aliasTable, 
            numEmissives, rng);
        RT::EmissiveTriangle tri = globals.emissives[entry.idx];
        Light::EmissiveTriSample lightSample = Light::EmissiveTriSample::get(pos, tri, rng);

        float3 le = Light::Le_EmissiveTriangle(tri, lightSample.bary, emissiveMapsDescHeapOffset);
        const float lightPdf = entry.pdf * lightSample.pdf;
        const uint lightID = tri.ID;
        const bool twoSided = tri.IsDoubleSided();
#endif

        const float t = length(lightSample.pos - pos);
        const float3 wi = (lightSample.pos - pos) / t;

        if((dot(lightSample.normal, -wi) > 0) && (t > 0))
        {
            const float dwdA = saturate(dot(lightSample.normal, -wi)) / (t * t);
            surface.SetWi(wi, normal);
            float3 ld = le * BSDF::UnifiedBSDF(surface) * dwdA;
                
            if (dot(ld, ld) > 0)
            {
                ld *= ReSTIR_RT::Visibility_Segment(pos, wi, t, normal, lightID, 
                    globals.bvh, surface.HasSpecularTransmission());
            }

            float bsdfPdf = 0;
            if(dot(ld, ld) > 0)
            {
                bsdfPdf = nextBounce < globals.maxDiffuseBounces ? 
                    BSDF::BSDFSamplerPdf(normal, surface, wi, rng) :
                    BSDF::BSDFSamplerPdf_NoDiffuse(normal, surface, wi);
                bsdfPdf *= dwdA;
            }

            ret.ld = RT::PowerHeuristic(lightPdf, bsdfPdf, ld);
            ret.le = le;
            ret.wi = wi;
            ret.pdf_solidAngle = lightPdf / dwdA;
            ret.dwdA = dwdA;
            ret.ID = lightID;
            ret.pos = lightSample.pos;
            ret.normal = lightSample.normal;
            ret.pdf_light = lightPdf;
            ret.twoSided = twoSided;
        }

        return ret;
    }

    template<bool TestVisibility>
    ReSTIR_Util::DirectLightingEstimate EvalDirect_Sky(float3 pos, float3 normal, 
        BSDF::ShadingData surface, float3 wi, BSDF::LOBE lobe, uint skyViewDescHeapOffset, 
        RaytracingAccelerationStructure g_bvh, inout RNG rng)
    {
        float2 u_wh = rng.Uniform2D();
        float2 u_d = rng.Uniform2D();
        float u_wrs_r = rng.Uniform();
        float u_wrs_d = rng.Uniform();

        SkyIncidentRadiance leFunc = SkyIncidentRadiance::Init(skyViewDescHeapOffset);
        BSDF::BSDFSamplerEval eval = BSDF::EvalBSDFSampler(normal, surface, wi, lobe, u_wh, u_d, leFunc);

        ReSTIR_Util::DirectLightingEstimate ret;
        // = Le(wi) * BSDF(wi) / pdf
        ret.ld = eval.bsdfOverPdf;
        ret.pdf_solidAngle = eval.pdf;

        if(TestVisibility && (dot(ret.ld, ret.ld) > 0))
            ret.ld *= ReSTIR_RT::Visibility_Ray(pos, wi, normal, g_bvh, surface.HasSpecularTransmission());

        return ret;
    }

    ReSTIR_Util::DirectLightingEstimate EvalDirect_Emissive_Case2(float3 pos, float3 normal, 
        BSDF::ShadingData surface, float3 wi, float3 le, float dwdA, float lightPdf, 
        BSDF::LOBE lobe, int16 bounce, uint16 maxDiffuseBounces, inout RNG rngReplay, 
        inout RNG rngNEE)
    {
        surface.SetWi(wi, normal);
        float3 ld = le * BSDF::UnifiedBSDF(surface) * dwdA;

        ReSTIR_Util::DirectLightingEstimate ret = ReSTIR_Util::DirectLightingEstimate::Init();
        if(dot(ld, ld) == 0)
            return ret;

        if(lobe == BSDF::LOBE::ALL)
        {
            rngNEE.Uniform3D();

            float bsdfPdf = bounce < maxDiffuseBounces ? 
                BSDF::BSDFSamplerPdf(normal, surface, wi, rngNEE) :
                BSDF::BSDFSamplerPdf_NoDiffuse(normal, surface, wi);
            float bsdfPdf_area = bsdfPdf * dwdA;

            ret.ld = RT::PowerHeuristic(lightPdf, bsdfPdf_area, ld);
            // Jacobian of shift simplifies to 1.0 when light sampling is used
            ret.pdf_solidAngle = 1.0f;
        }
        else
        {
            BSDF::BSDFSamplerEval eval;

            if(bounce < maxDiffuseBounces)
                eval = BSDF::EvalBSDFSampler(normal, surface, wi, lobe, rngReplay);
            else
                eval = BSDF::EvalBSDFSampler_NoDiffuse(normal, surface, wi, lobe, rngReplay);

            float bsdfPdf_area = eval.pdf * dwdA;
            ret.ld = surface.specular ? (bsdfPdf_area > 0 ? ld / bsdfPdf_area : 0) : 
                RT::PowerHeuristic(bsdfPdf_area, lightPdf, ld);
            ret.pdf_solidAngle = eval.pdf;
        }

        return ret;
    }

    ReSTIR_Util::DirectLightingEstimate EvalDirect_Emissive_Case3(float3 pos, float3 normal, 
        BSDF::ShadingData surface, float3 wi, float t, float3 le, float3 lightNormal, 
        float lightPdf, uint lightID, bool twoSided, BSDF::LOBE lobe, int16 bounce, 
        uint16 maxDiffuseBounces, RaytracingAccelerationStructure g_bvh, inout RNG rngReplay, 
        inout RNG rngNEE)
    {
        float wiDotLightNormal = dot(lightNormal, -wi);
        float dwdA = abs(wiDotLightNormal) / (t * t);

        surface.SetWi(wi, normal);
        float3 ld = (wiDotLightNormal > 0) || twoSided ? 
            le * BSDF::UnifiedBSDF(surface) * dwdA :
            0;

        if(dot(ld, ld) > 0)
        {
            ld *= ReSTIR_RT::Visibility_Segment(pos, wi, t, normal, lightID, g_bvh, 
                surface.HasSpecularTransmission());
        }

        ReSTIR_Util::DirectLightingEstimate ret = ReSTIR_Util::DirectLightingEstimate::Init();
        if(dot(ld, ld) == 0)
            return ret;

        if(lobe == BSDF::LOBE::ALL)
        {
            rngNEE.Uniform3D();

            float bsdfPdf = bounce < maxDiffuseBounces ? 
                BSDF::BSDFSamplerPdf(normal, surface, wi, rngNEE) :
                BSDF::BSDFSamplerPdf_NoDiffuse(normal, surface, wi);
            float bsdfPdf_area = bsdfPdf * dwdA;

            ret.ld = RT::PowerHeuristic(lightPdf, bsdfPdf_area, ld);
            // Jacobian of shit simplifies to 1.0 when light sampling is used
            ret.pdf_solidAngle = 1.0f;
        }
        else
        {
            BSDF::BSDFSamplerEval eval;

            if(bounce < maxDiffuseBounces)
                eval = BSDF::EvalBSDFSampler(normal, surface, wi, lobe, rngReplay);
            else
                eval = BSDF::EvalBSDFSampler_NoDiffuse(normal, surface, wi, lobe, rngReplay);

            float bsdfPdf_area = eval.pdf * dwdA;
            ret.ld = surface.specular ? (bsdfPdf_area > 0 ? ld / bsdfPdf_area : 0) : 
                RT::PowerHeuristic(bsdfPdf_area, lightPdf, ld);
            ret.pdf_solidAngle = bsdfPdf_area;
        }

        return ret;
    }
}

#endif