#ifndef RESTIR_DI_RESAMPLING_H
#define RESTIR_DI_RESAMPLING_H

#include "Params.hlsli"
#include "PairwiseMIS.hlsli"
#include "../../Common/FrameConstants.h"
#include "../../Common/Common.hlsli"
#include "../../Common/GBuffers.hlsli"

namespace RDI_Util
{
    struct TemporalCandidate
    {
        static TemporalCandidate Init()
        {
            TemporalCandidate ret;
            ret.valid = false;
            ret.lightIdx = UINT32_MAX;
        
            return ret;
        }

        BSDF::ShadingData surface;
        float3 pos;
        float3 normal;
        int16_t2 posSS;
        uint lightIdx;
        bool valid;
    };

    bool PlaneHeuristic(float3 samplePos, float3 currNormal, float3 currPos, float linearDepth,
        float tolerance = MAX_PLANE_DIST_REUSE)
    {
        float planeDist = dot(currNormal, samplePos - currPos);
        bool weight = abs(planeDist) <= tolerance * linearDepth;

        return weight;
    }

    TemporalCandidate FindTemporalCandidate(uint2 DTid, float3 pos, float3 normal, float z_view, 
        float roughness, BSDF::ShadingData surface, float2 prevUV, ConstantBuffer<cbFrameConstants> g_frame, 
        inout RNG rng)
    {
        TemporalCandidate candidate = RDI_Util::TemporalCandidate::Init();

        if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
            return candidate;

        const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
        const int2 prevPixel = prevUV * renderDim;

        GBUFFER_METALLIC_ROUGHNESS g_prevMR = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];
        const float2 prevMR = g_prevMR[prevPixel];
        GBuffer::Flags prevFlags = GBuffer::DecodeMetallic(prevMR.x);

        if(prevFlags.invalid || 
            prevFlags.emissive || 
            (abs(prevMR.y - roughness) > MAX_ROUGHNESS_DIFF_REUSE) || 
            (prevFlags.metallic != surface.metallic) ||
            (prevFlags.transmissive != surface.specTr))
            return candidate;

