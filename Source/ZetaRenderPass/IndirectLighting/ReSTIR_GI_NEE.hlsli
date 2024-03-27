#include "../Common/BSDF.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/LightVoxelGrid.hlsli"
#include "ReSTIR_GI_RT.hlsli"

namespace RGI_Util
{
    float3 NEE_Sun(float3 pos, float3 normal, BSDF::ShadingData surface, 
        RaytracingAccelerationStructure g_bvh, ConstantBuffer<cbFrameConstants> g_frame, inout RNG rng)
    {
#if SUN_DISK_SAMPLING == 1
        float3 bsdfxCosTheta;
        float pdf;
        float3 wi = Light::SampleSunDirection(-g_frame.SunDir, g_frame.SunCosAngularRadius, 
            normal, surface, bsdfxCosTheta, pdf, rng);
#else
        float3 wi = -g_frame.SunDir;
        surface.SetWi(wi, normal);
        float3 bsdfxCosTheta = BSDF::UnifiedBSDF(surface);
#endif

        if(dot(bsdfxCosTheta, bsdfxCosTheta) == 0)
            return 0.0;

        bool isUnoccluded = RGI_Trace::Visibility_Ray(pos, wi, normal, g_bvh, 
            surface.HasSpecularTransmission());
        if (!isUnoccluded)
            return 0.0;

        float3 le = Light::Le_Sun(pos, g_frame);
        float3 lo = bsdfxCosTheta * le;
#if SUPPRESS_SUN_FIREFLIES == 1
        lo = min(lo, le);
#endif
        return lo;
    }

