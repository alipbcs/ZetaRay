#ifndef RESTIR_DI_RESAMPLING_H
#define RESTIR_DI_RESAMPLING_H

#include "Params.hlsli"
#include "PairwiseMIS.hlsli"
#include "../../Common/FrameConstants.h"
#include "../../Common/GBuffers.hlsli"

namespace RDI_Util
{
    bool PlaneHeuristic(float3 samplePos, float3 currNormal, float3 currPos, float linearDepth)
    {
        float planeDist = dot(currNormal, samplePos - currPos);
        bool weight = abs(planeDist) <= MAX_PLANE_DIST_REUSE * linearDepth;

        return weight;
    }

    TemporalCandidate FindTemporalCandidates(uint2 DTid, float3 pos, float3 normal, float t_primary, 
        float roughness, float2 prevUV, ConstantBuffer<cbFrameConstants> g_frame, inout RNG rng)
    {
        TemporalCandidate candidate = RDI_Util::TemporalCandidate::Init();

        if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
            return candidate;

        GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
        GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
        GBUFFER_METALLIC_ROUGHNESS g_prevMetallicRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];

        const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
        const int2 prevPixel = prevUV * renderDim;

        for(int i = 0; i < NUM_TEMPORAL_SEARCH_ITER; i++)
        {
            const float theta = rng.Uniform() * TWO_PI;
            const float sinTheta = sin(theta);
            const float cosTheta = cos(theta);
            const int2 samplePosSS = prevPixel + (i > 0) * TEMPORAL_SEARCH_RADIUS * float2(sinTheta, cosTheta);

            if(samplePosSS.x >= renderDim.x || samplePosSS.y >= renderDim.y)
                continue;

            // plane-based heuristic
            float prevDepth = g_prevDepth[samplePosSS];
            const float3 prevPos = Math::WorldPosFromScreenSpace(samplePosSS, renderDim,
                prevDepth, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevViewInv, 
                g_frame.PrevCameraJitter);

            if(!RDI_Util::PlaneHeuristic(prevPos, normal, pos, t_primary))
                continue;

            const float2 prevMR = g_prevMetallicRoughness[samplePosSS];

            bool prevMetallic;
            bool prevEmissive;
            GBuffer::DecodeMetallicEmissive(prevMR.x, prevMetallic, prevEmissive);

            // skip invalid reservoirs
            if(prevEmissive)
                continue;

            // normal heuristic
            const float2 prevNormalEncoded = g_prevNormal[samplePosSS];
            const float3 prevNormal = Math::DecodeUnitVector(prevNormalEncoded);
            const float normalSimilarity = dot(prevNormal, normal);
            
            // roughness heuristic
            const bool roughnessSimilarity = abs(prevMR.y - roughness) < MAX_ROUGHNESS_DIFF_REUSE;

            candidate.valid = normalSimilarity >= MIN_NORMAL_SIMILARITY_REUSE && roughnessSimilarity;

            if(candidate.valid)
            {
                candidate.posSS = (int16_t2)samplePosSS;
                candidate.pos = prevPos;
                candidate.normal = prevNormal;
                candidate.metallic = prevMetallic;
                candidate.roughness = prevMR.y;

                break;
            }
        }

