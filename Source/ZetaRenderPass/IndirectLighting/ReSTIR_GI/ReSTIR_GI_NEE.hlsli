#ifndef RESTIR_GI_NEE_H
#define RESTIR_GI_NEE_H

#include "../NEE.hlsli"

namespace RGI_Util
{
    template<int NumLightSamples, bool skipDiffuse>
    ReSTIR_Util::DirectLightingEstimate NEE_Emissive_MIS(float3 pos, float3 normal, 
        BSDF::ShadingData surface, uint sampleSetIdx, uint numEmissives, 
        ReSTIR_Util::Globals globals, uint emissiveMapsDescHeapOffset, inout RNG rng)
    {
        ReSTIR_Util::DirectLightingEstimate ret = ReSTIR_Util::DirectLightingEstimate::Init();

        // No point in light sampling for specular surfaces
        const int numLightSamples = surface.specular ? 0 : NumLightSamples;

        // BSDF sampling
        {
            BSDF::BSDFSample bsdfSample;
            if(skipDiffuse)
                bsdfSample = BSDF::SampleBSDF_NoDiffuse(normal, surface, rng);
            else
                bsdfSample = BSDF::SampleBSDF(normal, surface, rng);

            float3 wi = bsdfSample.wi;
            float3 f = bsdfSample.f;
            float wiPdf = bsdfSample.pdf;

            // Check if closest hit is a light source
            ReSTIR_RT::Hit_Emissive hitInfo = ReSTIR_RT::Hit_Emissive::FindClosest(pos, normal, 
                wi, globals.bvh, globals.frameMeshData, surface.Transmissive());

            if (hitInfo.HitWasEmissive())
            {
                RT::EmissiveTriangle emissive = globals.emissives[hitInfo.emissiveTriIdx];
                float3 le = Light::Le_EmissiveTriangle(emissive, hitInfo.bary, emissiveMapsDescHeapOffset);

                const float3 vtx1 = Light::DecodeEmissiveTriV1(emissive);
                const float3 vtx2 = Light::DecodeEmissiveTriV2(emissive);
                float3 lightNormal = cross(vtx1 - emissive.Vtx0, vtx2 - emissive.Vtx0);
                float twoArea = length(lightNormal);
                twoArea = max(twoArea, 1e-6);
                lightNormal = dot(lightNormal, lightNormal) == 0 ? 1.0.xxx : lightNormal / twoArea;
                lightNormal = emissive.IsDoubleSided() && dot(-wi, lightNormal) < 0 ? 
                    -lightNormal : lightNormal;

                const float lightSourcePdf = numLightSamples > 0 ?
                    globals.aliasTable[hitInfo.emissiveTriIdx].CachedP_Orig : 
                    0;
                const float lightPdf = lightSourcePdf * (2.0f / twoArea);

                // Solid angle measure to area measure
                float dwdA = hitInfo.t > 0 ? saturate(dot(lightNormal, -wi)) / (hitInfo.t * hitInfo.t) : 0;
                wiPdf *= dwdA;

                le *= f * dwdA;
                ret.ld = RT::PowerHeuristic(wiPdf, lightPdf, le, 1, numLightSamples);
            }
        }

        // Light sampling
        [loop]
        for (int s_l = 0; s_l < numLightSamples; s_l++)
        {
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

            if(tri.twoSided && dot(pos - tri.pos, lightSample.normal) < 0)
                lightSample.normal *= -1;
#else
            Light::AliasTableSample entry = Light::AliasTableSample::get(globals.aliasTable, 
                numEmissives, rng);
            RT::EmissiveTriangle tri = globals.emissives[entry.idx];
            Light::EmissiveTriSample lightSample = Light::EmissiveTriSample::get(pos, tri, rng);

            float3 le = Light::Le_EmissiveTriangle(tri, lightSample.bary, emissiveMapsDescHeapOffset);
            const float lightPdf = entry.pdf * lightSample.pdf;
            const uint lightID = tri.ID;
#endif

            const float t = length(lightSample.pos - pos);
            const float3 wi = (lightSample.pos - pos) / t;
            
            // Light normal was reversed for double-sided lights if backfacing
            if(dot(lightSample.normal, -wi) > 0)
            {
                const float dwdA = saturate(dot(lightSample.normal, -wi)) / (t * t);

                surface.SetWi(wi, normal);
                le *= BSDF::Unified(surface).f * dwdA;
                
                if (dot(le, le) > 0)
                    le *= ReSTIR_RT::Visibility_Segment(pos, wi, t, normal, lightID, globals.bvh, surface.Transmissive());

                float bsdfPdf = skipDiffuse ? 
                    BSDF::BSDFSamplerPdf_NoDiffuse(normal, surface, wi) :
                    BSDF::BSDFSamplerPdf(normal, surface, wi, rng);
                bsdfPdf *= dwdA;

                ret.ld += RT::PowerHeuristic(lightPdf, bsdfPdf, le, numLightSamples);
            }
        }

        return ret;
    }

