#ifndef SKY_DI_RESAMPLING_H
#define SKY_DI_RESAMPLING_H

#include "Params.hlsli"
#include "PairwiseMIS.hlsli"
#include "../../Common/GBuffers.hlsli"
#include "../../Common/BSDFSampling.hlsli"

namespace SkyDI_Util
{
    struct TemporalCandidate
    {
        static TemporalCandidate Init()
        {
            TemporalCandidate ret;
            ret.valid = false;

            return ret;
        }

        BSDF::ShadingData surface;
        float3 pos;
        float3 normal;
        int16_t2 posSS;
        bool valid;
    };

    bool PlaneHeuristic(float3 samplePos, float3 currNormal, float3 currPos, float z_view,
        float tolerance = MAX_PLANE_DIST_REUSE)
    {
        float planeDist = dot(currNormal, samplePos - currPos);
        bool weight = abs(planeDist) <= tolerance * z_view;

        return weight;
    }

    TemporalCandidate FindTemporalCandidate(uint2 DTid, float3 pos, float3 normal, float z_view, 
        float roughness, BSDF::ShadingData surface, ConstantBuffer<cbFrameConstants> g_frame)
    {
        TemporalCandidate candidate = TemporalCandidate::Init();

        GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::MOTION_VECTOR];
        const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
        const float2 motionVec = g_motionVector[DTid];
        const float2 currUV = (DTid + 0.5f) / renderDim;
        const float2 prevUV = currUV - motionVec;
        const int2 prevPixel = prevUV * renderDim;

        if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
            return candidate;

