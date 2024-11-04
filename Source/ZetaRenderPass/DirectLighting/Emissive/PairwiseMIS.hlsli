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

        float Compute_m_i(Reservoir r_c, Reservoir r_i, float targetLum, float jacobian)
        {
            const float p_i_y_i = r_i.W > 0 ? r_i.w_sum / r_i.W : 0;
            const float p_c_y_i = targetLum;
            float numerator = r_i.M * p_i_y_i;
            float denom = (numerator / jacobian) + ((float)r_c.M / this.k) * p_c_y_i;
            float m_i = denom > 0 ? numerator / denom : 0;

            return m_i;
        }

        void Update_m_c(Reservoir r_c, Reservoir r_i, float targetLum, float jacobian)
        {
            const float p_i_y_c = targetLum;
            const float p_c_y_c = Math::Luminance(r_c.target);

            const float numerator = r_i.M * p_i_y_c * jacobian;
            const float denom = numerator + ((float)r_c.M / this.k) * p_c_y_c;
            // Note: denom can never be zero, otherwise r_c didn't have a valid sample
            // and this function shouldn't have been called
            this.m_c += 1 - (numerator / denom);
        }

        void Stream(Reservoir r_c, float3 pos_c, float3 normal_c, BSDF::ShadingData surface_c, 
            Reservoir r_i, float3 pos_i, float3 normal_i, BSDF::ShadingData surface_i, 
            float alpha_min, ConstantBuffer<cbFrameConstants> g_frame, 
            RaytracingAccelerationStructure g_bvh, 
            StructuredBuffer<RT::EmissiveTriangle> g_emissives, 
            StructuredBuffer<RT::MeshInstance> g_frameMeshData,
            inout RNG rng)
        {
            float3 target_c_y_i = 0;
            float3 target_i_y_c = 0.0;
            float m_i = 0;

            // m_i
            if(r_i.lightIdx != UINT32_MAX)
            {
                float jacobian_i_to_c = 0;

                if(IsShiftInvertible(r_i, surface_c, alpha_min))
                {
                    float3 wh_c = Math::FromTangentFrameToWorld(normal_c, r_i.wh_local);
                    float3 wh_i = Math::FromTangentFrameToWorld(normal_i, r_i.wh_local);
                    float whdotwo_i = abs(dot(surface_i.wo, wh_i));
                    float whdotwo_c = abs(dot(surface_c.wo, wh_c));
                    jacobian_i_to_c = whdotwo_i > 0 ? whdotwo_c / whdotwo_i : 0;
                    jacobian_i_to_c = r_c.halfVectorCopyShift ? jacobian_i_to_c : 1;

#if USE_HALF_VECTOR_COPY_SHIFT == 1
                    if(r_i.halfVectorCopyShift)
                    {
                        float3 wi_c = reflect(-surface_c.wo, wh_c);

                        BSDFHitInfo hitInfo = FindClosestHit(pos_c, normal_c, wi_c, g_bvh, 
                            g_frameMeshData, surface_c.Transmissive());
                        if(hitInfo.hit)
                        {
                            RT::EmissiveTriangle emissive = g_emissives[hitInfo.emissiveTriIdx];
                            float3 le = Light::Le_EmissiveTriangle(emissive, hitInfo.bary, 
                                g_frame.EmissiveMapsDescHeapOffset);

                            const float3 vtx1 = Light::DecodeEmissiveTriV1(emissive);
                            const float3 vtx2 = Light::DecodeEmissiveTriV2(emissive);
                            float3 lightNormal = cross(vtx1 - emissive.Vtx0, vtx2 - emissive.Vtx0);
                            float twoArea = length(lightNormal);
                            lightNormal = dot(lightNormal, lightNormal) == 0 ? 0 : lightNormal / twoArea;
                            lightNormal = emissive.IsDoubleSided() && dot(-wi_c, lightNormal) < 0 ? 
                                -lightNormal : lightNormal;

                            // Light is backfacing
                            if(dot(-wi_c, lightNormal) > 0)
                            {
                                float dwdA = saturate(dot(lightNormal, -wi_c)) / (hitInfo.t * hitInfo.t);
                                target_c_y_i = le * dwdA;
                            }

                            surface_c.SetWi(wi_c, normal_c);
                        }
                    }
                    else
#endif
                    {
                        EmissiveData emissive_i = EmissiveData::Init(r_i.lightIdx, r_i.bary, g_emissives);
                        emissive_i.SetSurfacePos(pos_c);
                        float dwdA = emissive_i.dWdA();

                        surface_c.SetWi(emissive_i.wi, normal_c);
                        target_c_y_i = r_i.le * dwdA;
                        if(dot(target_c_y_i, target_c_y_i) > 0)
                        {
                            target_c_y_i *= RtRayQuery::Visibility_Segment(pos_c, emissive_i.wi, emissive_i.t, normal_c, 
                                emissive_i.ID, g_bvh, surface_c.Transmissive());
                        }
                    }

                    target_c_y_i *= BSDF::Unified(surface_c).f;
                }

                const float targetLum = Math::Luminance(target_c_y_i);
                m_i = Compute_m_i(r_c, r_i, targetLum, jacobian_i_to_c);
            }

            // m_c
            float jacobian_c_to_i = 0;

            if(r_c.lightIdx != UINT32_MAX)
            {
                if(IsShiftInvertible(r_c, surface_i, alpha_min))
                {
                    float3 wh_i = Math::FromTangentFrameToWorld(normal_i, r_c.wh_local);
                    float3 wh_c = Math::FromTangentFrameToWorld(normal_c, r_c.wh_local);
                    float whdotwo_i = abs(dot(surface_i.wo, wh_i));
                    float whdotwo_c = abs(dot(surface_c.wo, wh_c));
                    jacobian_c_to_i = whdotwo_c == 0 ? 0 : whdotwo_i / whdotwo_c;
                    jacobian_c_to_i = r_c.halfVectorCopyShift ? jacobian_c_to_i : 1;

#if USE_HALF_VECTOR_COPY_SHIFT == 1
                    if(r_i.halfVectorCopyShift)
                    {
                        float3 wi_i = reflect(-surface_i.wo, wh_i);

                        BSDFHitInfo hitInfo = FindClosestHit(pos_i, normal_i, wi_i, g_bvh, 
                            g_frameMeshData, surface_i.Transmissive());
                        if(hitInfo.hit)
                        {
                            RT::EmissiveTriangle emissive = g_emissives[hitInfo.emissiveTriIdx];
                            float3 le = Light::Le_EmissiveTriangle(emissive, hitInfo.bary, 
                                g_frame.EmissiveMapsDescHeapOffset);

                            const float3 vtx1 = Light::DecodeEmissiveTriV1(emissive);
                            const float3 vtx2 = Light::DecodeEmissiveTriV2(emissive);
                            float3 lightNormal = cross(vtx1 - emissive.Vtx0, vtx2 - emissive.Vtx0);
                            float twoArea = length(lightNormal);
                            lightNormal = dot(lightNormal, lightNormal) == 0 ? 0 : lightNormal / twoArea;
                            lightNormal = emissive.IsDoubleSided() && dot(-wi_i, lightNormal) < 0 ? 
                                -lightNormal : lightNormal;

                            if(dot(-wi_i, lightNormal) > 0)
                            {
                                float dwdA = saturate(dot(lightNormal, -wi_i)) / (hitInfo.t * hitInfo.t);
                                target_i_y_c = le * dwdA;
                            }

                            surface_i.SetWi(wi_i, normal_i);
                        }
                    }
                    else
#endif
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
                        target_i_y_c = r_c.le * dwdA;

                        if(dot(target_i_y_c, target_i_y_c) > 0)
                        {
                            target_i_y_c *= RtRayQuery::Visibility_Segment(pos_i, wi_i, t_i, normal_i, 
                                r_c.lightID, g_bvh, surface_i.Transmissive());
                        }
                    }
                }

                target_i_y_c *= BSDF::Unified(surface_i).f;
            }

            const float targetLum = Math::Luminance(target_i_y_c);
            Update_m_c(r_c, r_i, targetLum, jacobian_c_to_i);

            if(r_i.lightIdx != UINT32_MAX)
            {
                const float w_i = m_i * Math::Luminance(target_c_y_i) * r_i.W;

                if (this.r_s.Update(w_i, r_i.halfVectorCopyShift, r_i.wh_local, 0 /*unused*/, 
                    r_i.lobe, r_i.le, r_i.lightIdx, r_i.bary, rng))
                {
                    r_s.target = target_c_y_i;
                }
            }

            this.M_s += r_i.M;
        }

        void End(Reservoir r_c, inout RNG rng)
        {
            // w_sum = Luminance(target) * W
            const float w_c = this.m_c * r_c.w_sum;
            if (this.r_s.Update(w_c, r_c.halfVectorCopyShift, r_c.wh_local, 0 /*unused*/, 
                r_c.lobe, r_c.le, r_c.lightIdx, r_c.bary, rng))
            {
                r_s.target = r_c.target;
            }

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