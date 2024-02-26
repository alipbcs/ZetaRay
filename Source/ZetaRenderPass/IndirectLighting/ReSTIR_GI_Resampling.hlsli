#ifndef RESTIR_GI_RESAMPLING_H
#define RESTIR_GI_RESAMPLING_H

#include "ReSTIR_GI_PathTracing.hlsli"
#include "ReSTIR_GI_Reservoir.hlsli"

namespace RGI_Util
{
    struct TemporalSampleData
    {
        float3 posW;
        float3 normal;
        float roughness;
        int16_t2 posSS;
        uint16_t metallic;
        float transmission;
        float eta_i;
    };

    template<int N>
    struct TemporalSamples
    {
        static TemporalSamples Init()
        {
            TemporalSamples<N> ret;

            for(int i = 0; i < N; i++)
                ret.valid[i] = false;

            return ret;
        }

        TemporalSampleData data[N];
        bool valid[N];
    };

    RGI_Util::Reservoir RIS_InitialCandidates(float3 primaryPos, float3 primaryNormal, float z_view,
        BSDF::ShadingData primarySurface, float ior, int maxNumBounces, float localCurvature, uint sampleSetIdx,
        ConstantBuffer<cbFrameConstants> g_frame, ConstantBuffer<cb_ReSTIR_GI_SpatioTemporal> g_local,
        RaytracingAccelerationStructure g_bvh, StructuredBuffer<RT::MeshInstance> g_frameMeshData, 
        StructuredBuffer<Vertex> g_vertices, StructuredBuffer<uint> g_indices, 
        StructuredBuffer<Material> g_materials, StructuredBuffer<RT::EmissiveTriangle> g_emissives,
        StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets, 
        StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable, 
        StructuredBuffer<RT::VoxelSample> g_lvg,
        inout RNG rngThread, inout RNG rngGroup)
    {
        RGI_Util::Reservoir r = RGI_Util::Reservoir::Init();

        // Find sample point (second path vertex)
        RGI_Trace::HitSurface hitInfo;
        float3 p_source;
        float3 wi = !primarySurface.HasSpecularTransmission() ?
            RGI_Util::SampleBRDF<true>(primaryNormal, primarySurface, rngThread, p_source) :
            RGI_Util::SampleDielectricBSDF_RIS<true>(primaryNormal, primarySurface, rngThread, p_source);

        if(p_source.x == 0)
            return r;

        RT::RayCone rayCone = RT::RayCone::InitFromPrimaryHit(g_frame.PixelSpreadAngle, z_view);

        uint hitID;
        bool hit = RGI_Trace::FindClosestHit<true>(primaryPos, primaryNormal, wi, g_bvh, g_frameMeshData, 
            g_vertices, g_indices, hitInfo, hitID, primarySurface.HasSpecularTransmission());

        if(!hit)
            return r;

        // Cone geometry changes based on whether it was reflected or refracted
        if(dot(wi, primaryNormal) >= 0)
        {
            rayCone.UpdateConeGeometry_Refl(localCurvature);
        }
        else
        {
            rayCone.UpdateConeGeometry_Tr_PrimaryHit(primarySurface.wo, wi, primaryNormal, 
                localCurvature, primarySurface.eta, primaryPos, g_frame.CameraPos);
        }

        const float3 hitPos = primaryPos + hitInfo.t * wi;
        const float3 hitNormal = hitInfo.normal;

        // Path tracing loop to find incident radiance from sample point towards primary surface
        float3 Lo = RGI_Util::PathTrace(primaryPos, primaryNormal, wi, ior, maxNumBounces, 
            sampleSetIdx, hitInfo, rayCone, g_bvh, g_frame, g_local, g_frameMeshData, 
            g_vertices, g_indices, g_materials, g_emissives, g_sampleSets, g_aliasTable, g_lvg, 
            rngThread, rngGroup);

        // target = Lo * BSDF(wi, wo) * |ndotwi|
        // source = P(wi)
        float3 target = Lo;

        if(dot(Lo, Lo) > 0)
        {
            primarySurface.SetWi(wi, primaryNormal);
            target *= BSDF::UnifiedBSDF(primarySurface);
        }

        float targetLum = Math::Luminance(target);
        float w = targetLum / max(p_source.x, 1e-6f);

        r.Update(w, hitPos, hitNormal, hitID, Lo, target, rngThread);

        // = w / targetLum
        r.W = targetLum > 0 ? 1.0 / p_source.x : 0.0;

        return r;
    }

