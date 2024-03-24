#ifndef RESTIR_GI_PATH_TRACING_H
#define RESTIR_GI_PATH_TRACING_H

#include "../IndirectLighting_Common.h"
#include "../Params.hlsli"
#include "../PathTracing.hlsli"
#include "../NEE.hlsli"

namespace RGI_Util
{
    float3 PathTrace(float3 pos, float3 normal, float3 wi, float ior, int maxNumBounces, 
        uint sampleSetIdx, ReSTIR_RT::HitSurface hitInfo, RT::RayCone rayCone, 
        RaytracingAccelerationStructure g_bvh, ConstantBuffer<cbFrameConstants> g_frame, 
        ConstantBuffer<cb_ReSTIR_GI_SpatioTemporal> g_local,
        StructuredBuffer<RT::MeshInstance> g_frameMeshData, StructuredBuffer<Vertex> g_vertices, 
        StructuredBuffer<uint> g_indices, StructuredBuffer<Material> g_materials, 
        StructuredBuffer<RT::EmissiveTriangle> g_emissives,
        StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets, 
        StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable, 
        StructuredBuffer<RT::VoxelSample> g_lvg,
        inout RNG rngThread, inout RNG rngGroup)
    {
        float3 li = 0.0;
        float3 throughput = 1.0f;
        float eta_curr = dot(normal, wi) < 0 ? ior : 1.0f;
        int bounce = 0;

        // Having found the second path vertex, do the path tracing loop to estimate radiance
        // reflected from it towards primary vertex
        do
        {
            float3 hitPos = pos + hitInfo.t * wi;
            BSDF::ShadingData surface;
            float eta;

            rayCone.NewHit(hitInfo.t);

            if(!ReSTIR_Util::GetMaterialData(-wi, g_materials, g_frame, eta_curr, rayCone, hitInfo, surface, eta))
                break;

            // Next event estimation
            li += throughput * ReSTIR_Util::NEE(hitPos, hitInfo, surface, sampleSetIdx, bounce, 
                g_bvh, g_frame, g_local, g_frameMeshData, g_emissives, g_sampleSets, g_aliasTable, 
                g_lvg, rngThread, rngGroup);

            // Skip the remaining code as it won't affect li
            if(bounce >= (maxNumBounces - 1))
                break;

            // Update path vertex
            float3 prevPos = pos;
            pos = hitPos;
            normal = hitInfo.normal;
            bounce++;

            // Russian Roulette
            if(IS_CB_FLAG_SET(CB_IND_FLAGS::RUSSIAN_ROULETTE) && (bounce >= MIN_NUM_BOUNCES_RUSSIAN_ROULETTE))
            {
                // Test against maximum throughput across the wave
                float waveThroughput = WaveActiveMax(Math::Luminance(throughput));

                float p_terminate = max(0.05, 1 - waveThroughput);
                if(rngGroup.Uniform() < p_terminate)
                    break;
                
                throughput /= (1 - p_terminate);
            }

            // w.r.t. solid angle
            float3 bsdfDivPdf = 0;

            if(surface.HasSpecularTransmission() && bounce < g_local.MaxTransmissionBounces)
            {
                // if(bounce >= g_local.MaxGlossyBounces)
                //     wi = ReSTIR_Util::SampleDielectricBTDF(normal, surface, rngThread, bsdfDivPdf);
                // else
                    wi = ReSTIR_Util::SampleDielectricBSDF_RIS<false>(normal, surface, rngThread, bsdfDivPdf);
            }
            else if(bounce >= g_local.MaxDiffuseBounces && bounce < g_local.MaxGlossyBounces)
                wi = ReSTIR_Util::SampleBRDF_NoDiffuse(normal, surface, rngThread, bsdfDivPdf);
            else if(bounce < g_local.MaxDiffuseBounces)
                wi = ReSTIR_Util::SampleBRDF<false>(normal, surface, rngThread, bsdfDivPdf);

            // Terminate early as extending this path won't contribute to incident radiance
            if(Math::Luminance(bsdfDivPdf) < 1e-6)
                break;

            // Trace a ray to find next path vertex
            bool transmitted = dot(normal, wi) < 0;
            uint unused;
            bool hit = ReSTIR_RT::FindClosestHit<false>(pos, normal, wi, g_bvh, g_frameMeshData, g_vertices, 
                g_indices, hitInfo, unused, surface.HasSpecularTransmission());

            if(!hit)
                break;

            throughput *= bsdfDivPdf;
            eta_curr = transmitted ? (eta_curr == 1.0f ? eta : 1.0f) : eta_curr;

            // Curvature is assumed to be zero after the first hit, so reflection doesn't 
            // change the cone spread angle. For transmission, skipping the update seems 
            // to have a negligible impact, but with a significant performance boost. Skip 
            // for now pending further investigation in the future.
#if 0
            if(transmitted)
            {
                rayCone.UpdateConeGeometry_Tr_CurvatureIs0(surface.wo, wi, normal, surface.eta, hitPos,
                    prevPos);
            }
#endif
        } while(bounce < maxNumBounces);

        return li;
    }
}

#endif