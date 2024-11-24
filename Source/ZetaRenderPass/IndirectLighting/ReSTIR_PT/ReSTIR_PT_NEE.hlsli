#ifndef RESTIR_PT_NEE_H
#define RESTIR_PT_NEE_H

#include "Params.hlsli"
#include "../NEE.hlsli"
#include "../../Common/BSDFSampling.hlsli"

namespace RPT_Util
{
    ReSTIR_Util::DirectLightingEstimate NEE_NonEmissive(float3 pos, float3 normal, 
        BSDF::ShadingData surface, ConstantBuffer<cbFrameConstants> g_frame, 
        RaytracingAccelerationStructure g_bvh, inout RNG rng)
    {
        ReSTIR_Util::DirectLightingEstimate ret = ReSTIR_Util::DirectLightingEstimate::Init();
        ret.dwdA = 1;

        // Weighted reservoir sampling to sample Le x BSDF, with BSDF lobes as source distributions
        ReSTIR_Util::SkyIncidentRadiance leFunc = ReSTIR_Util::SkyIncidentRadiance::Init(
            g_frame.EnvMapDescHeapOffset);

        float w_sum = 0;
        float3 target_z = 0;
        const float u_wrs = rng.Uniform();

        // Sample sun
        {
            const float3 wi_s = -g_frame.SunDir;
            // Skip when Sun is below the horizon or it hits backside of non-transmissive surface
            const bool visible = (wi_s.y > 0) &&
                ((dot(wi_s, normal) > 0) || surface.Transmissive());
            float pdf_s = 0;

            if(visible)
            {
                surface.SetWi(wi_s, normal);
                target_z = Light::Le_Sun(pos, g_frame) * BSDF::Unified(surface).f;

#if SKY_SAMPLING_PREFER_PERFORMANCE == 1
                pdf_s = BSDF::BSDFSamplerPdf_NoDiffuse(normal, surface, wi_s, leFunc);
#else
                pdf_s = BSDF::BSDFSamplerPdf(normal, surface, wi_s, leFunc, rng);
#endif
            }

            w_sum = RT::BalanceHeuristic(1, pdf_s, Math::Luminance(target_z));

            ret.lt = Light::TYPE::SUN;
            ret.lobe = BSDF::LOBE::ALL;
            ret.wi = wi_s;
        }

        // Sample sky
        {
#if SKY_SAMPLING_PREFER_PERFORMANCE == 1
            BSDF::BSDFSample bsdfSample = BSDF::SampleBSDF_NoDiffuse(normal, surface, 
                leFunc, rng);
#else
            BSDF::BSDFSample bsdfSample = BSDF::SampleBSDF(normal, surface, leFunc, rng);
#endif
            const float w_b = Math::Luminance(bsdfSample.bsdfOverPdf);
            w_sum += w_b;

            if((w_sum > 0) && (u_wrs < (w_b / w_sum)))
            {
                ret.lt = Light::TYPE::SKY;
                ret.lobe = bsdfSample.lobe;
                ret.wi = bsdfSample.wi;

                target_z = bsdfSample.f;
            }
        }

        const float targetLum = Math::Luminance(target_z);
        ret.ld = targetLum > 0 ? target_z * w_sum / targetLum : 0;
        ret.pdf_solidAngle = w_sum > 0 ? targetLum / w_sum : 0;

        if(dot(ret.ld, ret.ld) > 0)
            ret.ld *= RtRayQuery::Visibility_Ray(pos, ret.wi, normal, g_bvh, surface.Transmissive());

        return ret;
    }

    ReSTIR_Util::DirectLightingEstimate NEE_Bsdf(float3 pos, float3 normal, 
        BSDF::ShadingData surface, int nextBounce, ReSTIR_Util::Globals globals, 
        uint emissiveMapsDescHeapOffset, out BSDF::BSDFSample bsdfSample, 
        out RtRayQuery::Hit_Emissive hitInfo, inout RNG rng)
    {
        ReSTIR_Util::DirectLightingEstimate ret = ReSTIR_Util::DirectLightingEstimate::Init();

        // No point in light sampling for specular surfaces
        const bool specular = surface.GlossSpecular() && (surface.metallic || surface.specTr) && 
            (!surface.Coated() || surface.CoatSpecular());
        const int numLightSamples = specular ? 0 : 1;

        // In the last iteration of path tracing loop, BSDF sampling here is just used for direct 
        // lighting. Use <= instead of < to cover that case.
        if(nextBounce <= globals.maxNumBounces)
            bsdfSample = BSDF::SampleBSDF(normal, surface, rng);

        const float wiPdf = bsdfSample.pdf;
        const float3 wi = bsdfSample.wi;
        const float3 f = bsdfSample.f;

        // Check if closest hit is a light source
        hitInfo = RtRayQuery::Hit_Emissive::FindClosest(pos, normal, wi, globals.bvh, 
            globals.frameMeshData, surface.Transmissive());

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
            float lightPdf = 0;

            if(!specular)
            {
                const float lightSourcePdf = numLightSamples > 0 ?
                    globals.aliasTable[hitInfo.emissiveTriIdx].CachedP_Orig : 
                    0;
                lightPdf = twoArea > 0 ? lightSourcePdf * (2.0f / twoArea) : 0;
            }

            // Solid angle measure to area measure
            float dwdA = saturate(dot(lightNormal, -wi)) / (hitInfo.t * hitInfo.t);
            float wiPdf_area = wiPdf * dwdA;
            float3 ld = le * f * dwdA;

            ret.ld = specular ? (wiPdf_area > 0 ? ld / wiPdf_area : 0) : 
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
        if(nextBounce >= globals.maxNumBounces)
            bsdfSample.bsdfOverPdf = 0;

        return ret;
    }