        return candidate;
    }

    float TargetLumAtTemporalPixel(float3 le, float3 lightPos, float3 lightNormal, uint lightID, 
        uint2 posSS, float3 prevPos, float3 prevNormal, bool prevMetallic, float prevRoughness, 
        float prevLinearDepth, ConstantBuffer<cbFrameConstants> g_frame,
        RaytracingAccelerationStructure g_bvh, out BSDF::ShadingData prevSurface)
    {
        GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::BASE_COLOR];

        const float3 prevBaseColor = g_prevBaseColor[posSS].rgb;
        const float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, 
            g_frame.PrevViewInv._m23);
        const float3 prevWo = normalize(prevCameraPos - prevPos);

        prevSurface = BSDF::ShadingData::Init(prevNormal, prevWo, prevMetallic, prevRoughness, prevBaseColor);

        const float t = length(lightPos - prevPos);
        const float3 wi = (lightPos - prevPos) / t;
        prevSurface.SetWi(wi, prevNormal);

        float cosThetaPrime = saturate(dot(lightNormal, -wi));
        cosThetaPrime = dot(lightNormal, lightNormal) == 0 ? 1 : cosThetaPrime;
        const float dwdA = cosThetaPrime / max(t * t, 1e-6f);

        const float3 targetAtPrev = le * BSDF::UnifiedBSDF(prevSurface) * dwdA;
        float targetLumAtPrev = Math::Luminance(targetAtPrev);

        // should use previous frame's bvh
        targetLumAtPrev *= VisibilityApproximate(g_bvh, prevPos, wi, t, prevNormal, lightID, 
            prevSurface.HasSpecularTransmission());

        return targetLumAtPrev;
    }

    float3 TargetAtCurrentPixel(float3 le, float3 pos, float3 normal, float linearDepth, BSDF::ShadingData surface, 
        RaytracingAccelerationStructure g_bvh, inout EmissiveData prevEmissive, out float dwdA)
    {
        prevEmissive.SetSurfacePos(pos);
        dwdA = prevEmissive.dWdA();

        surface.SetWi(prevEmissive.wi, normal);
        float3 target = le * BSDF::UnifiedBSDF(surface) * dwdA;    
        target *= VisibilityApproximate(g_bvh, pos, prevEmissive.wi, prevEmissive.t, normal, 
            prevEmissive.ID, surface.HasSpecularTransmission());

        return target;
    }

    void TemporalResample1(float3 pos, float3 normal, float roughness, float t_primary, BSDF::ShadingData surface, 
        uint PrevReservoir_A_DescHeapIdx, uint PrevReservoir_B_DescHeapIdx, 
        TemporalCandidate candidate, ConstantBuffer<cbFrameConstants> g_frame, 
        StructuredBuffer<RT::EmissiveTriangle> g_emissives, RaytracingAccelerationStructure g_bvh, 
        inout Reservoir r, inout RNG rng, inout Target target)
    {
        const half prevM = PartialReadReservoir_M(candidate.posSS, PrevReservoir_A_DescHeapIdx);
        const half newM = r.M + prevM;

        if(r.w_sum != 0)
        {
            float targetLumAtPrev = 0.0f;

            if(Math::Luminance(r.Le) > 1e-6)
            {
                BSDF::ShadingData prevSurface;
                targetLumAtPrev = TargetLumAtTemporalPixel(r.Le, target.lightPos, 
                    target.lightNormal, target.lightID, candidate.posSS, candidate.pos, 
                    candidate.normal, candidate.metallic, candidate.roughness, 
                    t_primary, g_frame, g_bvh, prevSurface);
            }

            const float p_curr = (float)r.M * Math::Luminance(target.p_hat);
            const float m_curr = p_curr / max(p_curr + (float)prevM * targetLumAtPrev, 1e-6);
            r.w_sum *= m_curr;
        }

        if(candidate.lightIdx != UINT32_MAX)
        {
            Reservoir prev = PartialReadReservoir_ReuseRest(candidate.posSS,
                PrevReservoir_A_DescHeapIdx, 
                candidate.lightIdx);

            // compute target at current pixel with previous reservoir's sample
            EmissiveData prevEmissive = EmissiveData::Init(prev, g_emissives);
            float dwdA;
            const float3 currTarget = TargetAtCurrentPixel(prev.Le, pos, normal, t_primary, surface, 
                g_bvh, prevEmissive, dwdA);
            const float targetLumAtCurr = Math::Luminance(currTarget);

            // w_prev becomes zero; then only M needs to be updated, which is done at the end anyway
            if(targetLumAtCurr > 1e-6)
            {
                const float w_sum_prev = PartialReadReservoir_WSum(candidate.posSS, PrevReservoir_B_DescHeapIdx);
                const float targetLumAtPrev = prev.W > 0 ? w_sum_prev / prev.W : 0;
                // balance heuristic
                const float p_prev = (float)prev.M * targetLumAtPrev;
                const float m_prev = p_prev / max(p_prev + (float)r.M * targetLumAtCurr, 1e-6);
                const float w_prev = m_prev * targetLumAtCurr * prev.W;

                if(r.Update(w_prev, prev.Le, prev.LightIdx, prev.Bary, rng))
                {
                    target.p_hat = currTarget;
                    target.rayT = prevEmissive.t;
                    target.lightID = prevEmissive.ID;
                    target.wi = prevEmissive.wi;
                    target.lightNormal = prevEmissive.lightNormal;
                    target.lightPos = prevEmissive.lightPos;
                    target.dwdA = dwdA;
                }
            }
        }

        float targetLum = Math::Luminance(target.p_hat);
        r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;
        r.M = newM;
    }

    void SpatialResample(uint2 DTid, int numSamples, float radius, float3 pos, float3 normal, 
        float t_primary, float roughness, BSDF::ShadingData surface, uint prevReservoir_A_DescHeapIdx, 
        uint prevReservoir_B_DescHeapIdx, ConstantBuffer<cbFrameConstants> g_frame, 
        StructuredBuffer<RT::EmissiveTriangle> g_emissives, RaytracingAccelerationStructure g_bvh, 
        inout Reservoir r, inout Target target, inout RNG rng)
    {
        static const float2 k_hammersley[8] =
        {
            float2(0.0, -0.7777777777777778),
            float2(-0.5, -0.5555555555555556),
            float2(0.5, -0.33333333333333337),
            float2(-0.75, -0.11111111111111116),
            float2(0.25, 0.11111111111111116),
            float2(-0.25, 0.33333333333333326),
            float2(0.75, 0.5555555555555556),
            float2(-0.875, 0.7777777777777777)
        };

        GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
        GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
        GBUFFER_METALLIC_ROUGHNESS g_prevMetallicRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];
        GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::BASE_COLOR];

        // rotate sample sequence per pixel
        const float u0 = rng.Uniform();
        const int offset = (int)(rng.UniformUintBounded_Faster(8));
        const float theta = u0 * TWO_PI;
        const float sinTheta = sin(theta);
        const float cosTheta = cos(theta);

        const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
        PairwiseMIS pairwiseMIS = PairwiseMIS::Init((uint16_t)numSamples, r);

        float3 samplePos[4];
        int16_t2 samplePosSS[4];
        uint sampleLightIdx[4];
        float sampleRoughness[4];
        bool sampleMetallic[4];
        uint16_t k = 0;

        [loop]
        for (int i = 0; i < numSamples; i++)
        {
            float2 sampleUV = k_hammersley[(offset + i) & 7];
            float2 rotated;
            rotated.x = dot(sampleUV, float2(cosTheta, -sinTheta));
            rotated.y = dot(sampleUV, float2(sinTheta, cosTheta));
            rotated *= radius;
            const uint2 posSS_i = (uint2)round(float2(DTid) + rotated);

            if (Math::IsWithinBounds(posSS_i, renderDim))
            {
                const float t_i = g_prevDepth[posSS_i];

                if (t_i == FLT_MAX)
                    continue;

                float3 pos_i = Math::WorldPosFromScreenSpace(posSS_i, renderDim,
                    t_i, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevViewInv, 
                    g_frame.PrevCameraJitter);
                bool valid = PlaneHeuristic(pos_i, normal, pos, t_primary);

                const float2 mr_i = g_prevMetallicRoughness[posSS_i];
            
                bool metallic_i;
                bool emissive_i;
                GBuffer::DecodeMetallicEmissive(mr_i.x, metallic_i, emissive_i);

                // TODO ignoring this causes strange frame time spikes when sky is visible, due to specular brdf evaluation
                valid = valid && !emissive_i && (abs(mr_i.y - roughness) < MAX_ROUGHNESS_DIFF_REUSE);

                if (!valid)
                    continue;

                const uint lightIdx_i = PartialReadReservoir_ReuseLightIdx(posSS_i, prevReservoir_B_DescHeapIdx);

                samplePos[k] = pos_i;
                samplePosSS[k] = (int16_t2)posSS_i;
                sampleLightIdx[k] = lightIdx_i;
                sampleMetallic[k] = metallic_i;
                sampleRoughness[k] = mr_i.y;
                k++;
            }
        }

        const float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, g_frame.PrevViewInv._m23);
        pairwiseMIS.k = k;

        for (int i = 0; i < k; i++)
        {
            const float3 sampleNormal = Math::DecodeUnitVector(g_prevNormal[samplePosSS[i]]);
            const float3 sampleBaseColor = g_prevBaseColor[samplePosSS[i]].rgb;

            const float3 wo_i = normalize(prevCameraPos - samplePos[i]);
            BSDF::ShadingData surface_i = BSDF::ShadingData::Init(sampleNormal, wo_i,
                sampleMetallic[i], sampleRoughness[i], sampleBaseColor);

            Reservoir neighbor = PartialReadReservoir_ReuseRest(samplePosSS[i],
                prevReservoir_A_DescHeapIdx,
                sampleLightIdx[i]);

            const float neighborWSum = PartialReadReservoir_WSum(samplePosSS[i], prevReservoir_B_DescHeapIdx);

            pairwiseMIS.Stream(r, pos, normal, surface, neighbor, samplePos[i], sampleNormal, 
                neighborWSum, surface_i, g_emissives, g_bvh, target, rng);
        }

        pairwiseMIS.End(r, target, rng);
        r = pairwiseMIS.r_s;
    }
}

#endif