        float2 prevLensSample = 0;
        float3 prevOrigin = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, 
            g_frame.PrevViewInv._m23);
        if(g_frame.DoF)
        {
            RNG rngDoF = RNG::Init(RNG::PCG3d(prevPixel.xyx).zy, g_frame.FrameNum - 1);
            prevLensSample = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
            prevLensSample *= g_frame.LensRadius;
        }

        GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
            GBUFFER_OFFSET::DEPTH];
        const float prevViewDepth = g_prevDepth[prevPixel];
        const float3 prevPos = Math::WorldPosFromScreenSpace2(prevPixel, renderDim, prevViewDepth, 
            g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevCameraJitter, 
            g_frame.PrevView[0].xyz, g_frame.PrevView[1].xyz, g_frame.PrevView[2].xyz, 
            g_frame.DoF, prevLensSample, g_frame.FocusDepth, prevOrigin);

        if(!RDI_Util::PlaneHeuristic(prevPos, normal, pos, prevViewDepth, 0.01))
            return candidate;

        GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
            GBUFFER_OFFSET::NORMAL];
        const float3 prevNormal = Math::DecodeUnitVector(g_prevNormal[prevPixel]);
            
        float prevEta_next = DEFAULT_ETA_MAT;

        if(prevFlags.transmissive)
        {
            GBUFFER_IOR g_prevIOR = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
                GBUFFER_OFFSET::IOR];

            float ior = g_prevIOR[prevPixel];
            prevEta_next = GBuffer::DecodeIOR(ior);
        }

        GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::BASE_COLOR];
        const float4 prevBaseColor = prevFlags.subsurface ? g_prevBaseColor[prevPixel] :
            float4(g_prevBaseColor[prevPixel].rgb, 0);

        float prev_coat_weight = 0;
        float3 prev_coat_color = 0.0f;
        float prev_coat_roughness = 0;
        float prev_coat_ior = DEFAULT_ETA_COAT;

        if(prevFlags.coated)
        {
            GBUFFER_COAT g_coat = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
                GBUFFER_OFFSET::COAT];
            uint3 packed = g_coat[prevPixel].xyz;

            GBuffer::Coat coat = GBuffer::UnpackCoat(packed);
            prev_coat_weight = coat.weight;
            prev_coat_color = coat.color;
            prev_coat_roughness = coat.roughness;
            prev_coat_ior = coat.ior;
        }

        const float3 wo = normalize(prevOrigin - prevPos);
        candidate.surface = BSDF::ShadingData::Init(prevNormal, wo, prevFlags.metallic, 
            prevMR.y, prevBaseColor.rgb, ETA_AIR, prevEta_next, prevFlags.transmissive, 
            prevFlags.trDepthGt0, (half)prevBaseColor.w, prev_coat_weight, prev_coat_color, 
            prev_coat_roughness, prev_coat_ior);
        candidate.posSS = (int16_t2)prevPixel;
        candidate.pos = prevPos;
        candidate.normal = prevNormal;
        candidate.valid = true;

        return candidate;
    }

    float TargetLumAtTemporalPixel(float3 le, float3 lightPos, float3 lightNormal, uint lightID, 
        TemporalCandidate candidate, ConstantBuffer<cbFrameConstants> g_frame,
        RaytracingAccelerationStructure g_bvh)
    {
        const float t = length(lightPos - candidate.pos);
        const float3 wi = (lightPos - candidate.pos) / t;
        candidate.surface.SetWi(wi, candidate.normal);

        float cosThetaPrime = saturate(dot(lightNormal, -wi));
        cosThetaPrime = dot(lightNormal, lightNormal) == 0 ? 1 : cosThetaPrime;
        const float dwdA = cosThetaPrime / max(t * t, 1e-6f);

        const float3 targetAtPrev = le * BSDF::Unified(candidate.surface).f * dwdA;
        float targetLumAtPrev = Math::Luminance(targetAtPrev);

        // should use previous frame's bvh
        targetLumAtPrev *= RtRayQuery::Visibility_Segment(candidate.pos, wi, t, candidate.normal, 
            lightID, g_bvh, candidate.surface.Transmissive());

        return targetLumAtPrev;
    }

    float3 TargetAtCurrentPixel(float3 le, float3 pos, float3 normal, BSDF::ShadingData surface, 
        RaytracingAccelerationStructure g_bvh, inout EmissiveData prevEmissive, out float dwdA)
    {
        prevEmissive.SetSurfacePos(pos);
        dwdA = prevEmissive.dWdA();

        surface.SetWi(prevEmissive.wi, normal);
        float3 target = le * BSDF::Unified(surface).f * dwdA;
        if(dot(target, target) > 0)
        {
            target *= RtRayQuery::Visibility_Segment(pos, prevEmissive.wi, prevEmissive.t, normal, 
                prevEmissive.ID, g_bvh, surface.Transmissive());
        }

        return target;
    }

    void TemporalResample1(float3 pos, float3 normal, float roughness, float z_view, 
        BSDF::ShadingData surface, uint prevReservoir_A_DescHeapIdx, uint prevReservoir_B_DescHeapIdx, 
        TemporalCandidate candidate, ConstantBuffer<cbFrameConstants> g_frame, 
        StructuredBuffer<RT::EmissiveTriangle> g_emissives, 
        RaytracingAccelerationStructure g_bvh, inout Reservoir r, inout RNG rng, 
        inout Target target)
    {
        const half prevM = PartialReadReservoir_M(candidate.posSS, prevReservoir_A_DescHeapIdx);
        const half newM = r.M + prevM;

        if(r.w_sum != 0)
        {
            float targetLumAtPrev = 0.0f;

            if(dot(r.Le, r.Le) > 0)
            {
                targetLumAtPrev = TargetLumAtTemporalPixel(r.Le, target.lightPos, 
                    target.lightNormal, target.lightID, candidate, g_frame, g_bvh);
            }

            const float p_curr = (float)r.M * Math::Luminance(target.p_hat);
            const float denom = p_curr + (float)prevM * targetLumAtPrev;
            const float m_curr = denom > 0 ? p_curr / denom : 0;
            r.w_sum *= m_curr;
        }

        if(candidate.lightIdx != UINT32_MAX)
        {
            Reservoir prev = PartialReadReservoir_ReuseRest(candidate.posSS,
                prevReservoir_A_DescHeapIdx, 
                candidate.lightIdx);

            // compute target at current pixel with previous reservoir's sample
            EmissiveData prevEmissive = EmissiveData::Init(prev, g_emissives);
            float dwdA;
            const float3 currTarget = TargetAtCurrentPixel(prev.Le, pos, normal, surface, 
                g_bvh, prevEmissive, dwdA);
            const float targetLumAtCurr = Math::Luminance(currTarget);

            // w_prev becomes zero; then only M needs to be updated, which is done at the end anyway
            if(targetLumAtCurr > 0)
            {
                const float w_sum_prev = PartialReadReservoir_WSum(candidate.posSS, prevReservoir_B_DescHeapIdx);
                const float targetLumAtPrev = prev.W > 0 ? w_sum_prev / prev.W : 0;
                // balance heuristic
                const float p_prev = (float)prev.M * targetLumAtPrev;
                const float denom = p_prev + (float)r.M * targetLumAtCurr;
                const float m_prev = denom > 0 ? p_prev / denom : 0;
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
        float z_view, float roughness, BSDF::ShadingData surface, uint prevReservoir_A_DescHeapIdx, 
        uint prevReservoir_B_DescHeapIdx, ConstantBuffer<cbFrameConstants> g_frame, 
        StructuredBuffer<RT::EmissiveTriangle> g_emissives, RaytracingAccelerationStructure g_bvh, 
        inout Reservoir r, inout Target target, inout RNG rng)
    {
        static const half2 k_hammersley[8] =
        {
            half2(0.0, -0.7777777777777778),
            half2(-0.5, -0.5555555555555556),
            half2(0.5, -0.33333333333333337),
            half2(-0.75, -0.11111111111111116),
            half2(0.25, 0.11111111111111116),
            half2(-0.25, 0.33333333333333326),
            half2(0.75, 0.5555555555555556),
            half2(-0.875, 0.7777777777777777)
        };

        GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
            GBUFFER_OFFSET::NORMAL];
        GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
            GBUFFER_OFFSET::DEPTH];
        GBUFFER_METALLIC_ROUGHNESS g_prevMR = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];
        GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::BASE_COLOR];
        GBUFFER_IOR g_prevIOR = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::IOR];

        // rotate sample sequence per pixel
        const float u0 = rng.Uniform();
        const int offset = (int)(rng.UniformUintBounded_Faster(8));
        const float theta = u0 * TWO_PI;
        const float sinTheta = sin(theta);
        const float cosTheta = cos(theta);

        const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
        PairwiseMIS pairwiseMIS = PairwiseMIS::Init((uint16_t)numSamples, r);

        float3 samplePos[MAX_NUM_SPATIAL_SAMPLES];
        float3 sampleOrigin[MAX_NUM_SPATIAL_SAMPLES];
        int16_t2 samplePosSS[MAX_NUM_SPATIAL_SAMPLES];
        uint sampleLightIdx[MAX_NUM_SPATIAL_SAMPLES];
        bool sampleMetallic[MAX_NUM_SPATIAL_SAMPLES];
        float sampleRoughness[MAX_NUM_SPATIAL_SAMPLES];
        bool sampleTr[MAX_NUM_SPATIAL_SAMPLES];
        bool sampleSubsurf[MAX_NUM_SPATIAL_SAMPLES];
        bool sampleCoated[MAX_NUM_SPATIAL_SAMPLES];
        uint16_t k = 0;
        const float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, 
            g_frame.PrevViewInv._m23);

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
                const float2 mr_i = g_prevMR[posSS_i];
                GBuffer::Flags flags_i = GBuffer::DecodeMetallic(mr_i.x);

                if(flags_i.invalid || flags_i.emissive)
                    continue;

                const float depth_i = g_prevDepth[posSS_i];
                float2 lensSample = 0;
                float3 origin_i = prevCameraPos;
                if(g_frame.DoF)
                {
                    RNG rngDoF = RNG::Init(RNG::PCG3d(posSS_i.xyx).zy, g_frame.FrameNum - 1);
                    lensSample = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
                    lensSample *= g_frame.LensRadius;
                }

                float3 pos_i = Math::WorldPosFromScreenSpace2(posSS_i, renderDim, depth_i, 
                    g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevCameraJitter, 
                    g_frame.PrevView[0].xyz, g_frame.PrevView[1].xyz, g_frame.PrevView[2].xyz, 
                    g_frame.DoF, lensSample, g_frame.FocusDepth, origin_i);

                bool valid = PlaneHeuristic(pos_i, normal, pos, z_view);
                valid = valid && (abs(mr_i.y - roughness) < MAX_ROUGHNESS_DIFF_REUSE);

                if (!valid)
                    continue;

                const uint lightIdx_i = PartialReadReservoir_ReuseLightIdx(posSS_i, prevReservoir_B_DescHeapIdx);

                samplePos[k] = pos_i;
                sampleOrigin[k] = origin_i;
                samplePosSS[k] = (int16_t2)posSS_i;
                sampleLightIdx[k] = lightIdx_i;
                sampleMetallic[k] = flags_i.metallic;
                sampleRoughness[k] = mr_i.y;
                sampleTr[k] = flags_i.transmissive;
                sampleSubsurf[k] = flags_i.subsurface;
                sampleCoated[k] = flags_i.coated;

                k++;
            }
        }

        pairwiseMIS.k = k;

        for (int i = 0; i < k; i++)
        {
            const float3 sampleNormal = Math::DecodeUnitVector(g_prevNormal[samplePosSS[i]]);
            const float4 sampleBaseColor = sampleSubsurf[i] ? g_prevBaseColor[samplePosSS[i]] :
                float4(g_prevBaseColor[samplePosSS[i]].rgb, 0);

            float sampleEta_next = DEFAULT_ETA_MAT;

            if(sampleTr[i])
            {
                float ior = g_prevIOR[samplePosSS[i]];
                sampleEta_next = GBuffer::DecodeIOR(ior);
            }

            float sample_coat_weight = 0;
            float3 sample_coat_color = 0.0f;
            float sample_coat_roughness = 0;
            float sample_coat_ior = DEFAULT_ETA_COAT;

            if(sampleCoated[i])
            {
                GBUFFER_COAT g_coat = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
                    GBUFFER_OFFSET::COAT];
                uint3 packed = g_coat[samplePosSS[i]].xyz;

                GBuffer::Coat coat = GBuffer::UnpackCoat(packed);
                sample_coat_weight = coat.weight;
                sample_coat_color = coat.color;
                sample_coat_roughness = coat.roughness;
                sample_coat_ior = coat.ior;
            }

            const float3 wo_i = normalize(sampleOrigin[i] - samplePos[i]);
            BSDF::ShadingData surface_i = BSDF::ShadingData::Init(sampleNormal, wo_i,
                sampleMetallic[i], sampleRoughness[i], sampleBaseColor.xyz, ETA_AIR,
                sampleEta_next, sampleTr[i], false, (half)sampleBaseColor.w, sample_coat_weight, 
                sample_coat_color, sample_coat_roughness, sample_coat_ior);

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