    ReSTIR_Util::DirectLightingEstimate NEE_Emissive(float3 pos, float3 normal, 
        BSDF::ShadingData surface, uint sampleSetIdx, uint numEmissives, int nextBounce, 
        ReSTIR_Util::Globals globals, uint emissiveMapsDescHeapOffset, inout RNG rng)
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
        rng.Uniform3D();
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
            float3 ld = le * BSDF::Unified(surface).f * dwdA;
                
            if (dot(ld, ld) > 0)
            {
                ld *= RtRayQuery::Visibility_Segment(pos, wi, t, normal, lightID, 
                    globals.bvh, surface.Transmissive());
            }

            float bsdfPdf = 0;
            if(dot(ld, ld) > 0)
            {
                bsdfPdf = BSDF::BSDFSamplerPdf(normal, surface, wi, rng);
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
        ReSTIR_Util::SkyIncidentRadiance leFunc = ReSTIR_Util::SkyIncidentRadiance::Init(
            skyViewDescHeapOffset);
        BSDF::BSDFSamplerEval eval = BSDF::EvalBSDFSampler(normal, surface, wi, lobe, leFunc, rng);

        ReSTIR_Util::DirectLightingEstimate ret;
        // = Le(wi) * BSDF(wi) / pdf
        ret.ld = eval.bsdfOverPdf;
        ret.pdf_solidAngle = eval.pdf;

        if(TestVisibility && (dot(ret.ld, ret.ld) > 0))
            ret.ld *= RtRayQuery::Visibility_Ray(pos, wi, normal, g_bvh, surface.Transmissive());

        return ret;
    }

    ReSTIR_Util::DirectLightingEstimate EvalDirect_Emissive_Case2(float3 pos, float3 normal, 
        BSDF::ShadingData surface, float3 wi, float3 le, float dwdA, float lightPdf, 
        BSDF::LOBE lobe, inout RNG rngReplay, inout RNG rngNEE)
    {
        surface.SetWi(wi, normal);
        float3 ld = le * BSDF::Unified(surface).f * dwdA;

        ReSTIR_Util::DirectLightingEstimate ret = ReSTIR_Util::DirectLightingEstimate::Init();
        if(dot(ld, ld) == 0)
            return ret;

        if(lobe == BSDF::LOBE::ALL)
        {
            rngNEE.Uniform4D();

            float bsdfPdf = BSDF::BSDFSamplerPdf(normal, surface, wi, rngNEE);
            float bsdfPdf_area = bsdfPdf * dwdA;

            ret.ld = RT::PowerHeuristic(lightPdf, bsdfPdf_area, ld);
            // Jacobian of shift simplifies to 1.0 when light sampling is used
            ret.pdf_solidAngle = 1.0f;
        }
        else
        {
            BSDF::BSDFSamplerEval eval = BSDF::EvalBSDFSampler(normal, surface, wi, lobe, rngReplay);

            const bool specular = surface.GlossSpecular() && (surface.metallic || surface.specTr) && 
                (!surface.Coated() || surface.CoatSpecular());
            float bsdfPdf_area = eval.pdf * dwdA;
            ret.ld = specular ? (bsdfPdf_area > 0 ? ld / bsdfPdf_area : 0) : 
                RT::PowerHeuristic(bsdfPdf_area, lightPdf, ld);
            ret.pdf_solidAngle = eval.pdf;
        }

        return ret;
    }

    ReSTIR_Util::DirectLightingEstimate EvalDirect_Emissive_Case3(float3 pos, float3 normal, 
        BSDF::ShadingData surface, float3 wi, float t, float3 le, float3 lightNormal, 
        float lightPdf, uint lightID, bool twoSided, BSDF::LOBE lobe, 
        RaytracingAccelerationStructure g_bvh, inout RNG rngReplay, inout RNG rngNEE)
    {
        float wiDotLightNormal = dot(lightNormal, -wi);
        float dwdA = abs(wiDotLightNormal) / (t * t);

        surface.SetWi(wi, normal);
        float3 ld = (wiDotLightNormal > 0) || twoSided ? 
            le * BSDF::Unified(surface).f * dwdA :
            0;

        if(dot(ld, ld) > 0)
        {
            ld *= RtRayQuery::Visibility_Segment(pos, wi, t, normal, lightID, g_bvh, 
                surface.Transmissive());
        }

        ReSTIR_Util::DirectLightingEstimate ret = ReSTIR_Util::DirectLightingEstimate::Init();
        if(dot(ld, ld) == 0)
            return ret;

        if(lobe == BSDF::LOBE::ALL)
        {
            rngNEE.Uniform4D();

            float bsdfPdf = BSDF::BSDFSamplerPdf(normal, surface, wi, rngNEE);
            float bsdfPdf_area = bsdfPdf * dwdA;

            ret.ld = RT::PowerHeuristic(lightPdf, bsdfPdf_area, ld);
            // Jacobian of shift simplifies to 1.0 when light sampling is used
            ret.pdf_solidAngle = 1.0f;
        }
        else
        {
            BSDF::BSDFSamplerEval eval = BSDF::EvalBSDFSampler(normal, surface, wi, lobe, rngReplay);

            const bool specular = surface.GlossSpecular() && (surface.metallic || surface.specTr) && 
                (!surface.Coated() || surface.CoatSpecular());
            float bsdfPdf_area = eval.pdf * dwdA;
            ret.ld = specular ? (bsdfPdf_area > 0 ? ld / bsdfPdf_area : 0) : 
                RT::PowerHeuristic(bsdfPdf_area, lightPdf, ld);
            // = BSDF sampling PDF (solid angle measure) times geometry term
            ret.pdf_solidAngle = bsdfPdf_area;
        }

        return ret;
    }
}

#endif