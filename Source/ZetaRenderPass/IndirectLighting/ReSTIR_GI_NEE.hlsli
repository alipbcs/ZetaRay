#include "../Common/BRDF.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/LightVoxelGrid.hlsli"
#include "ReSTIR_GI_Trace.hlsli"

namespace RGI_Util
{
    float3 NEE_Sun(float3 pos, float3 normal, BRDF::ShadingData surface, 
        RaytracingAccelerationStructure g_bvh, ConstantBuffer<cbFrameConstants> g_frame, inout RNG rng)
    {
        float3 wi = -g_frame.SunDir;

#if SUN_DISK_SAMPLING == 1
        float pdf;
        float3 sampleLocal = Sampling::UniformSampleCone(rng.Uniform2D(), g_frame.SunCosAngularRadius, pdf);
        
        float3 T;
        float3 B;
        Math::Transform::revisedONB(wi, T, B);
        wi = sampleLocal.x * T + sampleLocal.y * B + sampleLocal.z * wi;
#endif
        
        bool isUnoccluded = RGI_Trace::Visibility_Ray(pos, wi, normal, g_bvh);
        if (!isUnoccluded)
            return 0.0;
        
        surface.SetWi(wi, normal);
        float3 brdfxCosTheta = BRDF::CombinedBRDF(surface);
        float3 le = Light::Le_Sun(pos, g_frame);
        float3 Lo = brdfxCosTheta * le;
        Lo = min(Lo, le);
        
        return Lo;
    }

    float3 NEE_Sky_RIS(float3 pos, float3 normal, BRDF::ShadingData surface, 
        RaytracingAccelerationStructure g_bvh, uint skyViewDescHeapOffset, inout RNG rng)
    {
        // Fast path for metallic surface
        if(surface.IsMetallic())
        {
            const float2 u = rng.Uniform2D();
            const float3 wi_g = BRDF::SampleSpecularMicrofacet(surface, normal, u);
            surface.SetWi(wi_g, normal);

            float3 Ld = Light::Le_Sky(wi_g, skyViewDescHeapOffset);
            Ld *= BRDF::SpecularMicrofacetGGXSmithDivPdf(surface);

            if(Math::Luminance(Ld) > 1e-5)
                Ld *= RGI_Trace::Visibility_Ray(pos, wi_g, normal, g_bvh);

            return Ld;
        }

        // Given the size and relatively low-frequency nature of sky, BRDF sampling should be fine.
        float w_sum;
        float3 wi;
        float3 target;
        float targetLum;

        // Sample glossy/specular BRDF
        {
            const float2 u = rng.Uniform2D();
            wi = BRDF::SampleSpecularMicrofacet(surface, normal, u);
            surface.SetWi(wi, normal);

            const float pdf_g = BRDF::GGXVNDFReflectionPdf(surface);
            const float pdf_d = !surface.backfacing_wi * BRDF::LambertianBRDFPdf(surface);

            target = BRDF::CombinedBRDF(surface);
            target *= Light::Le_Sky(wi, skyViewDescHeapOffset);
            targetLum = Math::Luminance(target);
            w_sum = RT::BalanceHeuristic(pdf_g, pdf_d, targetLum);
        }

        // Lambertian BRDF
        {
            float pdf_d;
            float3 wi_d = BRDF::SampleLambertianBrdf(normal, rng.Uniform2D(), pdf_d);
            surface.SetWi(wi_d, normal);

            float pdf_g = BRDF::GGXVNDFReflectionPdf(surface);
            
            float3 target_d = BRDF::CombinedBRDF(surface);
            target_d *= Light::Le_Sky(wi_d, skyViewDescHeapOffset);
            float targetLum_d = Math::Luminance(target_d);
            float w = RT::BalanceHeuristic(pdf_d, pdf_g, targetLum_d);
            w_sum += w;

            if (rng.Uniform() < (w / max(1e-6f, w_sum)))
            {
                wi = wi_d;
                target = target_d;
                targetLum = targetLum_d;
            }
        }

        if(Math::Luminance(target) > 1e-5)
            target *= RGI_Trace::Visibility_Ray(pos, wi, normal, g_bvh);

        float3 Ld = target * (w_sum / max(targetLum, 1e-6));

        return Ld;
    }