    ReSTIR_Util::DirectLightingEstimate NEE_Emissive_LVG(float3 pos, float3 normal, 
        BSDF::ShadingData surface, uint sampleSetIdx, uint numEmissives, int numSamples, 
        float3x4 view, ReSTIR_Util::Globals globals, uint emissiveMapsDescHeapOffset, 
        inout RNG rng)
    {
        ReSTIR_Util::DirectLightingEstimate ret = ReSTIR_Util::DirectLightingEstimate::Init();

        [loop]
        for (int s_l = 0; s_l < numSamples; s_l++)
        {
            RT::VoxelSample s;
            Light::EmissiveTriSample lightSample;
            float3 le;
            float lightPdf;
            uint lightID;

            if(LVG::Sample(pos, globals.gridDim, globals.extents, 64, view, globals.lvg, s, rng, (float)globals.offset_y))
            {
                lightSample.pos = s.pos;
                lightSample.normal = Math::DecodeOct32(s.normal);
                le = s.le;
                lightPdf = s.pdf;
                lightID = s.ID;

                if(s.twoSided && dot(lightSample.normal, pos - lightSample.pos) < 0)
                    lightSample.normal *= -1;
            }
            else
            {
                RT::PresampledEmissiveTriangle tri = Light::SamplePresampledSet(sampleSetIdx, globals.sampleSets, 
                    globals.sampleSetSize, rng);

                lightSample.pos = tri.pos;
                lightSample.normal = Math::DecodeOct32(tri.normal);
                lightSample.bary = Math::DecodeUNorm2(tri.bary);

                le = tri.le;
                lightPdf = tri.pdf;
                lightID = tri.ID;

                if(tri.twoSided && dot(pos - tri.pos, lightSample.normal) < 0)
                    lightSample.normal *= -1;
            }

            const float t = length(lightSample.pos - pos);
            const float3 wi = (lightSample.pos - pos) / t;
            
            if(lightID != UINT32_MAX && dot(lightSample.normal, -wi) > 0)
            {
                const float dwdA = saturate(dot(lightSample.normal, -wi)) / (t * t);

                surface.SetWi(wi, normal);
                le *= BSDF::Unified(surface).f * dwdA;
                
                if (Math::Luminance(le) > 1e-6)
                {
                    le *= ReSTIR_RT::Visibility_Segment(pos, wi, t, normal, lightID, globals.bvh, 
                        surface.Transmissive());
                }

                ret.ld += le / max(lightPdf, 1e-6);
            }
        }

        ret.ld /= numSamples;

        return ret;
    }

    ReSTIR_Util::DirectLightingEstimate NEE(float3 pos, float3 normal, BSDF::ShadingData surface, 
        uint sampleSetIdx, int bounce, ConstantBuffer<cbFrameConstants> g_frame, 
        ReSTIR_Util::Globals globals, inout RNG rngThread)
    {
#if NEE_EMISSIVE == 0
        float p_sun = rngThread.Uniform();

        // Sun and sky can be skipped if sun is below the horizon.
        if(-g_frame.SunDir.y > 0)
        {
            // Consider the sun only if the surface is not oriented away.
            float q = (surface.Transmissive() ? 1 : 
                dot(-g_frame.SunDir, normal) > 0) * P_SUN_VS_SKY;

            if(p_sun < q)
            {
                ReSTIR_Util::DirectLightingEstimate ls = ReSTIR_Util::NEE_Sun<true>(pos, normal, surface, 
                    globals.bvh, g_frame, rngThread);
                ls.ld /= q;
                ls.pdf_solidAngle *= q;

                return ls;
            }

            ReSTIR_Util::DirectLightingEstimate ls = ReSTIR_Util::NEE_Sky<true>(pos, normal, surface, 
                globals.bvh, g_frame.EnvMapDescHeapOffset, rngThread);
            ls.ld /= (1 - q);
            ls.pdf_solidAngle *= (1 - q);

            return ls;
        }

        return ReSTIR_Util::DirectLightingEstimate::Init();
#else

#if USE_MIS == 1

        if(bounce == 0)
        {
            return NEE_Emissive_MIS<MIS_NUM_LIGHT_SAMPLES, MIS_NON_DIFFUSE_BSDF_SAMPLING>(pos, 
                normal, surface, sampleSetIdx, g_frame.NumEmissiveTriangles, globals, 
                g_frame.EmissiveMapsDescHeapOffset, rngThread);
        }
        else
        {
#if MIS_ALL_BOUNCES == 1
            return NEE_Emissive_MIS<1, MIS_NON_DIFFUSE_BSDF_SAMPLING>(pos, normal, 
                surface, sampleSetIdx, g_frame.NumEmissiveTriangles, globals, 
                g_frame.EmissiveMapsDescHeapOffset, rngThread);

#elif defined(USE_LVG) && defined(USE_PRESAMPLED_SETS)
            return NEE_Emissive_LVG(pos, normal, surface, sampleSetIdx, 
                g_frame.NumEmissiveTriangles, 1, g_frame.CurrView, 
                globals, g_frame.EmissiveMapsDescHeapOffset, rngThread);
#else
            return ReSTIR_Util::NEE_Emissive<NEE_NUM_LIGHT_SAMPLES>(pos, normal, 
                surface, sampleSetIdx, g_frame.NumEmissiveTriangles, globals, 
                g_frame.EmissiveMapsDescHeapOffset, rngThread);
// NEE_MIS_ALL_BOUNCES
#endif
        }

// !USE_MIS
#else

#if defined(USE_LVG)
        const int numSamples = bounce == 0 ? 2 : 1;
        return ReSTIR_Util::NEE_Emissive_LVG(pos, normal, surface, sampleSetIdx, 
            g_frame.NumEmissiveTriangles, numSamples, g_frame.CurrView, 
            globals, g_frame.EmissiveMapsDescHeapOffset, rngThread);
#else
        return ReSTIR_Util::NEE_Emissive<NEE_NUM_LIGHT_SAMPLES>(pos, normal, 
            surface, sampleSetIdx, g_frame.NumEmissiveTriangles, globals, 
            g_frame.EmissiveMapsDescHeapOffset, rngThread);
#endif

// USE_MIS
#endif

// NEE_EMISSIVE
#endif
    }
}

#endif