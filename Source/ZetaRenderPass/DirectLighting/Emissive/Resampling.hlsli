#ifndef RESTIR_DI_RESAMPLING_H
#define RESTIR_DI_RESAMPLING_H

#include "Params.hlsli"
#include "PairwiseMIS.hlsli"
#include "../../Common/GBuffers.hlsli"

namespace RDI_Util
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

        if(!RDI_Util::PlaneHeuristic(prevPos, normal, pos, prevViewDepth, MAX_PLANE_DIST_REUSE))
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

    float OffsetPathTarget_CtT(Reservoir r_curr, float alpha_min, TemporalCandidate candidate, 
        float3 wh, ConstantBuffer<cbFrameConstants> g_frame,
        RaytracingAccelerationStructure g_bvh_prev,
        StructuredBuffer<RT::EmissiveTriangle> g_emissives,
        StructuredBuffer<RT::MeshInstance> g_frameMeshData)
    {
        if(!IsShiftInvertible(r_curr, candidate.surface, alpha_min))
            return 0;

        float3 target_offset = 0;
        float3 wi_offset;
        float t_offset = 0;
        uint lightID = UINT32_MAX;

#if USE_HALF_VECTOR_COPY_SHIFT == 1
        if(r_curr.halfVectorCopyShift)
        {
            wi_offset = reflect(-candidate.surface.wo, wh);
            BSDFHitInfo hitInfo = FindClosestHit(candidate.pos, candidate.normal, wi_offset, g_bvh_prev, 
                g_frameMeshData, candidate.surface.Transmissive());

            if(!hitInfo.hit)
                return 0;

            RT::EmissiveTriangle emissive = g_emissives[hitInfo.emissiveTriIdx];
            float3 le = Light::Le_EmissiveTriangle(emissive, hitInfo.bary, 
                g_frame.EmissiveMapsDescHeapOffset);

            const float3 vtx1 = Light::DecodeEmissiveTriV1(emissive);
            const float3 vtx2 = Light::DecodeEmissiveTriV2(emissive);
            float3 lightNormal = cross(vtx1 - emissive.Vtx0, vtx2 - emissive.Vtx0);
            float twoArea = length(lightNormal);
            lightNormal = dot(lightNormal, lightNormal) == 0 ? 0 : lightNormal / twoArea;
            lightNormal = emissive.IsDoubleSided() && dot(-wi_offset, lightNormal) < 0 ? 
                -lightNormal : lightNormal;

            if(dot(-wi_offset, lightNormal) > 0)
            {
                float dwdA = saturate(dot(lightNormal, -wi_offset)) / (hitInfo.t * hitInfo.t);
                target_offset = le * dwdA;
            }

            candidate.surface.SetWi(wi_offset, candidate.normal);
        }
        else
#endif
        {
            wi_offset = r_curr.lightPos - candidate.pos;
            const bool isZero = dot(wi_offset, wi_offset) == 0;
            t_offset = isZero ? 0 : length(wi_offset);
            wi_offset = isZero ? wi_offset : wi_offset / t_offset;
            candidate.surface.SetWi(wi_offset, candidate.normal);

            float3 lightNormal = r_curr.lightNormal;
            if(r_curr.doubleSided && dot(-wi_offset, lightNormal) < 0)
                lightNormal = -lightNormal;

            float cosThetaPrime = saturate(dot(lightNormal, -wi_offset));
            const float dwdA = isZero ? 0 : cosThetaPrime / (t_offset * t_offset);
            target_offset = r_curr.le * dwdA;
        }

        target_offset *= BSDF::Unified(candidate.surface).f;
        float targetLum_offset = Math::Luminance(target_offset);

        if(!r_curr.halfVectorCopyShift && targetLum_offset > 0)
        {
            targetLum_offset *= RtRayQuery::Visibility_Segment(candidate.pos, wi_offset, t_offset, 
                candidate.normal, r_curr.lightID, g_bvh_prev, candidate.surface.Transmissive());
        }

        return targetLum_offset;
    }

    float3 OffsetPathTarget_TtC(Reservoir r_prev, float3 pos, float3 normal, float alpha_min,
        BSDF::ShadingData surface, float3 wh, ConstantBuffer<cbFrameConstants> g_frame,
        RaytracingAccelerationStructure g_bvh,
        StructuredBuffer<RT::EmissiveTriangle> g_emissives,
        StructuredBuffer<RT::MeshInstance> g_frameMeshData)
    {
        if(!IsShiftInvertible(r_prev, surface, alpha_min))
            return 0;

        float3 target_offset = 0;
        float3 wi_offset;
        float t_offset = 0;
        uint lightID = UINT32_MAX;

#if USE_HALF_VECTOR_COPY_SHIFT == 1
        if(r_prev.halfVectorCopyShift)
        {
            wi_offset = reflect(-surface.wo, wh);

            BSDFHitInfo hitInfo = FindClosestHit(pos, normal, wi_offset, g_bvh, 
                g_frameMeshData, surface.Transmissive());

            if(!hitInfo.hit)
                return 0;

            RT::EmissiveTriangle emissive = g_emissives[hitInfo.emissiveTriIdx];
            float3 le = Light::Le_EmissiveTriangle(emissive, hitInfo.bary, 
                g_frame.EmissiveMapsDescHeapOffset);

            const float3 vtx1 = Light::DecodeEmissiveTriV1(emissive);
            const float3 vtx2 = Light::DecodeEmissiveTriV2(emissive);
            float3 lightNormal = cross(vtx1 - emissive.Vtx0, vtx2 - emissive.Vtx0);
            float twoArea = length(lightNormal);
            lightNormal = dot(lightNormal, lightNormal) == 0 ? 0 : lightNormal / twoArea;
            lightNormal = emissive.IsDoubleSided() && dot(-wi_offset, lightNormal) < 0 ? 
                -lightNormal : lightNormal;

            // Light is backfacing
            if(dot(-wi_offset, lightNormal) > 0)
            {
                float dwdA = saturate(dot(lightNormal, -wi_offset)) / (hitInfo.t * hitInfo.t);
                target_offset = le * dwdA;
            }

            surface.SetWi(wi_offset, normal);
        }
        else
#endif
        {
            EmissiveData prevEmissive = EmissiveData::Init(r_prev.lightIdx, r_prev.bary, g_emissives);
            prevEmissive.SetSurfacePos(pos);
            wi_offset = prevEmissive.wi;
            t_offset = prevEmissive.t;
            lightID = prevEmissive.ID;
            float dwdA = prevEmissive.dWdA();

            surface.SetWi(prevEmissive.wi, normal);
            target_offset = r_prev.le * dwdA;
        }

        target_offset *= BSDF::Unified(surface).f;
        if(!r_prev.halfVectorCopyShift && dot(target_offset, target_offset) > 0)
        {
            target_offset *= RtRayQuery::Visibility_Segment(pos, wi_offset, t_offset, normal, 
                lightID, g_bvh, surface.Transmissive());
        }

        return target_offset;
    }

    void TemporalResample1(float3 pos, float3 normal, BSDF::ShadingData surface, 
        float alpha_min, TemporalCandidate candidate, uint prevReservoir_A_DescHeapIdx, 
        uint prevReservoir_B_DescHeapIdx, ConstantBuffer<cbFrameConstants> g_frame, 
        RaytracingAccelerationStructure g_bvh, RaytracingAccelerationStructure g_bvh_prev,
        StructuredBuffer<RT::EmissiveTriangle> g_emissives,
        StructuredBuffer<RT::MeshInstance> g_frameMeshData,
        inout Reservoir r_curr, inout RNG rng)
    {
        Reservoir r_prev = Reservoir::Load(candidate.posSS,
            prevReservoir_A_DescHeapIdx, prevReservoir_B_DescHeapIdx);
        const uint16 newM = r_curr.M + r_prev.M;

        // Shift from current pixel to temporal
        if(r_curr.w_sum != 0)
        {
            float3 wh_prev = Math::FromTangentFrameToWorld(candidate.normal, r_curr.wh_local);
            float whdotwo = abs(dot(candidate.surface.wo, wh_prev));
            float jacobian = r_curr.partialJacobian == 0 ? 0 : whdotwo / r_curr.partialJacobian;
            jacobian = r_curr.halfVectorCopyShift ? jacobian : 1;

            float targetLum_prev = OffsetPathTarget_CtT(r_curr, alpha_min, candidate, wh_prev, 
                g_frame, g_bvh_prev, g_emissives, g_frameMeshData);
            const float numerator = r_curr.M * Math::Luminance(r_curr.target);
            const float denom = numerator + r_prev.M * targetLum_prev * jacobian;
            const float m_curr = denom > 0 ? numerator / denom : 0;
            r_curr.w_sum *= m_curr;
        }

        // Shift from temporal to current
        if(r_prev.lightIdx != UINT32_MAX)
        {
            float3 wh_curr = Math::FromTangentFrameToWorld(normal, r_prev.wh_local);
            float3 wh_prev = Math::FromTangentFrameToWorld(candidate.normal, r_prev.wh_local);
            float whdotwo_prev = abs(dot(candidate.surface.wo, wh_prev));
            float whdotwo_curr = abs(dot(surface.wo, wh_curr));
            float jacobian = whdotwo_prev > 0 ? whdotwo_curr / whdotwo_prev : 0;
            jacobian = r_prev.halfVectorCopyShift ? jacobian : 1;

            const float3 target_curr = OffsetPathTarget_TtC(r_prev, pos, normal, alpha_min, surface,
                wh_curr, g_frame, g_bvh, g_emissives, g_frameMeshData);
            const float targetLum_curr = Math::Luminance(target_curr);

            // w_prev becomes zero and only M needs to be updated, which is done at the end anyway
            if(targetLum_curr > 0)
            {
                const float targetLum_prev = r_prev.W > 0 ? r_prev.w_sum / r_prev.W : 0;
                // Balance Heuristic
                const float numerator = r_prev.M * targetLum_prev;
                const float denom = numerator / jacobian + r_curr.M * targetLum_curr;
                const float m_prev = denom > 0 ? numerator / denom : 0;
                const float w_prev = m_prev * targetLum_curr * r_prev.W;

                if (r_curr.Update(w_prev, r_prev.halfVectorCopyShift, r_prev.wh_local, whdotwo_curr, 
                    r_prev.lobe, r_prev.le, r_prev.lightIdx, r_prev.bary, rng))
                {
                    r_curr.target = target_curr;
                }
            }
        }

        float targetLum = Math::Luminance(r_curr.target);
        r_curr.W = targetLum > 0.0 ? r_curr.w_sum / targetLum : 0.0;
        r_curr.M = newM;
    }

    void SpatialResample(uint2 DTid, int numSamples, float radius, float3 pos, float3 normal, 
        float z_view, float roughness, BSDF::ShadingData surface, float alpha_min, 
        uint reservoir_A_DescHeapIdx, uint reservoir_B_DescHeapIdx, 
        ConstantBuffer<cbFrameConstants> g_frame, 
        RaytracingAccelerationStructure g_bvh, 
        StructuredBuffer<RT::EmissiveTriangle> g_emissives, 
        StructuredBuffer<RT::MeshInstance> g_frameMeshData,
        inout Reservoir r, inout RNG rng)
    {
        static const half2 k_samples[32] =
        {
            half2(0.620442, 0.900394),
            half2(0.444621, 0.939978),
            half2(0.455839, 0.748007),
            half2(0.764451, 0.802669),
            half2(0.670828, 0.696273),
            half2(0.222218, 0.77409),
            half2(0.50221, 0.525416),
            half2(0.362746, 0.651879),
            half2(0.629943, 0.519856),
            half2(0.572175, 0.370443),
            half2(0.347296, 0.420629),
            half2(0.284706, 0.54219),
            half2(0.127776, 0.611066),
            half2(0.882008, 0.65589),
            half2(0.778418, 0.515129),
            half2(0.210426, 0.218347),
            half2(0.0992503, 0.412867),
            half2(0.334951, 0.269779),
            half2(0.490129, 0.225641),
            half2(0.675389, 0.149382),
            half2(0.803938, 0.345802),
            half2(0.979837, 0.401849),
            half2(0.984763, 0.574738),
            half2(0.218297, 0.355557),
            half2(0.367467, 0.0574971),
            half2(0.567781, 0.0132011),
            half2(0.814379, 0.183564),
            half2(0.00541991, 0.531187),
            half2(0.924393, 0.261443),
            half2(0.287806, 0.952222),
            half2(0.679866, 0.288912),
            half2(0.341006, 0.827133)
        };

        GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::NORMAL];
        GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::DEPTH];
        GBUFFER_METALLIC_ROUGHNESS g_mr = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];
        GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::BASE_COLOR];

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
        bool sampleMetallic[MAX_NUM_SPATIAL_SAMPLES];
        float sampleRoughness[MAX_NUM_SPATIAL_SAMPLES];
        bool sampleTr[MAX_NUM_SPATIAL_SAMPLES];
        bool sampleSubsurf[MAX_NUM_SPATIAL_SAMPLES];
        bool sampleCoated[MAX_NUM_SPATIAL_SAMPLES];
        uint16_t k = 0;

        [loop]
        for (int i = 0; i < numSamples; i++)
        {
            float2 sampleUV = k_samples[(offset + i) & 31];
            float2 rotated;
            rotated.x = dot(sampleUV, float2(cosTheta, -sinTheta));
            rotated.y = dot(sampleUV, float2(sinTheta, cosTheta));
            rotated *= radius;
            const uint2 posSS_i = (uint2)round(float2(DTid) + rotated);

            if (Math::IsWithinBounds(posSS_i, renderDim))
            {
                const float2 mr_i = g_mr[posSS_i];
                GBuffer::Flags flags_i = GBuffer::DecodeMetallic(mr_i.x);

                if(flags_i.invalid || flags_i.emissive)
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
            const float3 sampleNormal = Math::DecodeUnitVector(g_normal[samplePosSS[i]]);
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

            Reservoir r_spatial = Reservoir::Load(samplePosSS[i], reservoir_A_DescHeapIdx, 
                reservoir_B_DescHeapIdx);

            pairwiseMIS.Stream(r, pos, normal, surface, r_spatial, samplePos[i], sampleNormal, 
                surface_i, alpha_min, g_frame, g_bvh, g_emissives, g_frameMeshData, rng);
        }

        pairwiseMIS.End(r, rng);
        r = pairwiseMIS.r_s;
    }
}

#endif