    float3 NEE_Sky_MIS(float3 pos, float3 normal, BRDF::ShadingData surface, 
        RaytracingAccelerationStructure g_bvh, uint skyViewDescHeapOffset, inout RNG rng)
    {
        // Given the size and relatively low-frequency nature of sky, BRDF sampling should be fine.
        float3 Ld = 1;

        // Sample glossy/specular BRDF
        {
            const float2 u = rng.Uniform2D();
            const float3 wi_g = BRDF::SampleSpecularMicrofacet(surface, normal, u);
            surface.SetWi(wi_g, normal);

            Ld *= Light::Le_Sky(wi_g, skyViewDescHeapOffset);

            if(Math::Luminance(Ld) > 1e-5)
                Ld *= RGI_Trace::Visibility_Ray(pos, wi_g, normal, g_bvh);
        }

        // Fast path for metallic surface
        if(surface.IsMetallic())
            return Ld * BRDF::SpecularMicrofacetGGXSmithDivPdf(surface);

        // MIS for glossy sample
        {
            Ld *= BRDF::CombinedBRDF(surface);

            const float p_g = BRDF::GGXVNDFReflectionPdf(surface);
            const float p_d = !surface.backfacing_wi * BRDF::LambertianBRDFPdf(surface);
            Ld = RT::PowerHeuristic(p_g, p_d, Ld);
        }

        // Lambertian BRDF
        {
            const float2 u = rng.Uniform2D();
            float p_d;
            const float3 wi_d = BRDF::SampleLambertianBrdf(normal, u, p_d);
            surface.SetWi(wi_d, normal);

            float3 Ld_1 = BRDF::CombinedBRDF(surface);
            Ld_1 *= Light::Le_Sky(wi_d, skyViewDescHeapOffset);
            
            if(Math::Luminance(Ld_1) > 1e-5)
                Ld_1 *= RGI_Trace::Visibility_Ray(pos, wi_d, normal, g_bvh);

            const float p_g = BRDF::GGXVNDFReflectionPdf(surface);
            Ld += RT::PowerHeuristic(p_d, p_g, Ld_1);
        }

        return Ld;
    }

    float3 NEE_Emissive(float3 pos, float3 normal, BRDF::ShadingData surface, uint sampleSetIdx,
        uint sampleSetSize, uint numEmissives, RaytracingAccelerationStructure g_bvh, 
        StructuredBuffer<RT::MeshInstance> g_frameMeshData, 
        StructuredBuffer<RT::EmissiveTriangle> g_emissives, 
        StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets, 
        StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable, 
        uint emissiveMapsDescHeapOffset, inout RNG rng)
    {
        float3 Ld = 0;

        [loop]
        for (int s_l = 0; s_l < NEE_POWER_SAMPLING_NUM_LIGHT_SAMPLES; s_l++)
        {
#if defined(USE_PRESAMPLED_SETS)
            RT::PresampledEmissiveTriangle tri = Light::UnformSampleSampleSet(sampleSetIdx, g_sampleSets, 
                sampleSetSize, rng);

            Light::EmissiveTriAreaSample lightSample;
            lightSample.pos = tri.pos;
            lightSample.normal = Math::DecodeOct16(tri.normal);
            lightSample.bary = Math::DecodeUNorm2(tri.bary);

            float3 Le = tri.le;
            const float lightPdf = tri.pdf;
            const uint emissiveIdx = tri.idx;
            const uint lightID = tri.ID;

            if(tri.twoSided && dot(pos - tri.pos, lightSample.normal) < 0)
                lightSample.normal *= -1;
#else
            float lightSourcePdf;
            const uint emissiveIdx = Light::SampleAliasTable(g_aliasTable, numEmissives, rng, lightSourcePdf);
            RT::EmissiveTriangle emissive = g_emissives[emissiveIdx];

            const Light::EmissiveTriAreaSample lightSample = Light::SampleEmissiveTriangleSurface(pos, emissive, rng);

            float3 Le = Light::Le_EmissiveTriangle(emissive, lightSample.bary, emissiveMapsDescHeapOffset);
            const float lightPdf = lightSourcePdf * lightSample.pdf;
            const uint lightID = emissive.ID;
#endif

            const float t = length(lightSample.pos - pos);
            const float3 wi = (lightSample.pos - pos) / t;

            if(dot(lightSample.normal, -wi) > 0)
            {
                const float dwdA = saturate(dot(lightSample.normal, -wi)) / (t * t);

                surface.SetWi(wi, normal);
                Le *= BRDF::CombinedBRDF(surface) * dwdA;
                    
                if (Math::Luminance(Le) > 1e-6)
                    Le *= RGI_Trace::Visibility_Segment(pos, wi, t, normal, lightID, g_bvh);

                Ld += Le / lightPdf;
            }
        }

        Ld /= NEE_POWER_SAMPLING_NUM_LIGHT_SAMPLES;

        return Ld;
    }