        GBUFFER_METALLIC_ROUGHNESS g_prevMR = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];
        const float2 prevMR = g_prevMR[prevPixel];
        GBuffer::Flags prevFlags = GBuffer::DecodeMetallic(prevMR.x);

        // Skip if not on the same surface
        if(prevFlags.invalid || 
            prevFlags.emissive || 
            (abs(prevMR.y - roughness) > 0.3) || 
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

        // plane-based heuristic
        GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
            GBUFFER_OFFSET::DEPTH];
        const float prevViewDepth = g_prevDepth[prevPixel];
        const float3 prevPos = Math::WorldPosFromScreenSpace2(prevPixel, renderDim, prevViewDepth, 
            g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevCameraJitter, 
            g_frame.PrevView[0].xyz, g_frame.PrevView[1].xyz, g_frame.PrevView[2].xyz, 
            g_frame.DoF, prevLensSample, g_frame.FocusDepth, prevOrigin);

        if(!PlaneHeuristic(prevPos, normal, pos, z_view, MAX_PLANE_DIST_REUSE))
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

    void TemporalResample(TemporalCandidate candidate, float3 pos, float3 normal,
        BSDF::ShadingData surface, uint prevReservoir_A_DescHeapIdx, uint prevReservoir_B_DescHeapIdx, 
        uint prevReservoir_C_DescHeapIdx, float alpha_min, RaytracingAccelerationStructure g_bvh,
        RaytracingAccelerationStructure g_bvh_prev, ConstantBuffer<cbFrameConstants> g_frame, 
        inout Reservoir r, inout RNG rng)
    {
        Reservoir r_prev = Reservoir::Load(candidate.posSS, 
            prevReservoir_A_DescHeapIdx, prevReservoir_B_DescHeapIdx,
            prevReservoir_C_DescHeapIdx);

        // Lower confidence due to visibility change
        r_prev.M = (r.lightType == Light::TYPE::SUN) && g_frame.SunMoved ? (uint16)0 : r_prev.M;
        const uint16 newM = r.M + r_prev.M;

        // Shift from current pixel to temporal
        if(r.w_sum != 0)
        {
            float targetLum_prev = 0;
            float3 wi_offset = r.wx; 
            float jacobian = 1;

            if(IsShiftInvertible(r, candidate.surface, alpha_min))
            {
                if(r.halfVectorCopyShift)
                {
                    float3 wh_local = r.wx;
                    float3 wh_t = FromTangentFrameToWorld(candidate.normal, wh_local);

                    wi_offset = reflect(-candidate.surface.wo, wh_t);
                    jacobian = r.partialJacobian == 0 ? 1 : 
                        abs(dot(candidate.surface.wo, wh_t)) / r.partialJacobian;
                }

                candidate.surface.SetWi(wi_offset, candidate.normal);

                const float3 le = r.lightType == Light::TYPE::SKY ? 
                    Light::Le_Sky(wi_offset, g_frame.EnvMapDescHeapOffset) :
                    Light::Le_Sun(candidate.pos, g_frame);
                const float3 target_prev = le * BSDF::Unified(candidate.surface).f;
                targetLum_prev = Math::Luminance(target_prev);

                if(targetLum_prev > 0)
                {
                    targetLum_prev *= RtRayQuery::Visibility_Ray(candidate.pos, wi_offset, candidate.normal, 
                        g_bvh_prev, candidate.surface.Transmissive());
                }
            }

            const float p_curr = r.M * Math::Luminance(r.target);
            float numerator = p_curr;
            float denom = numerator + r_prev.M * targetLum_prev * jacobian;
            const float m_curr = denom > 0 ? numerator / denom : 0;
            r.w_sum *= m_curr;
        }

        // Shift from temporal to current
        if(r_prev.IsValid() && r_prev.M > 0)
        {
            float3 wi_offset = r_prev.wx;
            float jacobian = 1;
            float3 target_curr = 0;

            if(IsShiftInvertible(r_prev, surface, alpha_min))
            {
                if(r_prev.halfVectorCopyShift)
                {
                    float3 wh_local = r_prev.wx;
                    float3 wh_c = FromTangentFrameToWorld(normal, wh_local);
                    float3 wh_t = FromTangentFrameToWorld(candidate.normal, wh_local);

                    wi_offset = reflect(-surface.wo, wh_c);
                    float whdotwo_t = abs(dot(candidate.surface.wo, wh_t));
                    jacobian = whdotwo_t > 0 ? abs(dot(surface.wo, wh_c)) / whdotwo_t : 1;
                }

                surface.SetWi(wi_offset, normal);

                const float3 le = r_prev.lightType == Light::TYPE::SKY ? 
                    Light::Le_Sky(wi_offset, g_frame.EnvMapDescHeapOffset) :
                    Light::Le_Sun(pos, g_frame);
                target_curr = le * BSDF::Unified(surface).f;
            }
            
            // w_prev becomes zero and only M needs to be updated, which is done at the end anyway
            if(dot(target_curr, target_curr) > 0)
            {
                if(RtRayQuery::Visibility_Ray(pos, wi_offset, normal, g_bvh, surface.Transmissive()))
                {
                    const float targetLum_curr = Math::Luminance(target_curr);
                    const float targetLum_prev = r_prev.W > 0 ? r_prev.w_sum / r_prev.W : 0;
                    const float numerator = r_prev.M * targetLum_prev;
                    const float denom = numerator / jacobian + r.M * targetLum_curr;
                    // Balance Heuristic
                    const float m_prev = denom > 0 ? numerator / denom : 0;
                    const float w_prev = m_prev * targetLum_curr * r_prev.W;

                    // float3 wh_local = WorldToTangentFrame(normal, wh_c);
                    // float3 wh_local = r_prev.wx;
                    // float3 wx = r_prev.halfVectorCopyShift ? wh_local : wi_offset;

                    r.Update(w_prev, r_prev.wx, r_prev.lightType, r_prev.lobe, r_prev.halfVectorCopyShift, 
                        surface.whdotwo, target_curr, rng);
                }
            }
        }

        float targetLum = Math::Luminance(r.target);
        r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;
        r.M = newM;
    }

    void SpatialResample(uint2 DTid, float3 pos, float3 normal, float z_view, 
        float roughness, BSDF::ShadingData surface, uint reservoir_A_DescHeapIdx, 
        uint reservoir_B_DescHeapIdx, uint reservoir_C_DescHeapIdx, float alpha_min, 
        RaytracingAccelerationStructure g_bvh, ConstantBuffer<cbFrameConstants> g_frame, 
        inout Reservoir r_c, inout RNG rng)
    {
        static const half2 k_samples[16] =
        {
            half2(-0.899423, 0.365076),
            half2(-0.744442, -0.124006),
            half2(-0.229714, 0.245876),
            half2(-0.545186, 0.741148),
            half2(-0.156274, -0.336366),
            half2(0.468400, 0.348798),
            half2(0.035776, 0.606928),
            half2(-0.208966, 0.904852),
            half2(-0.491070, -0.484810),
            half2(0.162490, -0.081156),
            half2(0.232062, -0.851382),
            half2(0.641310, -0.162124),
            half2(0.320798, 0.922460),
            half2(0.959086, 0.263642),
            half2(0.531136, -0.519002),
            half2(-0.223014, -0.774740)
        };

        GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::DEPTH];
        GBUFFER_METALLIC_ROUGHNESS g_mr = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];

        // rotate sample sequence per pixel
        const float u0 = rng.Uniform();
        const int offset = (int)rng.UniformUintBounded_Faster(16);
        const float theta = u0 * TWO_PI;
        const float sinTheta = sin(theta);
        const float cosTheta = cos(theta);

        const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
        PairwiseMIS pairwiseMIS = PairwiseMIS::Init(NUM_SPATIAL_SAMPLES, r_c);

        float3 samplePos[NUM_SPATIAL_SAMPLES];
        float3 sampleOrigin[NUM_SPATIAL_SAMPLES];
        int16_t2 samplePosSS[NUM_SPATIAL_SAMPLES];
        bool sampleMetallic[NUM_SPATIAL_SAMPLES];
        float sampleRoughness[NUM_SPATIAL_SAMPLES];
        bool sampleTr[NUM_SPATIAL_SAMPLES];
        bool sampleSubsurf[NUM_SPATIAL_SAMPLES];
        bool sampleCoated[NUM_SPATIAL_SAMPLES];
        uint16 k = 0;

        [loop]
        for (int i = 0; i < NUM_SPATIAL_SAMPLES; i++)
        {
            float2 sampleUV = k_samples[(offset + i) & 15];
            float2 rotated;
            rotated.x = dot(sampleUV, float2(cosTheta, -sinTheta));
            rotated.y = dot(sampleUV, float2(sinTheta, cosTheta));
            rotated *= SPATIAL_SEARCH_RADIUS;
            const int2 posSS_i = round(float2(DTid) + rotated);

            if (Math::IsWithinBounds(posSS_i, (int2)renderDim))
            {
                const float2 mr_i = g_mr[posSS_i];
                GBuffer::Flags flags_i = GBuffer::DecodeMetallic(mr_i.x);

                if (flags_i.invalid || flags_i.emissive)
                    continue;

                const float depth_i = g_depth[posSS_i];
                float2 lensSample = 0;
                float3 origin_i = g_frame.CameraPos;
                if(g_frame.DoF)
                {
                    RNG rngDoF = RNG::Init(RNG::PCG3d(posSS_i.xyx).zy, g_frame.FrameNum);
                    lensSample = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
                    lensSample *= g_frame.LensRadius;
                }

                float3 pos_i = Math::WorldPosFromScreenSpace2(posSS_i, renderDim, depth_i, 
                    g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrCameraJitter, 
                    g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, g_frame.CurrView[2].xyz, 
                    g_frame.DoF, lensSample, g_frame.FocusDepth, origin_i);

                bool valid = PlaneHeuristic(pos_i, normal, pos, z_view);
                valid = valid && (abs(mr_i.y - roughness) < MAX_ROUGHNESS_DIFF_REUSE);

                if (!valid)
                    continue;

                samplePos[k] = pos_i;
                sampleOrigin[k] = origin_i;
                samplePosSS[k] = (int16_t2)posSS_i;
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
            GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
                GBUFFER_OFFSET::NORMAL];
            const float3 sampleNormal = Math::DecodeUnitVector(g_normal[samplePosSS[i]]);

            GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
                GBUFFER_OFFSET::BASE_COLOR];
            const float4 sampleBaseColor = sampleSubsurf[i] ? g_baseColor[samplePosSS[i]] :
                float4(g_baseColor[samplePosSS[i]].rgb, 0);

            float sampleEta_next = DEFAULT_ETA_MAT;

            if(sampleTr[i])
            {
                GBUFFER_IOR g_ior = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
                    GBUFFER_OFFSET::IOR];

                float ior = g_ior[samplePosSS[i]];
                sampleEta_next = GBuffer::DecodeIOR(ior);
            }

            float sample_coat_weight = 0;
            float3 sample_coat_color = 0.0f;
            float sample_coat_roughness = 0;
            float sample_coat_ior = DEFAULT_ETA_COAT;

            if(sampleCoated[i])
            {
                GBUFFER_COAT g_coat = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
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

            Reservoir r_spatial = Reservoir::Load(samplePosSS[i], 
                reservoir_A_DescHeapIdx, reservoir_B_DescHeapIdx,
                reservoir_C_DescHeapIdx);

            pairwiseMIS.Stream(r_c, pos, normal, surface, r_spatial, samplePos[i], 
                sampleNormal, surface_i, alpha_min, g_bvh, g_frame, rng);
        }

        pairwiseMIS.End(r_c, rng);
        r_c = pairwiseMIS.r_s;
    }
}

#endif