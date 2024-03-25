#ifndef RESTIR_GI_PAIRWISE_MIS_H
#define RESTIR_GI_PAIRWISE_MIS_H

namespace RGI_Util
{
    // Ref: Bitterli, Benedikt, "Correlations and Reuse for Fast and Accurate Physically Based Light Transport" (2022). Ph.D Dissertation.
    // https://digitalcommons.dartmouth.edu/dissertations/77
    struct PairwiseMIS
    {    
        static PairwiseMIS Init(uint16_t numSamples, RGI_Util::Reservoir r_c)
        {
            PairwiseMIS ret;

            ret.r_s = RGI_Util::Reservoir::Init();
            ret.m_c = 1.0f;
            ret.M_s = r_c.M;
            ret.k = numSamples;

            return ret;
        }

        float Compute_m_i(RGI_Util::Reservoir r_c, float targetLum, RGI_Util::Reservoir r_i, 
            float jacobianNeighborToCurr)
        {
            // TODO following seems to be the correct term to use, but for some reason gives terrible results
#if 0
            const float p_i_y_i = r_i.W > 0 ? r_i.w_sum / r_i.W : 0;
#else
            const float p_i_y_i = r_i.W > 0 ? (r_i.M * r_i.w_sum) / r_i.W : 0;
#endif

            const float p_c_y_i = targetLum * r_c.M;
            // Jacobian term in the numerator cancels out with the same term in the resampling weight
            float numerator = r_i.M * p_i_y_i;
            float denom = numerator / jacobianNeighborToCurr + (r_c.M / this.k) * p_c_y_i;
            float m_i = denom > 0 ? numerator / denom : 0;

            return m_i;
        }

        void Update_m_c(RGI_Util::Reservoir r_c, RGI_Util::Reservoir r_i, float3 brdfCosTheta_i, 
            float jacobianCurrToNeighbor)
        {
            if(!r_i.IsValid())
            {
                this.m_c += 1;
                return;
            }

            const float target_i = Math::Luminance(r_c.Lo * brdfCosTheta_i);
            const float p_i_y_c = target_i * jacobianCurrToNeighbor;

            const float p_c_y_c = Math::Luminance(r_c.target_z);

            const float numerator = r_i.M * p_i_y_c;
            const bool denomGt0 = (p_c_y_c + numerator) > 0; 
            this.m_c += denomGt0 ? 1 - numerator / (numerator + (r_c.M / this.k) * p_c_y_c) : 1;
        }

        void Stream(RGI_Util::Reservoir r_c, float3 posW_c, float3 normal_c, float linearDepth_c, 
            BSDF::ShadingData surface_c, RGI_Util::Reservoir r_i, float3 posW_i, float3 normal_i, 
            BSDF::ShadingData surface_i, RaytracingAccelerationStructure g_bvh, inout RNG rng)
        {
            float3 target_curr;
            float m_i;

            // m_i
            if(r_i.IsValid())
            {
                float3 wi = r_i.pos - posW_c;
                float t = length(wi);
                wi /= t;

                surface_c.SetWi(wi, normal_c);
                const float3 brdfCosTheta_c = BSDF::CombinedBRDF(surface_c);
                target_curr = r_i.Lo * brdfCosTheta_c;

                if(Math::Luminance(target_curr) > 1e-5)
                    target_curr *= RGI_Trace::Visibility_Segment(posW_c, wi, t, normal_c, r_i.ID, g_bvh);

                const float targetLum = Math::Luminance(target_curr);
                const float J_temporal_to_curr = JacobianReconnectionShift(r_i.normal, posW_c, posW_i, r_i.pos);
                m_i = Compute_m_i(r_c, targetLum, r_i, J_temporal_to_curr);
            }

            float3 brdfCosTheta_i;
            float J_curr_to_temporal;

            // m_c
            if(r_c.IsValid())
            {
                float3 wi = r_c.pos - posW_i;
                float t = length(wi);
                wi /= t;

                surface_i.SetWi(wi, normal_i);
                brdfCosTheta_i = BSDF::CombinedBRDF(surface_i);

                if(Math::Luminance(r_c.Lo * brdfCosTheta_i) > 1e-5)
                    brdfCosTheta_i *= RGI_Trace::Visibility_Segment(posW_i, wi, t, normal_i, r_c.ID, g_bvh);

                J_curr_to_temporal = JacobianReconnectionShift(r_c.normal, posW_i, posW_c, r_c.pos);
            }    

            Update_m_c(r_c, r_i, brdfCosTheta_i, J_curr_to_temporal);

            if(r_i.IsValid())
            {
                // Jacobian term cancels out with the same term in m_i's numerator
                const float w_i = m_i * Math::Luminance(target_curr) * r_i.W;
                this.r_s.Update(w_i, r_i.pos, r_i.normal, r_i.ID, r_i.Lo, target_curr, rng);
            }

            this.M_s += r_i.M;
        }