    float3 NEE_Emissive_MIS(float3 pos, float3 normal, BRDF::ShadingData surface, uint sampleSetIdx,
        uint sampleSetSize, uint numEmissives, RaytracingAccelerationStructure g_bvh, 
        StructuredBuffer<RT::MeshInstance> g_frameMeshData, 
        StructuredBuffer<RT::EmissiveTriangle> g_emissives, 
        StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets, 
        StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable, 
        uint emissiveMapsDescHeapOffset, inout RNG rng)
    {
        float3 Ld = 0;

        // BRDF sampling
        {
            const float3 wi = BRDF::SampleSpecularMicrofacet(surface, normal, rng.Uniform2D());

            // Check if closest hit is a light source
            RGI_Trace::HitSurface_Emissive hitInfo;
            bool hitEmissive = RGI_Trace::FindClosestHit_Emissive(pos, normal, wi, g_bvh, g_frameMeshData, hitInfo);

            if (hitEmissive)
            {
                RT::EmissiveTriangle emissive = g_emissives[hitInfo.emissiveTriIdx];
                float3 Le = Light::Le_EmissiveTriangle(emissive, hitInfo.bary, emissiveMapsDescHeapOffset);

                const float3 vtx1 = Light::DecodeEmissiveTriV1(emissive);
                const float3 vtx2 = Light::DecodeEmissiveTriV2(emissive);
                float3 lightNormal = cross(vtx1 - emissive.Vtx0, vtx2 - emissive.Vtx0);
                float twoArea = length(lightNormal);
                twoArea = max(twoArea, 1e-6);
                lightNormal = all(lightNormal == 0) ? 1.0.xxx : lightNormal / twoArea;
                lightNormal = emissive.IsDoubleSided() && dot(-wi, lightNormal) < 0 ? lightNormal * -1.0f : lightNormal;

                const float lightSourcePdf = g_aliasTable[hitInfo.emissiveTriIdx].CachedP_Orig;
                const float lightPdf = lightSourcePdf * (1.0f / (0.5f * twoArea));

                surface.SetWi(wi, normal);
                float wiPdf = BRDF::GGXVNDFReflectionPdf(surface);

                // Solid angle measure to area measure
                float dwdA = saturate(dot(lightNormal, -wi)) / max(hitInfo.t * hitInfo.t, 1e-6f);
                wiPdf *= dwdA;

                Le *= BRDF::CombinedBRDF(surface) * dwdA;
                Ld = RT::PowerHeuristic(wiPdf, lightPdf, Le, 1, NEE_POWER_SAMPLING_NUM_LIGHT_SAMPLES);
            }
        }

        // Light sampling
        [loop]
        for (int s_l = 0; s_l < NEE_POWER_SAMPLING_NUM_LIGHT_SAMPLES; s_l++)
        {
#if defined(USE_PRESAMPLED_SETS)
            RT::PresampledEmissiveTriangle tri = Light::UnformSampleSampleSet(sampleSetIdx, g_sampleSets, 
                sampleSetSize, rng);

            Light::EmissiveTriAreaSample lightSample;
            lightSample.pos = tri.pos;
            lightSample.normal = Math::DecodeOct16(tri.normal);
            lightSample.bary = Math::DecodeUNorm2(tri.bary);

            float3 Le = tri.le;
            const float lightPdf = tri.pdf;
            const uint emissiveIdx = tri.idx;
            const uint lightID = tri.ID;

            if(tri.twoSided && dot(pos - tri.pos, lightSample.normal) < 0)
                lightSample.normal *= -1;
#else
            float lightSourcePdf;
            const uint emissiveIdx = Light::SampleAliasTable(g_aliasTable, numEmissives, rng, lightSourcePdf);
            RT::EmissiveTriangle emissive = g_emissives[emissiveIdx];

            const Light::EmissiveTriAreaSample lightSample = Light::SampleEmissiveTriangleSurface(pos, emissive, rng);

            float3 Le = Light::Le_EmissiveTriangle(emissive, lightSample.bary, emissiveMapsDescHeapOffset);
            const float lightPdf = lightSourcePdf * lightSample.pdf;
            const uint lightID = emissive.ID;
#endif

            const float t = length(lightSample.pos - pos);
            const float3 wi = (lightSample.pos - pos) / t;
            
            if(dot(lightSample.normal, -wi) > 0)
            {
                const float dwdA = saturate(dot(lightSample.normal, -wi)) / (t * t);

                surface.SetWi(wi, normal);
                Le *= BRDF::CombinedBRDF(surface) * dwdA;
                
                if (Math::Luminance(Le) > 1e-6)
                    Le *= RGI_Trace::Visibility_Segment(pos, wi, t, normal, lightID, g_bvh);

                const float brdfPdf = BRDF::GGXVNDFReflectionPdf(surface) * dwdA;
                Ld += RT::PowerHeuristic(lightPdf, brdfPdf, Le, NEE_POWER_SAMPLING_NUM_LIGHT_SAMPLES);
            }
        }

        return Ld;
    }