    float3 NEE_Sky_RIS_Opaque(float3 pos, float3 normal, BSDF::ShadingData surface, 
        RaytracingAccelerationStructure g_bvh, uint skyViewDescHeapOffset, inout RNG rng)
    {
        // Fast path for metallic surface
        if(surface.IsMetallic())
        {
            const float2 u = rng.Uniform2D();
            const float3 wi_g = BSDF::SampleMicrofacetBRDF(surface, normal, u);
            surface.SetWi_Refl(wi_g, normal);

            float3 Ld = Light::Le_Sky(wi_g, skyViewDescHeapOffset);
            Ld *= BSDF::MicrofacetBRDFDivPdf(surface);

            if(Math::Luminance(Ld) > 1e-5)
                Ld *= RGI_Trace::Visibility_Ray(pos, wi_g, normal, g_bvh, false);

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
            wi = BSDF::SampleMicrofacetBRDF(surface, normal, u);
            surface.SetWi_Refl(wi, normal);

            const float pdf_g = BSDF::VNDFReflectionPdf(surface);
            const float pdf_d = !surface.invalid * BSDF::LambertianBRDFPdf(surface);

            target = BSDF::DielectricBRDF(surface);
            target *= Light::Le_Sky(wi, skyViewDescHeapOffset);
            targetLum = Math::Luminance(target);
            w_sum = RT::BalanceHeuristic(pdf_g, pdf_d, targetLum);
        }

        // Lambertian BRDF
        {
            float pdf_d;
            float3 wi_d = BSDF::SampleLambertianBRDF(normal, rng.Uniform2D(), pdf_d);
            surface.SetWi_Refl(wi_d, normal);

            float pdf_g = BSDF::VNDFReflectionPdf(surface);
            
            float3 target_d = BSDF::DielectricBRDF(surface);
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
            target *= RGI_Trace::Visibility_Ray(pos, wi, normal, g_bvh, false);

        float3 Ld = target * (w_sum / max(targetLum, 1e-6));

        return Ld;
    }

    float3 NEE_Sky_RIS(float3 pos, float3 normal, BSDF::ShadingData surface, 
        RaytracingAccelerationStructure g_bvh, uint skyViewDescHeapOffset, inout RNG rng)
    {
        if(surface.IsMetallic() || !surface.HasSpecularTransmission())
            return NEE_Sky_RIS_Opaque(pos, normal, surface, g_bvh, skyViewDescHeapOffset, rng);

        float w_sum = 0;
        float3 target = 0;
        float targetLum = 0;

        float3 wh = surface.specular ? 
            normal : 
            BSDF::SampleMicrofacet(surface.wo, surface.alpha, normal, rng.Uniform2D());

        float wh_pdf = 1;

        // Evaluate VNDF
        if(!surface.specular)
        {
            surface.ndotwh = saturate(dot(normal, wh));
            float alphaSq = surface.alpha * surface.alpha;
            float NDF = BSDF::GGX(surface.ndotwh, alphaSq);
            float G1 = BSDF::SmithG1_GGX(alphaSq, surface.ndotwo);
            wh_pdf = (NDF * G1) / surface.ndotwo;
        }

        float3 wi = refract(-surface.wo, wh, 1 / surface.eta);
        bool tir = dot(wi, wi) == 0;

        // Specular/glossy transmission
        if(!tir)
        {
            surface.SetWi_Tr(wi, normal, wh);
            float pdf_t = 1;

            // Account for change of density from half vector to incident vector
            if(!surface.specular)
            {
                pdf_t = wh_pdf * surface.whdotwo;
                float denom = mad(surface.whdotwi, surface.eta, surface.whdotwo);
                denom *= denom;
                float dwm_dwi = surface.whdotwi / denom;
                pdf_t *= dwm_dwi;
            }

            target = BSDF::DielectricBTDF(surface);
            target *= Light::Le_Sky(wi, skyViewDescHeapOffset);
            targetLum = Math::Luminance(target);
            w_sum = targetLum / max(pdf_t, 1e-6);
        }

        // Specular/glossy reflection
        {
            float3 wi_r = reflect(-surface.wo, wh);
            surface.SetWi_Refl(wi_r, normal, wh);

            float pdf_r = 1;

            // Account for change of density from half vector to incident vector
            if(!surface.specular)
                pdf_r = wh_pdf / 4.0f;

            // After using the law of reflection, VNDF samples might end up below the surface
            float pdf_d = !surface.invalid * 
                (surface.transmission != 1) * 
                BSDF::LambertianBRDFPdf(surface);

            float3 target_r = BSDF::DielectricBRDF(surface);
            target_r *= Light::Le_Sky(wi_r, skyViewDescHeapOffset);
            float targetLum_r = Math::Luminance(target_r);
            float w = RT::BalanceHeuristic(pdf_r, pdf_d, targetLum_r);
            w_sum += w;

            if (rng.Uniform() < (w / max(1e-6f, w_sum)))
            {
                wi = wi_r;
                target = target_r;
                targetLum = targetLum_r;
            }
        }

        // Lambertian
        if(surface.transmission < 1)
        {
            float pdf_d;
            float3 wi_d = BSDF::SampleLambertianBRDF(normal, rng.Uniform2D(), pdf_d);
            surface.SetWi_Refl(wi_d, normal);

            float pdf_r = BSDF::VNDFReflectionPdf(surface);
            
            float3 target_d = BSDF::UnifiedBRDF(surface);
            target_d *= Light::Le_Sky(wi_d, skyViewDescHeapOffset);
            float targetLum_d = Math::Luminance(target_d);
            float w = RT::BalanceHeuristic(pdf_d, pdf_r, targetLum_d);
            w_sum += w;

            if (rng.Uniform() < (w / max(1e-6f, w_sum)))
            {
                wi = wi_d;
                target = target_d;
                targetLum = targetLum_d;
            }
        }

        if(Math::Luminance(target) > 1e-5)
            target *= RGI_Trace::Visibility_Ray(pos, wi, normal, g_bvh, surface.HasSpecularTransmission());

        float3 Ld = target * (w_sum / max(targetLum, 1e-6));
        Ld = any(isnan(Ld)) ? 0 : Ld;

        return Ld;
    }

    // TODO doesn't handle transmission
    float3 NEE_Sky_MIS(float3 pos, float3 normal, BSDF::ShadingData surface, 
        RaytracingAccelerationStructure g_bvh, uint skyViewDescHeapOffset, inout RNG rng)
    {
        // Given the size and relatively low-frequency nature of sky, BRDF sampling should be fine.
        float3 Ld = 1;

        // Sample glossy/specular BRDF
        {
            const float2 u = rng.Uniform2D();
            const float3 wi_g = BSDF::SampleMicrofacetBRDF(surface, normal, u);
            surface.SetWi_Refl(wi_g, normal);

            Ld *= Light::Le_Sky(wi_g, skyViewDescHeapOffset);

            if(Math::Luminance(Ld) > 1e-5)
                Ld *= RGI_Trace::Visibility_Ray(pos, wi_g, normal, g_bvh, false);
        }

        // Fast path for metallic surface
        if(surface.IsMetallic())
            return Ld * BSDF::MicrofacetBRDFDivPdf(surface);

        // MIS for glossy sample
        {
            Ld *= BSDF::DielectricBRDF(surface);

            const float p_g = BSDF::VNDFReflectionPdf(surface);
            const float p_d = !surface.invalid * BSDF::LambertianBRDFPdf(surface);
            Ld = RT::PowerHeuristic(p_g, p_d, Ld);
        }

        // Lambertian BRDF
        {
            const float2 u = rng.Uniform2D();
            float p_d;
            const float3 wi_d = BSDF::SampleLambertianBRDF(normal, u, p_d);
            surface.SetWi_Refl(wi_d, normal);

            float3 Ld_1 = BSDF::DielectricBRDF(surface);
            Ld_1 *= Light::Le_Sky(wi_d, skyViewDescHeapOffset);
            
            if(Math::Luminance(Ld_1) > 1e-5)
                Ld_1 *= RGI_Trace::Visibility_Ray(pos, wi_d, normal, g_bvh, false);

            const float p_g = BSDF::VNDFReflectionPdf(surface);
            Ld += RT::PowerHeuristic(p_d, p_g, Ld_1);
        }

        return Ld;
    }

    float3 NEE_Emissive(float3 pos, float3 normal, BSDF::ShadingData surface, uint sampleSetIdx,
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
                Le *= BSDF::UnifiedBSDF(surface) * dwdA;
                    
                if (Math::Luminance(Le) > 1e-6)
                    Le *= RGI_Trace::Visibility_Segment(pos, wi, t, normal, lightID, g_bvh, surface.HasSpecularTransmission());

                Ld += Le / lightPdf;
            }
        }