        void End(RGI_Util::Reservoir r_c, float3 posW_c, inout RNG rng)
        {
            float3 wi = r_c.pos - posW_c;
            float t = length(wi);
            wi /= t;
            const float w_c = Math::Luminance(r_c.target_z) * r_c.W * this.m_c;
            this.r_s.Update(w_c, r_c.pos, r_c.normal, r_c.ID, r_c.Lo, r_c.target_z, rng);

            this.r_s.M = this.M_s;
            const float targetLum = Math::Luminance(r_s.target_z);
            this.r_s.W = targetLum > 0 ? this.r_s.w_sum / (targetLum * (1 + this.k)) : 0;
            // TODO investigate
            this.r_s.W = isnan(this.r_s.W) ? 0 : this.r_s.W;
        }

        RGI_Util::Reservoir r_s;
        float m_c;
        uint16_t M_s;
        uint16_t k;
    };

    void SpatialResample(uint2 DTid, uint16_t numSamples, float radius, float3 posW, 
        float3 normal, float z_view, float roughness, BSDF::ShadingData surface, 
        uint prevReservoir_A_DescHeapIdx, uint prevReservoir_B_DescHeapIdx, uint prevReservoir_C_DescHeapIdx, 
        ConstantBuffer<cbFrameConstants> g_frame, RaytracingAccelerationStructure g_bvh, 
        inout Reservoir r, inout RNG rng)
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

        GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
        GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
        GBUFFER_METALLIC_ROUGHNESS g_prevMetallicRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];
        GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::BASE_COLOR];

        // rotate sample sequence per pixel
        const float u0 = rng.Uniform();
        const uint offset = rng.UniformUintBounded_Faster(8);
        const float theta = u0 * TWO_PI;
        const float sinTheta = sin(theta);
        const float cosTheta = cos(theta);
        const int2 renderDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);
        numSamples = min(numSamples, 2);
        PairwiseMIS pairwiseMIS = PairwiseMIS::Init(numSamples, r);

        float3 samplePosW[2];
        int16_t2 samplePosSS[2];
        float sampleRoughness[2];
        bool sampleMetallic[2];
        float3 sampleNormal[2];
        uint16_t k = 0;

        for (int i = 0; i < 4; i++)
        {
            float2 sampleUV = k_hammersley[(offset + i) & 7];
            float2 rotated;
            rotated.x = dot(sampleUV, float2(cosTheta, -sinTheta));
            rotated.y = dot(sampleUV, float2(sinTheta, cosTheta));
            rotated *= radius;
            const int2 posSS_i = round(float2(DTid) + rotated);

            if (Math::IsWithinBounds(posSS_i, renderDim))
            {
                const float depth_i = g_prevDepth[posSS_i];
                if (depth_i == FLT_MAX)
                    continue;

                float3 posW_i = Math::WorldPosFromScreenSpace(posSS_i, renderDim,
                    depth_i, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevViewInv, 
                    g_frame.PrevCameraJitter, g_frame.CameraNear);
                bool valid = PlaneHeuristic(posW_i, normal, posW, z_view);

                const float2 mr_i = g_prevMetallicRoughness[posSS_i];
                
                bool metallic_i;
                bool emissive_i;
                GBuffer::DecodeMetallicEmissive(mr_i.x, metallic_i, emissive_i);

                const float3 normal_i = Math::DecodeUnitVector(g_prevNormal[samplePosSS[i]]);

                // normal heuristic
                const float normalSimilarity = dot(normal_i, normal);
                // roughness heuristic
                const float roughnessDiff = abs(mr_i.y - roughness);

                valid = valid && !emissive_i && 
                    normalSimilarity > 0.0 && 
                    roughnessDiff < 0.2;

                if (!valid)
                    continue;

                samplePosW[k] = posW_i;
                samplePosSS[k] = (int16_t2)posSS_i;
                sampleMetallic[k] = metallic_i;
                sampleRoughness[k] = mr_i.y;
                sampleNormal[k] = normal_i;
                k++;

                if(k == numSamples)
                    break;
            }
        }

        const float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, g_frame.PrevViewInv._m23);
        pairwiseMIS.k = k;

        for (int i = 0; i < k; i++)
        {
            const float3 sampleBaseColor = g_prevBaseColor[samplePosSS[i]].rgb;

            const float3 wo_i = normalize(prevCameraPos - samplePosW[i]);
            BSDF::ShadingData surface_i = BSDF::ShadingData::Init(sampleNormal[i], wo_i,
                sampleMetallic[i], sampleRoughness[i], sampleBaseColor);

            Reservoir neighbor = PartialReadReservoir_Reuse(samplePosSS[i],
                prevReservoir_A_DescHeapIdx,
                prevReservoir_B_DescHeapIdx);
            PartialReadReservoir_ReuseRest(samplePosSS[i], prevReservoir_C_DescHeapIdx, neighbor);

            pairwiseMIS.Stream(r, posW, normal, z_view, surface, neighbor, samplePosW[i], 
                sampleNormal[i], surface_i, g_bvh, rng);
        }

        pairwiseMIS.End(r, posW, rng);
        r = pairwiseMIS.r_s;
    }
}

#endif