    bool PlaneHeuristic(float3 samplePos, float3 currNormal, float3 currPos, float linearDepth, 
        float th = MAX_PLANE_DIST_REUSE)
    {
        float planeDist = dot(currNormal, samplePos - currPos);
        bool onPlane = abs(planeDist) <= th * linearDepth;

        return onPlane;
    }

    template<int N>
    TemporalSamples<N> FindTemporalCandidate(uint2 DTid, float3 posW, float3 normal, float viewZ, 
        float roughness, float transmission, float2 prevUV, ConstantBuffer<cbFrameConstants> g_frame, 
        inout RNG rng)
    {
        TemporalSamples<N> candidate = RGI_Util::TemporalSamples<N>::Init();

        if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
            return candidate;

        GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
        GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
        GBUFFER_METALLIC_ROUGHNESS g_prevMetallicRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];
        GBUFFER_TRANSMISSION g_transmission = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::TRANSMISSION];

        const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
        int2 prevPixel = prevUV * renderDim;
        int curr = 0;

        for(int i = 0; i < NUM_TEMPORAL_SEARCH_ITER; i++)
        {
            const float theta = rng.Uniform() * TWO_PI;
            const float sinTheta = sin(theta);
            const float cosTheta = cos(theta);
            const float2 offset = TEMPORAL_SEARCH_RADIUS * float2(sinTheta, cosTheta);
            int2 samplePosSS = prevPixel + (i > 0) * offset;

            if(samplePosSS.x >= renderDim.x || samplePosSS.y >= renderDim.y)
                continue;

            if(i > 0 && samplePosSS.x == DTid.x && samplePosSS.y == DTid.y)
                continue;

            // plane-based heuristic
            float viewZ_prev = g_prevDepth[samplePosSS];
            const float3 prevPos = Math::WorldPosFromScreenSpace(samplePosSS,
                renderDim,
                viewZ_prev,
                g_frame.TanHalfFOV,
                g_frame.AspectRatio,
                g_frame.PrevViewInv,
                g_frame.PrevCameraJitter);

            if(!RGI_Util::PlaneHeuristic(prevPos, normal, posW, viewZ))
                continue;

            const float2 prevMR = g_prevMetallicRoughness[samplePosSS];

            bool prevMetallic;
            bool prevTransmissive;
            bool prevEmissive;
            GBuffer::DecodeMetallic(prevMR.x, prevMetallic, prevTransmissive, prevEmissive);

            if(prevEmissive)
                continue;

            const float3 prevNormal = Math::DecodeUnitVector(g_prevNormal[samplePosSS]);
            candidate.valid[curr] = dot(prevNormal, normal) > 0.1;

            if(roughness < 0.5)
            {
                const float roughnessDiff = abs(prevMR.y - roughness);
                candidate.valid[curr] = candidate.valid[curr] && (roughnessDiff < 0.15);
            }

            float prevTransmission = 0;
            float prevEta_i = 1.5f;

            if(prevTransmissive)
            {
                float2 tr_ior = g_transmission[samplePosSS];
                prevTransmission = tr_ior.x;
                prevEta_i = GBuffer::DecodeIOR(tr_ior.y);
            }

            candidate.valid[curr] = candidate.valid[curr] && abs(transmission - prevTransmission) < 0.1f;

            if(candidate.valid[curr])
            {
                candidate.data[curr].posSS = (int16_t2)samplePosSS;
                candidate.data[curr].posW = prevPos;
                candidate.data[curr].normal = prevNormal;
                candidate.data[curr].metallic = prevMetallic;
                candidate.data[curr].roughness = prevMR.y;
                candidate.data[curr].transmission = prevTransmission;
                candidate.data[curr].eta_i = prevEta_i;

                curr++;

                if(curr == N)
                    break;
            }
        }

        return candidate;
    }

    float TargetLumAtTemporalPixel(Reservoir r_curr, TemporalSampleData candidate, ConstantBuffer<cbFrameConstants> g_frame,
        RaytracingAccelerationStructure g_bvh, bool testVisibility = true)
    {
        float3 wi = r_curr.pos - candidate.posW;
        if(dot(wi, wi) == 0)
            return 0;

        float t = length(wi);
        wi /= max(t, 1e-6);

        GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::BASE_COLOR];

        const float3 baseColor_prev = g_prevBaseColor[candidate.posSS].rgb;
        const float3 camPos_prev = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, g_frame.PrevViewInv._m23);
        const float3 wo_prev = normalize(camPos_prev - candidate.posW);

        BSDF::ShadingData surface_prev = BSDF::ShadingData::Init(candidate.normal, wo_prev, candidate.metallic, 
            candidate.roughness, baseColor_prev, candidate.eta_i, 1.0f, candidate.transmission);

        surface_prev.SetWi(wi, candidate.normal);
        const float3 target_prev = r_curr.Lo * BSDF::UnifiedBSDF(surface_prev);
        const float targetLum_prev = Math::Luminance(target_prev);

        // Test if sample point was visible from temporal pixel. Ideally, previous frame's 
        // BVH should be used.
        if(testVisibility && targetLum_prev > 1e-5)
        {
            if(!RGI_Trace::Visibility_Segment(candidate.posW, wi, t, candidate.normal, r_curr.ID, g_bvh, surface_prev.HasSpecularTransmission()))
                return 0;
        }

        return targetLum_prev;
    }

    // Jacobian of path reconnection in solid angle measure, e.g. reusing a path from pixel q at pixel r
    //        T[x1_q, x2_q, x3_q, ...] = x1_r, x2_r, x3_q, ...
    float JacobianReconnectionShift(float3 x2_normal, float3 x1_r, float3 x1_q, float3 x2_q)
    {
        float3 v_r = x1_r - x2_q;
        const float t_r2 = dot(v_r, v_r);
        v_r = dot(v_r, v_r) == 0 ? v_r : v_r / max(sqrt(t_r2), 1e-6);

        float3 v_q = x1_q - x2_q;
        const float t_q2 = dot(v_q, v_q);
        v_q = dot(v_q, v_q) == 0 ? v_q : v_q / max(sqrt(t_q2), 1e-6);

        // phi_r is the angle between v_r and surface normal at x2_q
        // phi_q is the angle between v_q and surface normal at x2_q
        float cosPhi_r = dot(v_r, x2_normal);
        float cosPhi_q = dot(v_q, x2_normal);

        float j = (abs(cosPhi_r) * t_q2) / max(abs(cosPhi_q) * t_r2, 1e-6);
        // j = clamp(j, 1e-4, 1e2);

        return j;
    }

    void TemporalResample1(float3 posW, float3 normal, float roughness, BSDF::ShadingData surface, 
        uint prevReservoir_A_DescHeapIdx, uint prevReservoir_B_DescHeapIdx, uint prevReservoir_C_DescHeapIdx,
        TemporalSampleData candidate, ConstantBuffer<cbFrameConstants> g_frame, RaytracingAccelerationStructure g_bvh, 
        inout Reservoir r, inout RNG rng)
    {
        Reservoir r_prev = PartialReadReservoir_Reuse(candidate.posSS,
            prevReservoir_A_DescHeapIdx,
            prevReservoir_B_DescHeapIdx);

        const uint16_t M_new = r.M + r_prev.M;

        // Target at temporal pixel with current pixel's sample
        if(r.w_sum != 0)
        {
            float targetLum_prev = 0.0f;

            if(r_prev.M > 0 && Math::Luminance(r.Lo) > 1e-6)
                targetLum_prev = TargetLumAtTemporalPixel(r, candidate, g_frame, g_bvh);

            const float p_curr = Math::Luminance(r.target_z);
            // p_temporal at current reservoir's sample (r.y) is equal to p_temporal(x) where
            // x is the input that would get mapped to r.y under the shift, followed by division 
            // by Jacobian of the mapping (J(T(x) = y)). Since J(T(x) = y) = 1 / J(T^-1(y) = x) 
            // we have
            // p_temporal(r.y) = p_temporal(x) / J(T(x) = y)
            //                 = p_temporal(x) * J(T^-1(y) = x)
            const float J_curr_to_temporal = JacobianReconnectionShift(r.normal, candidate.posW, posW, r.pos);
            const float m_curr = p_curr / max(p_curr + r_prev.M * targetLum_prev * J_curr_to_temporal, 1e-6);
            r.w_sum *= m_curr;
        }

        if(r_prev.ID == uint(-1) || dot(r_prev.Lo, 1) == 0)
        {
            float targetLum = Math::Luminance(r.target_z);
            r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;
            r.M = M_new;

            return;
        }

        // target at current pixel with previous reservoir's sample
        float3 wi = r_prev.pos - posW;
        float t = length(wi);
        wi /= t;

        surface.SetWi(wi, normal);
        const float3 target_curr = r_prev.Lo * BSDF::UnifiedBSDF(surface);
        const float targetLum_curr = Math::Luminance(target_curr);

        // Target at current pixel with temporal reservoir's sample
        if(targetLum_curr > 1e-6)
        {
            if(RGI_Trace::Visibility_Segment(posW, wi, t, normal, r_prev.ID, g_bvh, surface.HasSpecularTransmission()))
            {
                PartialReadReservoir_ReuseRest(candidate.posSS, prevReservoir_C_DescHeapIdx, r_prev);

                const float targetLum_prev = r_prev.W > 0 ? r_prev.w_sum / r_prev.W : 0;
                const float J_temporal_to_curr = JacobianReconnectionShift(r_prev.normal, posW, candidate.posW, r_prev.pos);
                // J_temporal_to_curr in the numerator cancels out with the same term in w_prev
                const float numerator = r_prev.M * targetLum_prev;
                const float denom = numerator / max(J_temporal_to_curr, 1e-6) + targetLum_curr;
                // Balance heuristic
                const float m_prev = numerator / max(denom, 1e-6);
                const float w_prev = m_prev * targetLum_curr * r_prev.W;

                r.Update(w_prev, r_prev.pos, r_prev.normal, r_prev.ID, r_prev.Lo, target_curr, rng);
            }
        }

        float targetLum = Math::Luminance(r.target_z);
        r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;
        r.M = M_new;
    }

    void TemporalResample2(float3 posW, float3 normal, float roughness, BSDF::ShadingData surface, 
        uint prevReservoir_A_DescHeapIdx, uint prevReservoir_B_DescHeapIdx, uint prevReservoir_C_DescHeapIdx,
        TemporalSampleData candidate[2], ConstantBuffer<cbFrameConstants> g_frame, RaytracingAccelerationStructure g_bvh, 
        inout Reservoir r, inout RNG rng)
    {
        uint16_t M_new = r.M;
        Reservoir r_prev[2];

        for (int k = 0; k < 2; k++)
        {
            r_prev[k] = PartialReadReservoir_Reuse(candidate[k].posSS,
                prevReservoir_A_DescHeapIdx,
                prevReservoir_B_DescHeapIdx);

            M_new += r_prev[k].M;
        }
    
        // Target at temporal pixel with current pixel's sample
        {
            const float p_curr = Math::Luminance(r.target_z);
            float denom = p_curr;

            if(Math::Luminance(r.Lo) > 1e-5)
            {
                for(int p = 0; p < 2; p++)
                {
                    if(r_prev[p].M == 0)
                        continue;

                    float targetLum_prev = TargetLumAtTemporalPixel(r, candidate[p], g_frame, g_bvh, p != 0);
                    float J_curr_to_temporal = JacobianReconnectionShift(r.normal, candidate[p].posW, posW, r.pos);
                    denom += r_prev[p].M * J_curr_to_temporal * targetLum_prev;
                }
            }

            const float m_curr = denom == 0 ? 0 : p_curr / denom;
            r.w_sum *= m_curr;
        }

        // Target at current pixel with temporal reservoir's sample
        for(int i = 0; i < 2; i++)    
        {
            float3 wi = r_prev[i].pos - posW;
            float t = all(wi == 0) ? 0 : length(wi);
            wi /= max(t, 1e-6);
            surface.SetWi(wi, normal);

            const float3 target_curr = r_prev[i].Lo * BSDF::UnifiedBSDF(surface);
            const float targetLum_curr = Math::Luminance(target_curr);
    
            // RIS weights becomes zero; then only M needs to be updated, which is done at the end anyway
            if(targetLum_curr < 1e-5f)
                continue;

            if(RGI_Trace::Visibility_Segment(posW, wi, t, normal, r_prev[i].ID, g_bvh, surface.HasSpecularTransmission()))
            {
                PartialReadReservoir_ReuseRest(candidate[i].posSS, prevReservoir_C_DescHeapIdx, r_prev[i]);

                const float targetLum_prev = r_prev[i].W > 0 ? r_prev[i].w_sum / r_prev[i].W : 0;
                // Balance heuristic
                const float J_temporal_to_curr = JacobianReconnectionShift(r_prev[i].normal, posW, candidate[i].posW, r_prev[i].pos);
                // J_temporal_to_curr in the numerator cancels out with the same term in w_prev
                const float numerator = r_prev[i].M * targetLum_prev;
                float denom = (numerator / J_temporal_to_curr) + targetLum_curr;
                if(r_prev[1 - i].M > 0 && targetLum_prev > 0)
                {
                    const float J_temporal_to_temporal = JacobianReconnectionShift(r_prev[i].normal, candidate[1 - i].posW, 
                        candidate[i].posW, r_prev[i].pos);
                    const float targetLum_other = TargetLumAtTemporalPixel(r_prev[i], candidate[1 - i], g_frame, g_bvh);
                    denom += r_prev[1 - i].M * targetLum_other / max(J_temporal_to_temporal, 1e-6);
                }

                denom = J_temporal_to_curr == 0 ? 0 : denom;
                const float m_prev = denom == 0 ? 0 : numerator / denom;
                // Resampling weight
                const float w_prev = m_prev * targetLum_curr * r_prev[i].W;

                r.Update(w_prev, r_prev[i].pos, r_prev[i].normal, r_prev[i].ID, r_prev[i].Lo, target_curr, rng);
            }
        }

        float targetLum = Math::Luminance(r.target_z);
        r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;
        r.M = M_new;
    }

    float2 VirtualMotionReproject_Refl(float3 posW, float roughness, float3 wo, float whdotwo, float ndotwo, float t,
        float k, float z_view, ConstantBuffer<cbFrameConstants> g_frame)
    {
        // For a spherical mirror, the radius of curvature is defined as R = 1 / k where k denotes curvature. Then
        //        1 / d_o + 1 / d_i = 2 / R
        //        1 / d_o + 1 / d_i = 2k
        // 
        // where d_i and d_o denote distance along the optical axis (surface normal) for the image and the object 
        // respectively. Let wo be the unit direction vector from the surface towards the eye, then ray 
        // distances t_i and t_o are given by
        //        t_o = d_o / whdotwo
        //        t_i = -d_i / whdotwo
        //
        // Replacing into above
        //        t_i = t_o / (1 - 2 k whdotwo t_o)
        //
        // As a convention, convex and concave surfaces have negative and positive radius of curvatures respectively.
        // Ref: https://phys.libretexts.org/Bookshelves/University_Physics/Book%3A_University_Physics_(OpenStax)/University_Physics_III_-_Optics_and_Modern_Physics_(OpenStax)/02%3A_Geometric_Optics_and_Image_Formation/2.03%3A_Spherical_Mirrors
    
        float pixelWidth = 2.0 * g_frame.TanHalfFOV * z_view / max(ndotwo, 1e-6);

        // Curvature computation above used the opposite signs
        k *= -1.0f;
        k *= pixelWidth;
        float imageDist = t / max(1.0f - 2 * k * t * whdotwo, 1e-6);

        // Interpolate between virtual motion and surface motion using GGX dominant factor
        // Ref: D. Zhdan, "ReBLUR: A Hierarchical Recurrent Denoiser," in Ray Tracing Gems 2, 2021.
        float factor = BSDF::MicrofacetBRDFGGXSmithDominantFactor(ndotwo, roughness);
        float3 virtualPos = posW - wo * imageDist * factor;
    
        float2 prevUV = Math::UVFromWorldPos(virtualPos, 
            float2(g_frame.RenderWidth, g_frame.RenderHeight), 
            g_frame.TanHalfFOV,
            g_frame.AspectRatio,
            g_frame.PrevView);

        return prevUV;
    }

    float2 VirtualMotionReproject_FlatMirror(float3 posW, float3 wo, float t, float4x4 prevViewProj)
    {
        float3 virtualPos = posW - wo * t;
        float4 virtualPosNDC = mul(prevViewProj, float4(virtualPos, 1.0f));
        float2 prevUV = Math::UVFromNDC(virtualPosNDC.xy / virtualPosNDC.w);

        return prevUV;
    }

    float2 VirtualMotionReproject_Tr(float3 posW, float3 wo, float t, float3 normal, float eta,
        float k, float z_view, float whdotwo, float ndotwo, ConstantBuffer<cbFrameConstants> g_frame)
    {
        float pixelWidth = 2.0 * g_frame.TanHalfFOV * z_view / max(ndotwo, 1e-6);
        k *= -1.0f;
        k *= pixelWidth;
        float imageDist_tr = eta / (k * (1 - 1 / eta) - 1 / (eta * t));
        float imageDist_refl = t / (1.0f - 2 * k * t * whdotwo);

        float3 wi = refract(-wo, normal, 1 / eta);
        // Switch to reflection for TIR
        float3 virtualPos = dot(wi, wi) == 0 ? posW - wo * imageDist_refl : posW + wi * imageDist_tr;
        float2 prevUV = Math::UVFromWorldPos(virtualPos, 
            float2(g_frame.RenderWidth, g_frame.RenderHeight), 
            g_frame.TanHalfFOV,
            g_frame.AspectRatio,
            g_frame.PrevView);

        return prevUV;
    }

    // When some reservoir has a much higher weight compared to other reservoirs in its 
    // neighborhood, it can lead to artifacts that appear as bright spots. Following
    // attempts to mitigate this issue.
    void SuppressOutlierReservoirs(inout RGI_Util::Reservoir r)
    {
        float waveSum = WaveActiveSum(r.w_sum);
        float waveAvg = (waveSum - r.w_sum) / (WaveGetLaneCount() - 1);
        if(r.w_sum > 25 * waveAvg)
            r.M = 1;
    }

    RGI_Util::Reservoir EstimateIndirectLighting(uint2 DTid, float3 posW, float3 normal, float roughness, 
        float ior, float z_view, BSDF::ShadingData surface, float localCurvature, 
        ConstantBuffer<cbFrameConstants> g_frame, ConstantBuffer<cb_ReSTIR_GI_SpatioTemporal> g_local, 
        RaytracingAccelerationStructure g_bvh, 
        StructuredBuffer<RT::MeshInstance> g_frameMeshData, 
        StructuredBuffer<Vertex> g_vertices, StructuredBuffer<uint> g_indices, 
        StructuredBuffer<Material> g_materials, StructuredBuffer<RT::EmissiveTriangle> g_emissives,
        StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets, 
        StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable,
        StructuredBuffer<RT::VoxelSample> g_lvg,
        inout RNG rngThread, inout RNG rngGroup)
    {
        int maxNumBounces = max(g_local.MaxDiffuseBounces, 
            max(g_local.MaxGlossyBounces, g_local.MaxTransmissionBounces));

        // Artifacts become noticeable in motion for specular surfaces
        if(IS_CB_FLAG_SET(CB_IND_FLAGS::STOCHASTIC_MULTI_BOUNCE) && (roughness >= 0.1 || g_frame.CameraStatic))
            maxNumBounces = rngGroup.Uniform() < 0.5 ? 1 : maxNumBounces;

        // Use the same sample set for all the threads in this group
        const uint sampleSetIdx = rngGroup.UniformUintBounded_Faster(g_local.SampleSetSize_NumSampleSets >> 16);

        RGI_Util::Reservoir r = RGI_Util::RIS_InitialCandidates(posW, normal, z_view, surface, ior, maxNumBounces, 
            localCurvature, sampleSetIdx, g_frame, g_local, g_bvh, g_frameMeshData, g_vertices, g_indices, 
            g_materials, g_emissives, g_sampleSets, g_aliasTable, g_lvg, rngThread, rngGroup);

        if (IS_CB_FLAG_SET(CB_IND_FLAGS::TEMPORAL_RESAMPLE))
        {            
            // Reverse reproject current pixel
            GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
                GBUFFER_OFFSET::MOTION_VECTOR];
            const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
            const float2 motionVec = g_motionVector[DTid];
            const float2 currUV = (DTid + 0.5f) / renderDim;
            const float2 prevSurfaceUV = currUV - motionVec;
            float2 prevUV = FLT_MAX;
            float3 relfectedPos = r.pos;

            // Decide between virtual motion and surface motion
            if(roughness >= MAX_ROUGHNESS_VIRTUAL_MOTION || g_frame.CameraStatic)
            {
                prevUV = prevSurfaceUV;
            }
            else if(roughness < MAX_ROUGHNESS_VIRTUAL_MOTION)
            {
                // if(!r.IsValid() && all(prevSurfaceUV >= 0) && all(prevSurfaceUV) <= 1.0)
                if(all(prevSurfaceUV >= 0) && all(prevSurfaceUV) <= 1.0)
                {
                    int2 prevPixel = prevSurfaceUV * renderDim;
                    relfectedPos = PartialReadReservoir_Pos(prevPixel,
                        g_local.PrevReservoir_A_DescHeapIdx);
                }

                float t = length(relfectedPos - posW);
                float3 wi = (relfectedPos - posW) / t;
                bool reflection = dot(wi, normal) >= 0;
                float s = reflection ? 1 : surface.eta;
                float3 wh = normalize(mad(wi, s, surface.wo));
                wh = !reflection && surface.eta > 1 ? -wh : wh;
                float whdotwo = saturate(dot(surface.wo, wh));

                if(relfectedPos.x != FLT_MAX)
                {
                    prevUV = reflection ? 
                        VirtualMotionReproject_Refl(posW, roughness, surface.wo, whdotwo, 
                            surface.ndotwo, t, localCurvature, z_view, g_frame):
                        VirtualMotionReproject_Tr(posW, surface.wo, t, normal, surface.eta, 
                            localCurvature, z_view, whdotwo, surface.ndotwo, g_frame);
                }
            }

            TemporalSamples<2> candidate = RGI_Util::FindTemporalCandidate<2>(DTid, posW, normal, 
                z_view, roughness, surface.transmission, prevUV, g_frame, rngThread);

            float th = localCurvature == 0 ? 0.004 : 0.15;
            bool ee = PlaneHeuristic(relfectedPos, r.normal, r.pos, z_view, th);
            candidate.valid[0] = candidate.valid[0] && (!r.IsValid() || ee);

            // Skip temporal resampling if no valid sample is found
            if (candidate.valid[1] && roughness > 0.05)
            {
                RGI_Util::TemporalResample2(posW, normal, roughness, surface,
                    g_local.PrevReservoir_A_DescHeapIdx, g_local.PrevReservoir_B_DescHeapIdx, 
                    g_local.PrevReservoir_C_DescHeapIdx, candidate.data, g_frame, 
                    g_bvh, r, rngThread);
            }
            else if (candidate.valid[0])
            {
                 RGI_Util::TemporalResample1(posW, normal, roughness, surface,
                     g_local.PrevReservoir_A_DescHeapIdx, g_local.PrevReservoir_B_DescHeapIdx, 
                     g_local.PrevReservoir_C_DescHeapIdx, candidate.data[0], g_frame, 
                     g_bvh, r, rngThread);
            }

            if(IS_CB_FLAG_SET(CB_IND_FLAGS::BOILING_SUPPRESSION))
                SuppressOutlierReservoirs(r);

            RGI_Util::WriteReservoir(DTid, r, g_local.CurrReservoir_A_DescHeapIdx,
                g_local.CurrReservoir_B_DescHeapIdx, g_local.CurrReservoir_C_DescHeapIdx, 
                g_local.M_max);
        }

        // if (IS_CB_FLAG_SET(CB_IND_FLAGS::SPATIAL_RESAMPLE))
        // {
        //     RGI_Util::SpatialResample(DTid, 1, SPATIAL_SEARCH_RADIUS, posW, normal, z_view, roughness, 
        //         surface, g_local.PrevReservoir_A_DescHeapIdx, g_local.PrevReservoir_B_DescHeapIdx, 
        //         g_local.PrevReservoir_C_DescHeapIdx, g_frame, g_bvh, r, rngThread);
        // }

        return r;
    }
}

#endif