        Ld /= NEE_POWER_SAMPLING_NUM_LIGHT_SAMPLES;

        return Ld;
    }

    template<int NumLightSamples>
    float3 NEE_Emissive_MIS(float3 pos, float3 normal, BSDF::ShadingData surface, uint sampleSetIdx,
        uint sampleSetSize, uint numEmissives, RaytracingAccelerationStructure g_bvh, 
        StructuredBuffer<RT::MeshInstance> g_frameMeshData, 
        StructuredBuffer<RT::EmissiveTriangle> g_emissives, 
        StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets, 
        StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable, 
        uint emissiveMapsDescHeapOffset, inout RNG rng)
    {
        float3 Ld = 0;
        float3 f = 0;
        // No point in light sampling for specular surfaces
        const int numLightSamples = surface.specular ? 0 : NumLightSamples;

        // BSDF sampling
        {
            float3 f;
            float wiPdf;
            float3 wi = RT::SampleUnifiedBSDF_NoDiffuse(normal, surface, rng, f, wiPdf);

            // Check if closest hit is a light source
            RGI_Trace::HitSurface_Emissive hitInfo;
            bool hitEmissive = RGI_Trace::FindClosestHit_Emissive(pos, normal, wi, g_bvh, g_frameMeshData, 
                hitInfo, surface.HasSpecularTransmission());

            if (hitEmissive)
            {
                RT::EmissiveTriangle emissive = g_emissives[hitInfo.emissiveTriIdx];
                float3 Le = Light::Le_EmissiveTriangle(emissive, hitInfo.bary, emissiveMapsDescHeapOffset);

                const float3 vtx1 = Light::DecodeEmissiveTriV1(emissive);
                const float3 vtx2 = Light::DecodeEmissiveTriV2(emissive);
                float3 lightNormal = cross(vtx1 - emissive.Vtx0, vtx2 - emissive.Vtx0);
                float twoArea = length(lightNormal);
                twoArea = max(twoArea, 1e-6);
                lightNormal = dot(lightNormal, lightNormal) == 0 ? 1.0.xxx : lightNormal / twoArea;
                lightNormal = emissive.IsDoubleSided() && dot(-wi, lightNormal) < 0 ? lightNormal * -1.0f : lightNormal;

                const float lightSourcePdf = numLightSamples > 0 ?
                    g_aliasTable[hitInfo.emissiveTriIdx].CachedP_Orig : 
                    0;
                const float lightPdf = lightSourcePdf * (1.0f / (0.5f * twoArea));

                // Solid angle measure to area measure
                float dwdA = saturate(dot(lightNormal, -wi)) / max(hitInfo.t * hitInfo.t, 1e-6f);
                wiPdf *= dwdA;

                Le *= f * dwdA;
                Ld = RT::PowerHeuristic(wiPdf, lightPdf, Le, 1, numLightSamples);
            }
        }

        // Light sampling
        [loop]
        for (int s_l = 0; s_l < numLightSamples; s_l++)
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
                Le *= BSDF::UnifiedBSDF(surface) * dwdA;
                
                if (Math::Luminance(Le) > 1e-6)
                    Le *= RGI_Trace::Visibility_Segment(pos, wi, t, normal, lightID, g_bvh, surface.HasSpecularTransmission());

                float bsdfPdf = RT::UnifiedBSDFPdf_NoDiffuse(normal, surface, wi);
                bsdfPdf *= dwdA;

                Ld += RT::PowerHeuristic(lightPdf, bsdfPdf, Le, numLightSamples);
            }
        }

        return Ld;
    }

    float3 NEE_Emissive_LVG(float3 pos, float3 normal, BSDF::ShadingData surface, uint sampleSetIdx,
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
                Le *= BSDF::UnifiedBSDF(surface) * dwdA;
                
                if (Math::Luminance(Le) > 1e-6)
                {
                    Le *= RGI_Trace::Visibility_Segment(pos, wi, t, normal, lightID, g_bvh, 
                        surface.HasSpecularTransmission());
                }

                Ld += Le / max(lightPdf, 1e-6);
            }
        }

        Ld /= numSamples;

        return Ld;
    }

    float3 NEE(float3 pos, RGI_Trace::HitSurface hitInfo, BSDF::ShadingData surface, 
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
            // Consider the sun only if the surface is not oriented away.
            float q = (surface.HasSpecularTransmission() ? 1 : 
                dot(-g_frame.SunDir, hitInfo.normal) > 0) * P_SUN_VS_SKY;

            if(p_sun < q)
                return NEE_Sun(pos, hitInfo.normal, surface, g_bvh, g_frame, rngThread) / q;

#if 0
            // Use MIS (two shadow rays) for the first hit, RIS (one shadow ray) for subsequent hits
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

        uint sampleSetSize = g_local.SampleSetSize_NumSampleSets & 0xffff;
        uint16_t3 gridDim = uint16_t3(g_local.GridDim_xy & 0xffff, g_local.GridDim_xy >> 16, g_local.GridDim_z);
        half3 extents = asfloat16(uint16_t3(g_local.Extents_xy & 0xffff, g_local.Extents_xy >> 16, 
            g_local.Extents_z_Offset_y & 0xffff));
        half offset_y = asfloat16(uint16_t(g_local.Extents_z_Offset_y >> 16));

#if NEE_USE_MIS == 1

        if(bounce == 0)
        {
            return NEE_Emissive_MIS<2>(pos, hitInfo.normal, surface, sampleSetIdx, sampleSetSize,
                g_frame.NumEmissiveTriangles, g_bvh, g_frameMeshData, g_emissives, g_sampleSets, g_aliasTable, 
                g_frame.EmissiveMapsDescHeapOffset, rngThread);
        }
        else
        {
#if NEE_MIS_ALL_BOUNCES == 1
            return NEE_Emissive_MIS<1>(pos, hitInfo.normal, surface, sampleSetIdx, sampleSetSize,
                g_frame.NumEmissiveTriangles, g_bvh, g_frameMeshData, g_emissives, g_sampleSets, g_aliasTable, 
                g_frame.EmissiveMapsDescHeapOffset, rngThread);

#elif defined(USE_LVG) && defined(USE_PRESAMPLED_SETS)
            return NEE_Emissive_LVG(pos, hitInfo.normal, surface, sampleSetIdx, 
                sampleSetSize, g_frame.NumEmissiveTriangles, 1, g_frame.CurrView, 
                gridDim, 
                extents, 
                offset_y, g_bvh, g_frameMeshData, g_emissives, g_sampleSets, 
                g_aliasTable, g_lvg, g_frame.EmissiveMapsDescHeapOffset, rngThread);
#else
            return NEE_Emissive(pos, hitInfo.normal, surface, sampleSetIdx, sampleSetSize, 
                g_frame.NumEmissiveTriangles, g_bvh, g_frameMeshData, g_emissives, 
                g_sampleSets, g_aliasTable, g_frame.EmissiveMapsDescHeapOffset, rngThread);
// NEE_MIS_ALL_BOUNCES
#endif
        }

// !NEE_USE_MIS
#else

#if defined(USE_LVG)
        const int numSamples = bounce == 0 ? 2 : 1;
        return NEE_Emissive_LVG(pos, hitInfo.normal, surface, sampleSetIdx, 
            sampleSetSize, g_frame.NumEmissiveTriangles, numSamples, g_frame.CurrView, 
            gridDim, 
            extents, 
            offset_y, g_bvh, g_frameMeshData, g_emissives, g_sampleSets, 
            g_aliasTable, g_lvg, g_frame.EmissiveMapsDescHeapOffset, rngThread);
#else
        return NEE_Emissive(pos, hitInfo.normal, surface, sampleSetIdx, sampleSetSize, 
            g_frame.NumEmissiveTriangles, g_bvh, g_frameMeshData, g_emissives, 
            g_sampleSets, g_aliasTable, g_frame.EmissiveMapsDescHeapOffset, rngThread);
#endif

// NEE_USE_MIS
#endif

// NEE_EMISSIVE
#endif
    }
}