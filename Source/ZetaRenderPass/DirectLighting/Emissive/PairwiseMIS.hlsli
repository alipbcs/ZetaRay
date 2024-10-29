#ifndef RESTIR_DI_PAIRWISE_MIS_H
#define RESTIR_DI_PAIRWISE_MIS_H

#include "Util.hlsli"

namespace RDI_Util
{
    // Ref: Bitterli, Benedikt, "Correlations and Reuse for Fast and Accurate Physically Based Light Transport" (2022). Ph.D Dissertation.
    // https://digitalcommons.dartmouth.edu/dissertations/77
    struct PairwiseMIS
    {    
        static PairwiseMIS Init(uint16_t numStrategies, Reservoir r_c)
        {
            PairwiseMIS ret;
            ret.r_s = Reservoir::Init();
            ret.m_c = 1.0f;
            ret.M_s = r_c.M;
            ret.k = numStrategies;

            return ret;
        }

        float Compute_m_i(Reservoir r_c, Reservoir r_i, float targetLum)
        {
            const float p_i_y_i = r_i.W > 0 ? r_i.w_sum / r_i.W : 0;
            const float p_c_y_i = targetLum;
            float numerator = r_i.M * p_i_y_i;
            float denom = numerator + ((float)r_c.M / this.k) * p_c_y_i;
            float m_i = denom > 0 ? numerator / denom : 0;

            return m_i;
        }

        void Update_m_c(Reservoir r_c, Reservoir r_i, float3 target)
        {
            const float p_i_y_c = Math::Luminance(target);
            const float p_c_y_c = Math::Luminance(r_c.target);

            const float numerator = r_i.M * p_i_y_c;
            const float denom = numerator + ((float)r_c.M / this.k) * p_c_y_c;
            // Note: denom can never be zero, otherwise r_c didn't have a valid sample
            // and this function shouldn't have been called
            this.m_c += 1 - (numerator / denom);
        }

        void Stream(Reservoir r_c, float3 pos_c, float3 normal_c, BSDF::ShadingData surface_c, 
            Reservoir r_i, float3 pos_i, float3 normal_i, BSDF::ShadingData surface_i, 
            StructuredBuffer<RT::EmissiveTriangle> g_emissives, 
            RaytracingAccelerationStructure g_bvh, inout RNG rng)
        {
            float3 currTarget = 0;
            float m_i = 0;
            EmissiveData emissive_i;

            // m_i
            if(r_i.lightIdx != UINT32_MAX)
            {
                emissive_i = EmissiveData::Init(r_i.lightIdx, r_i.bary, g_emissives);
                emissive_i.SetSurfacePos(pos_c);
                float dwdA = emissive_i.dWdA();

                surface_c.SetWi(emissive_i.wi, normal_c);
                currTarget = r_i.le * BSDF::Unified(surface_c).f * dwdA;

                if(dot(currTarget, currTarget) > 0)
                {
                    currTarget *= RtRayQuery::Visibility_Segment(pos_c, emissive_i.wi, emissive_i.t, normal_c, 
                        emissive_i.ID, g_bvh, surface_c.Transmissive());
                }

                const float targetLum = Math::Luminance(currTarget);
                m_i = Compute_m_i(r_c, r_i, targetLum);
            }

            // m_c
            float3 target_c = 0.0;

            if(r_c.lightIdx != UINT32_MAX)
            {
                float3 wi_i = r_c.lightPos - pos_i;
                const bool isZero = dot(wi_i, wi_i) == 0;
                float t_i = isZero ? 0 : length(wi_i);
                wi_i = isZero ? 0 : wi_i / t_i;
                surface_i.SetWi(wi_i, normal_i);

                const float3 lightNormal = dot(r_c.lightNormal, -wi_i) < 0 && r_c.doubleSided ?
                    -r_c.lightNormal : r_c.lightNormal;
                const float cosThetaPrime = saturate(dot(lightNormal, -wi_i));
                const float dwdA = isZero ? 0 : cosThetaPrime / (t_i * t_i);

                target_c = r_c.le * BSDF::Unified(surface_i).f * dwdA;

                if(dot(target_c, target_c) > 0)
                {
                    target_c *= RtRayQuery::Visibility_Segment(pos_i, wi_i, t_i, normal_i, 
                        r_c.lightID, g_bvh, surface_i.Transmissive());
                }
                
                Update_m_c(r_c, r_i, target_c);
            }

            if(r_i.lightIdx != UINT32_MAX)
            {
                const float w_i = m_i * Math::Luminance(currTarget) * r_i.W;

                if (this.r_s.Update(w_i, r_i.le, r_i.lightIdx, r_i.bary, rng))
                    r_s.target = currTarget;
            }

            this.M_s += r_i.M;
        }

        void End(Reservoir r_c, inout RNG rng)
        {
            // w_sum = Luminance(target) * W
            const float w_c = this.m_c * r_c.w_sum;
            if(r_s.Update(w_c, r_c.le, r_c.lightIdx, r_c.bary, rng))
                r_s.target = r_c.target;

            this.r_s.M = this.M_s;
            const float targetLum = Math::Luminance(r_s.target);
            this.r_s.W = targetLum > 0 ? this.r_s.w_sum / (targetLum * (1 + this.k)) : 0;
        }

        Reservoir r_s;
        float m_c;
        half M_s;
        uint16_t k;
    };
}

#endif