    float3 NEE_Emissive_LVG(float3 pos, float3 normal, BRDF::ShadingData surface, uint sampleSetIdx,
        uint sampleSetSize, uint numEmissives, int numSamples, float3x4 view, uint3 gridDim, float3 extents, 
        float offset_y, RaytracingAccelerationStructure g_bvh, 
        StructuredBuffer<RT::MeshInstance> g_frameMeshData, 
        StructuredBuffer<RT::EmissiveTriangle> g_emissives, 
        StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets, 
        StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable, 
        StructuredBuffer<RT::VoxelSample> g_lvg,
        uint emissiveMapsDescHeapOffset, inout RNG rng)
    {
        float3 Ld = 0;

        [loop]
        for (int s_l = 0; s_l < numSamples; s_l++)
        {
            RT::VoxelSample s;
            Light::EmissiveTriAreaSample lightSample;
            float3 Le;
            float lightPdf;
            uint lightID;

            if(LVG::Sample(pos, gridDim, extents, 64, view, g_lvg, s, rng, offset_y))
            {
                lightSample.pos = s.pos;
                lightSample.normal = Math::DecodeOct16(s.normal);
                Le = s.le;
                lightPdf = s.pdf;
                lightID = s.ID;

                if(s.twoSided && dot(lightSample.normal, pos - lightSample.pos) < 0)
                    lightSample.normal *= -1;
            }
            else
            {
                RT::PresampledEmissiveTriangle tri = Light::UnformSampleSampleSet(sampleSetIdx, g_sampleSets, 
                    sampleSetSize, rng);

                lightSample.pos = tri.pos;
                lightSample.normal = Math::DecodeOct16(tri.normal);
                lightSample.bary = Math::DecodeUNorm2(tri.bary);

                Le = tri.le;
                lightPdf = tri.pdf;
                lightID = tri.ID;

                if(tri.twoSided && dot(pos - tri.pos, lightSample.normal) < 0)
                    lightSample.normal *= -1;
            }

            const float t = length(lightSample.pos - pos);
            const float3 wi = (lightSample.pos - pos) / t;
            
            if(lightID != uint32_t(-1) && dot(lightSample.normal, -wi) > 0)
            {
                const float dwdA = saturate(dot(lightSample.normal, -wi)) / (t * t);

                surface.SetWi(wi, normal);
                Le *= BRDF::CombinedBRDF(surface) * dwdA;
                
                if (Math::Luminance(Le) > 1e-6)
                    Le *= RGI_Trace::Visibility_Segment(pos, wi, t, normal, lightID, g_bvh);

                Ld += Le / max(lightPdf, 1e-6);
            }
        }

        Ld /= numSamples;

        return Ld;
    }

