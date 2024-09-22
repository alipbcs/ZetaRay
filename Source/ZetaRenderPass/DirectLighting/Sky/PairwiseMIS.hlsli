#ifndef SKY_DI_PAIRWISE_MIS_H
#define SKY_DI_PAIRWISE_MIS_H

#include "Reservoir.hlsli"
#include "../../Common/RayQuery.hlsli"

namespace SkyDI_Util
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

        float Compute_m_i(Reservoir r_c, float targetLum, Reservoir r_i, 
            float jacobian)
        {
            const float p_i_y_i = r_i.W > 0 ? r_i.w_sum / r_i.W : 0;
            const float p_c_y_i = targetLum;
            float numerator = r_i.M * p_i_y_i / jacobian;
            float denom = numerator + ((float)r_c.M / this.k) * p_c_y_i;
            float m_i = denom > 0 ? numerator / denom : 0;

            return m_i;
        }

        void Update_m_c(Reservoir r_c, Reservoir r_i, float targetLum, float jacobian)
        {
            const float p_i_y_c = targetLum;
            const float p_c_y_c = Math::Luminance(r_c.target);

            const float numerator = r_i.M * p_i_y_c * jacobian;
            const float denom = numerator + ((float)r_c.M / this.k) * p_c_y_c * jacobian;
            // Note: denom can never be zero, otherwise r_c didn't have a valid sample
            // and this function shouldn't have been called
            this.m_c += 1 - (numerator / denom);
        }

        void Stream(Reservoir r_c, float3 pos_c, float3 normal_c, BSDF::ShadingData surface_c, 
            Reservoir r_i, float3 pos_i, float3 normal_i, BSDF::ShadingData surface_i, 
            float alpha_min, RaytracingAccelerationStructure g_bvh, 
            ConstantBuffer<cbFrameConstants> g_frame, inout RNG rng)
        {
            // Shift from pixel i to center
            float m_i = 0;
            float3 target_c_y_i = 0;

            if(r_i.IsValid())
            {
                float3 wi_offset = r_i.wx;
                float jacobian = 1;

                if(IsShiftInvertible(r_i, surface_c, alpha_min))
                {
                    if(r_i.halfVectorCopyShift)
                    {
                        float3 wh_local = r_i.wx;
                        float3 wh_c = FromTangentFrameToWorld(normal_c, wh_local);
                        float3 wh_i = FromTangentFrameToWorld(normal_i, wh_local);

                        wi_offset = reflect(-surface_c.wo, wh_c);
                        float whdotwo_i = abs(dot(surface_i.wo, wh_i));
                        jacobian = whdotwo_i > 0 ? abs(dot(surface_c.wo, wh_c)) / whdotwo_i : 1;
                    }

                    surface_c.SetWi(wi_offset, normal_c);

                    const float3 le = r_i.lightType == Light::TYPE::SKY ? 
                        Light::Le_Sky(wi_offset, g_frame.EnvMapDescHeapOffset) :
                        Light::Le_Sun(pos_c, g_frame);
                    target_c_y_i = le * BSDF::Unified(surface_c).f;

                    if(dot(target_c_y_i, target_c_y_i) > 0)
                    {
                        target_c_y_i *= RtRayQuery::Visibility_Ray(pos_c, wi_offset, normal_c, 
                            g_bvh, surface_c.Transmissive());
                    }
                }

                const float targetLum = Math::Luminance(target_c_y_i);
                m_i = Compute_m_i(r_c, targetLum, r_i, jacobian);
            }

            // Shift center to pixel i
            if(r_c.IsValid())
            {
                float3 target = 0;
                float3 wi_offset = r_c.wx;
                float jacobian = 1;

                if(IsShiftInvertible(r_c, surface_i, alpha_min))
                {
                    if(r_c.halfVectorCopyShift)
                    {
                        float3 wh_local = r_c.wx;
                        float3 wh_i = FromTangentFrameToWorld(normal_i, wh_local);

                        wi_offset = reflect(-surface_i.wo, wh_i);
                        float whdotwo_i = abs(dot(surface_i.wo, wh_i));
                        jacobian = whdotwo_i > 0 ? abs(dot(surface_i.wo, wh_i)) / r_c.partialJacobian : 1;
                    }

                    surface_i.SetWi(wi_offset, normal_i);

                    const float3 le = r_c.lightType == Light::TYPE::SKY ? 
                        Light::Le_Sky(wi_offset, g_frame.EnvMapDescHeapOffset) :
                        Light::Le_Sun(pos_i, g_frame);
                    target = le * BSDF::Unified(surface_i).f;

                    if(dot(target, target) > 0)
                    {
                        target *= RtRayQuery::Visibility_Ray(pos_i, wi_offset, normal_i, 
                            g_bvh, surface_i.Transmissive());
                    }
                }

                const float targetLum = Math::Luminance(target);
                Update_m_c(r_c, r_i, targetLum, jacobian);
            }

            if(r_i.IsValid())
            {
                const float w_i = m_i * Math::Luminance(target_c_y_i) * r_i.W;

                this.r_s.Update(w_i, r_i.wx, r_i.lightType, r_i.lobe, r_i.halfVectorCopyShift, 
                    surface_c.whdotwo, target_c_y_i, rng);
            }

            this.M_s += r_i.M;
        }

        void End(Reservoir r_c, inout RNG rng)
        {
            // w_sum = Luminance(target) * W
            const float w_c = this.m_c * r_c.w_sum;
            this.r_s.Update(w_c, r_c.wx, r_c.lightType, r_c.lobe, r_c.halfVectorCopyShift, 
                r_c.partialJacobian, r_c.target, rng);
            this.r_s.M = this.M_s;

            const float targetLum = Math::Luminance(r_s.target);
            this.r_s.W = targetLum > 0 ? this.r_s.w_sum / (targetLum * (1 + this.k)) : 0;
        }

        Reservoir r_s;
        float m_c;
        uint16 M_s;
        uint16 k;
    };
}

#endif