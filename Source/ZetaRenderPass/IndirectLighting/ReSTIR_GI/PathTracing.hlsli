#ifndef RESTIR_GI_PATH_TRACING_H
#define RESTIR_GI_PATH_TRACING_H

#include "../IndirectLighting_Common.h"
#include "../../Common/BSDFSampling.hlsli"
#include "ReSTIR_GI_NEE.hlsli"

namespace ReSTIR_RT
{
    float3 PathTrace(float3 pos, float3 normal, float ior, uint sampleSetIdx, 
        BSDF::BSDFSample bsdfSample, ReSTIR_RT::Hit hitInfo, RT::RayDifferentials rd,
        ConstantBuffer<cbFrameConstants> g_frame, ReSTIR_Util::Globals globals,
        SamplerState samp, inout RNG rngThread, inout RNG rngGroup)
    {
        float3 li = 0.0;
        float3 throughput = 1.0f;
        float eta_curr = dot(normal, bsdfSample.wi) < 0 ? ior : ETA_AIR;
        int bounce = 0;

        while(true)
        {
            float3 hitPos = mad(hitInfo.t, bsdfSample.wi, pos);
            float3 dpdx;
            float3 dpdy;
            rd.dpdx_dpdy(hitPos, hitInfo.normal, dpdx, dpdy);
            rd.ComputeUVDifferentials(dpdx, dpdy, hitInfo.triDiffs.dpdu, hitInfo.triDiffs.dpdv);

            BSDF::ShadingData surface;
            float eta_mat;
            if(!ReSTIR_RT::GetMaterialData(-bsdfSample.wi, globals.materials, g_frame, eta_curr, 
                rd.uv_grads, hitInfo, surface, eta_mat, samp))
            {
                break;
            }

            // Next event estimation
            li += throughput * RGI_Util::NEE(hitPos, hitInfo.normal, surface, sampleSetIdx, bounce, 
                g_frame, globals, rngThread).ld;

            // Skip the remaining code as it won't affect li
            if(bounce >= (globals.maxNumBounces - 1))
                break;

            // Update path vertex
            float3 prevPos = pos;
            pos = hitPos;
            normal = hitInfo.normal;
            bounce++;

            // Russian Roulette
            if(globals.russianRoulette && (bounce >= MIN_NUM_BOUNCES_RUSSIAN_ROULETTE))
            {
                // Test against maximum throughput across the wave
                float waveThroughput = WaveActiveMax(Math::Luminance(throughput));

                float p_terminate = max(0.05, 1 - waveThroughput);
                if(rngGroup.Uniform() < p_terminate)
                    break;
                
                throughput /= (1 - p_terminate);
            }

            bsdfSample = BSDF::BSDFSample::Init();
            bool sampleNonDiffuse = (bounce < globals.maxGlossyBounces_NonTr) ||
                (surface.HasSpecularTransmission() && (bounce < globals.maxGlossyBounces_Tr));

            if(bounce < globals.maxDiffuseBounces)
                bsdfSample = BSDF::SampleBSDF(normal, surface, rngThread);
            else if(sampleNonDiffuse)
                bsdfSample = BSDF::SampleBSDF_NoDiffuse(normal, surface, rngThread);

            // Terminate early as extending this path won't contribute to incident radiance
            if(Math::Luminance(bsdfSample.bsdfOverPdf) < 1e-6)
                break;

            // Trace a ray to find next path vertex
            hitInfo = ReSTIR_RT::Hit::FindClosest<false>(pos, normal, bsdfSample.wi, globals.bvh, 
                globals.frameMeshData, globals.vertices, globals.indices, 
                surface.HasSpecularTransmission());

            if(!hitInfo.hit)
                break;

            throughput *= bsdfSample.bsdfOverPdf;
            bool transmitted = dot(normal, bsdfSample.wi) < 0;
            eta_curr = transmitted ? (eta_curr == 1.0f ? eta_mat : 1.0f) : eta_curr;

            rd.UpdateRays(pos, normal, bsdfSample.wi, surface.wo, hitInfo.triDiffs, 
                dpdx, dpdy, transmitted, surface.eta);
        }

        return li;
    }
}

#endif