    float3 NEE(float3 pos, RGI_Trace::HitSurface hitInfo, BRDF::ShadingData surface, 
        uint sampleSetIdx, int bounce, RaytracingAccelerationStructure g_bvh, 
        ConstantBuffer<cbFrameConstants> g_frame, ConstantBuffer<cb_ReSTIR_GI_SpatioTemporal> g_local,
        StructuredBuffer<RT::MeshInstance> g_frameMeshData,
        StructuredBuffer<RT::EmissiveTriangle> g_emissives,
        StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets, 
        StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable,
        StructuredBuffer<RT::VoxelSample> g_lvg,
        inout RNG rngThread, inout RNG rngGroup)
    {
#if !defined (NEE_EMISSIVE)
        float p_sun = rngThread.Uniform();

        // Sun and sky can be skipped if sun is below the horizon.
        if(-g_frame.SunDir.y > 0)
        {
            // Consider the sun only if the (opaque) surface is not oriented away.
            float q = (dot(-g_frame.SunDir, hitInfo.normal) > 1e-6) * P_SUN_VS_SKY;

            if(p_sun < q)
                return NEE_Sun(pos, hitInfo.normal, surface, g_bvh, g_frame, rngThread) / q;

            // Use MIS (two shadow rays) for the first hit, RIS (one shadow ray) for subsequent hits
#if 0
            if(bounce == 0)
                return NEE_Sky_MIS(pos, hitInfo.normal, surface, g_bvh, g_frame.EnvMapDescHeapOffset, rngThread) / (1 - q);
            else
                return NEE_Sky_RIS(pos, hitInfo.normal, surface, g_bvh, g_frame.EnvMapDescHeapOffset, rngThread) / (1 - q);
#else
            return NEE_Sky_RIS(pos, hitInfo.normal, surface, g_bvh, g_frame.EnvMapDescHeapOffset, rngThread) / (1 - q);
#endif
        }

        return 0.0;
#else

#if NEE_USE_MIS == 1

#if NEE_MIS_ALL_BOUNCES == 0
        if(bounce == 0)
#endif
        {
            return NEE_Emissive_MIS(pos, hitInfo.normal, surface, sampleSetIdx, g_local.SampleSetSize,
                g_frame.NumEmissiveTriangles, g_bvh, g_frameMeshData, g_emissives, g_sampleSets, g_aliasTable, 
                g_frame.EmissiveMapsDescHeapOffset, rngThread);
        }
#if NEE_MIS_ALL_BOUNCES == 0
        else
        {
#if defined(USE_LVG) && defined(USE_PRESAMPLED_SETS)
            return NEE_Emissive_LVG(pos, hitInfo.normal, surface, sampleSetIdx, 
                g_local.SampleSetSize, g_frame.NumEmissiveTriangles, 1, g_frame.CurrView, 
                uint3(g_local.GridDim_x, g_local.GridDim_y, g_local.GridDim_z), 
                float3(g_local.Extents_x, g_local.Extents_y, g_local.Extents_z), 
                g_local.Offset_y, g_bvh, g_frameMeshData, g_emissives, g_sampleSets, 
                g_aliasTable, g_lvg, g_frame.EmissiveMapsDescHeapOffset, rngThread);
#else
            return NEE_Emissive(pos, hitInfo.normal, surface, sampleSetIdx, g_local.SampleSetSize, 
                g_frame.NumEmissiveTriangles, g_bvh, g_frameMeshData, g_emissives, 
                g_sampleSets, g_aliasTable, g_frame.EmissiveMapsDescHeapOffset, rngThread);
#endif
        }
#endif

#else

#if defined(USE_LVG)
        const int numSamples = bounce == 0 ? 2 : 1;
        return NEE_Emissive_LVG(pos, hitInfo.normal, surface, sampleSetIdx, 
            g_local.SampleSetSize, g_frame.NumEmissiveTriangles, numSamples, g_frame.CurrView, 
            uint3(g_local.GridDim_x, g_local.GridDim_y, g_local.GridDim_z), 
            float3(g_local.Extents_x, g_local.Extents_y, g_local.Extents_z), 
            g_local.Offset_y, g_bvh, g_frameMeshData, g_emissives, g_sampleSets, 
            g_aliasTable, g_lvg, g_frame.EmissiveMapsDescHeapOffset, rngThread);
#else
        return NEE_Emissive(pos, hitInfo.normal, surface, sampleSetIdx, g_local.SampleSetSize, 
            g_frame.NumEmissiveTriangles, g_bvh, g_frameMeshData, g_emissives, 
            g_sampleSets, g_aliasTable, g_frame.EmissiveMapsDescHeapOffset, rngThread);
#endif

#endif

#endif
    }
}