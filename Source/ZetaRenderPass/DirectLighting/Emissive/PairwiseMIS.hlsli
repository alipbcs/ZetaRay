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
            ret.target_s = Target::Init();

            return ret;
        }

        float Compute_m_i(Reservoir r_c, Reservoir r_i, float targetLum, float w_sum_i)
        {
            const float p_i_y_i = r_i.W > 0 ? w_sum_i / r_i.W : 0;
            const float p_c_y_i = targetLum;
            float numerator = (float)r_i.M * p_i_y_i;
            float denom = numerator + ((float)r_c.M / this.k) * p_c_y_i;
            float m_i = denom > 0 ? numerator / denom : 0;

            return m_i;
        }

        void Update_m_c(Reservoir r_c, Reservoir r_i, Target target_c, float3 brdfCosTheta_i, 
            float3 wi_i, float t_i)
        {
            const float cosThetaPrime = saturate(dot(target_c.lightNormal, -wi_i));
            const float dwdA = cosThetaPrime / max(t_i * t_i, 1e-6);
            const float p_i_y_c = Math::Luminance(r_c.Le * brdfCosTheta_i * dwdA);
            const float p_c_y_c = Math::Luminance(target_c.p_hat);

            const float numerator = (float)r_i.M * p_i_y_c;
            const float denom = numerator + ((float)r_c.M / this.k) * p_c_y_c;
            // Note: denom can never be zero, otherwise r_c didn't have a valid sample
            // and this function shouldn't have been called
            this.m_c += 1 - (numerator / denom);
        }

        void Stream(Reservoir r_c, float3 pos_c, float3 normal_c, BSDF::ShadingData surface_c, 
            Reservoir r_i, float3 pos_i, float3 normal_i, float w_sum_i, 
            BSDF::ShadingData surface_i, StructuredBuffer<RT::EmissiveTriangle> g_emissives, 
            RaytracingAccelerationStructure g_bvh, Target target_c, inout RNG rng)
        {
            float3 currTarget = 0;
            float m_i = 0;
            EmissiveData emissive_i;
            float dwdA;

            // m_i
            if(r_i.IsValid())
            {
                emissive_i = EmissiveData::Init(r_i, g_emissives);
                emissive_i.SetSurfacePos(pos_c);
                dwdA = emissive_i.dWdA();

                surface_c.SetWi(emissive_i.wi, normal_c);
                const float3 brdfCosTheta_c = BSDF::UnifiedBSDF(surface_c);
                currTarget = r_i.Le * brdfCosTheta_c * dwdA;

                if(dot(currTarget, currTarget) > 0)
                {
                    currTarget *= VisibilityApproximate(g_bvh, pos_c, emissive_i.wi, emissive_i.t, normal_c, 
                        emissive_i.ID, surface_c.HasSpecularTransmission());
                }

                const float targetLum = Math::Luminance(currTarget);
                m_i = Compute_m_i(r_c, r_i, targetLum, w_sum_i);
            }

            // m_c
            float3 brdfCosTheta_i = 0.0;
            float3 wi_i = 0.0;
            float t_i = 0;

            if(r_c.IsValid())
            {
                t_i = length(target_c.lightPos - pos_i);
                wi_i = (target_c.lightPos - pos_i) / t_i;
                surface_i.SetWi(wi_i, normal_i);
                brdfCosTheta_i = BSDF::UnifiedBSDF(surface_i);

                if(dot(brdfCosTheta_i, brdfCosTheta_i) > 0)
                {
                    brdfCosTheta_i *= VisibilityApproximate(g_bvh, pos_i, wi_i, t_i, normal_i, 
                        target_c.lightID, surface_i.HasSpecularTransmission());
                }
                
                Update_m_c(r_c, r_i, target_c, brdfCosTheta_i, wi_i, t_i);
            }

            if(r_i.IsValid())
            {
                const float w_i = m_i * Math::Luminance(currTarget) * r_i.W;

                if (this.r_s.Update(w_i, r_i.Le, r_i.LightIdx, r_i.Bary, rng))
                {
                    this.target_s.p_hat = currTarget;
                    this.target_s.rayT = emissive_i.t;
                    this.target_s.lightID = emissive_i.ID;
                    this.target_s.wi = emissive_i.wi;
                    this.target_s.dwdA = dwdA;
                }
            }

            this.M_s += r_i.M;
        }

        void End(Reservoir r_c, inout Target target_c, inout RNG rng)
        {
            const float w_c = Math::Luminance(target_c.p_hat) * r_c.W * this.m_c;
            if(!this.r_s.Update(w_c, r_c.Le, r_c.LightIdx, r_c.Bary, rng))
                target_c = this.target_s;

            this.r_s.M = this.M_s;
            const float targetLum = Math::Luminance(target_c.p_hat);
            this.r_s.W = targetLum > 0 ? this.r_s.w_sum / (targetLum * (1 + this.k)) : 0;
        }

        Reservoir r_s;
        float m_c;
        half M_s;
        uint16_t k;
        Target target_s;
    };
}

#endif