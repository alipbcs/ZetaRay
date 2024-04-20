#ifndef RT_H
#define RT_H

#include "../../ZetaCore/RayTracing/RtCommon.h"
#include "../Common/BSDF.hlsli"

namespace RT
{
    // Ref: T. Akenine-Moller, J. Nilsson, M. Andersson, C. Barre-Brisebois, R. Toth 
    // and T. Karras, "Texture Level of Detail Strategies for Real-Time Ray Tracing," in 
    // Ray Tracing Gems 1, 2019.
    struct RayCone
    {
        static RayCone InitFromPrimaryHit(float pixelSpreadAngle, float t)
        {
            RayCone r;

            r.Width = half(pixelSpreadAngle * t);
            r.SpreadAngle = half(pixelSpreadAngle);

            return r;
        }

        void NewHit(float t)
        {
            this.Width = half(this.Width + t * this.SpreadAngle);
        }

        void UpdateConeGeometry_Refl(float curvature)
        {
            this.SpreadAngle = half(this.SpreadAngle + 2.0f * curvature);
        }

        void UpdateConeGeometry_Tr(float3 wo, float3 wi, float3 normal, float curvature, float eta, 
            float3 p, float3 origin, float3 origin_l, float3 origin_u)
        {
            // Form coordinate system around normal
            float3 m = wo - dot(normal, wo) * normal;
            m = normalize(m);
            float3 u = cross(m, normal);

            // Find off-axis cone directions
            float4 quat = Math::RotationQuaternion_Acute(u, 0.5f * this.SpreadAngle);
            float4 quat_inv = Math::InverseRotationQuaternion(quat);

            float3 d_u = Math::RotateVector(-wo, quat);
            float3 d_l = Math::RotateVector(-wo, quat_inv);

            // Find refraction of off-axis cone directions with perturbed normals
            quat = Math::RotationQuaternion_Acute(u, curvature);
            quat_inv = Math::InverseRotationQuaternion(quat);

            float3 n_l = Math::RotateVector(normal, quat);
            float3 n_u = Math::RotateVector(normal, quat_inv);

            float3 t_u = refract(-wo, n_u, 1 / eta);
            float3 t_l = refract(-wo, n_l, 1 / eta);

            // Intersect off-axis cone directions with tangent plane around hit point
            float pdotNormal = dot(normal, p);

            float numerator_l = pdotNormal - dot(normal, origin_l);
            float denom_l = dot(normal, d_l);
            float dist_l = numerator_l / denom_l;
            float3 p_l = origin_l + dist_l * d_l;

            float numerator_u = pdotNormal - dot(normal, origin_u);
            float denom_u = dot(normal, d_u);
            float dist_u = numerator_u / denom_u;
            float3 p_u = origin_u + dist_u * d_u;

            float3 q = m - dot(m, wi) * wi;
            q = normalize(q);
            float newWidth = 0;

            // Find intersection of rays p + wq and p_u + w_u t_u. Skip in case of TIR.
            if(dot(t_u, t_u) != 0)
            {
                // Solve for x = (A^T A)^-1 A^T b where A = [t_u -q]
                float3 b = p - p_u;
                float tdotq = dot(t_u, q);
                float det = 1 - tdotq * tdotq;
                float3 A1 = float3(tdotq * t_u.x - q.x,
                    tdotq * t_u.y - q.y,
                    tdotq * t_u.z - q.z);
                newWidth += det == 0 ? 0 : abs(dot(A1, b) / det);
            }

            // Find intersection of rays p + wq and p_l + w_l t_l. Skip in case of TIR.
            if(dot(t_l, t_l) != 0)
            {
                // Solve for x = (A^T A)^-1 A^T b where A = [t_l -q]
                float3 b = p - p_l;
                float tdotq = dot(t_l, q);
                float det = 1 - tdotq * tdotq;
                float3 A1 = float3(tdotq * t_l.x - q.x,
                    tdotq * t_l.y - q.y,
                    tdotq * t_l.z - q.z);
                newWidth += det == 0 ? 0 : abs(dot(A1, b) / det);
            }

            this.Width = half(newWidth);
            this.SpreadAngle = half(Math::ArcCos(dot(t_u, t_l)) * sign(t_u.x * t_l.y - t_u.y * t_l.x));
        }

        void UpdateConeGeometry_Tr_CurvatureIs0(float3 wo, float3 wi, float3 normal, float eta, 
            float3 p, float3 origin)
        {
            if(this.SpreadAngle == 0)
                return;

            float3 m = wo - dot(normal, wo) * normal;
            m = normalize(m);
            float3 u = cross(m, normal);

            float4 quat = Math::RotationQuaternion_Acute(u, 0.5f * this.SpreadAngle);
            float4 quat_inv = Math::InverseRotationQuaternion(quat);

            float3 d_u = Math::RotateVector(-wo, quat);
            float3 d_l = Math::RotateVector(-wo, quat_inv);

            float3 t_u = refract(d_u, normal, 1 / eta);
            float3 t_l = refract(d_l, normal, 1 / eta);

            float3 v = normal - dot(normal, wo) * wo;
            v = normalize(v);
            float3 origin_u = origin + v * this.Width;
            float3 origin_l = origin - v * this.Width;
            float pdotNormal = dot(normal, p);

            float numerator_l = pdotNormal - dot(normal, origin_l);
            float denom_l = dot(normal, d_l);
            float dist_l = numerator_l / denom_l;
            float3 p_l = origin_l + dist_l * d_l;

            float numerator_u = pdotNormal - dot(normal, origin_u);
            float denom_u = dot(normal, d_u);
            float dist_u = numerator_u / denom_u;
            float3 p_u = origin_u + dist_u * d_u;

            float3 q = m - dot(m, wi) * wi;
            q = normalize(q);

            // Approximate solution but faster
#if 1
            float2 newWidth = float2(dot(p_u - p, q), dot(p_l - p, q));
            newWidth *= newWidth;
            newWidth = sqrt(newWidth);
            this.Width = half(newWidth.x * (dot(t_u, t_u) != 0) + newWidth.y * (dot(t_l, t_l) != 0));

            // Exact solution
#else
            float newWidth = 0;

            if(dot(t_u, t_u) != 0)
            {
                float3 b = p - p_u;
                float tdotq = dot(t_u, q);
                float det = 1 - tdotq * tdotq;
                float3 A1 = float3(tdotq * t_u.x - q.x,
                    tdotq * t_u.y - q.y,
                    tdotq * t_u.z - q.z);
                newWidth += det == 0 ? 0 : abs(dot(A1, b) / det);
            }

            if(dot(t_l, t_l) != 0)
            {
                float3 b = p - p_l;
                float tdotq = dot(t_l, q);
                float det = 1 - tdotq * tdotq;
                float3 A1 = float3(tdotq * t_l.x - q.x,
                    tdotq * t_l.y - q.y,
                    tdotq * t_l.z - q.z);
                newWidth += det == 0 ? 0 : abs(dot(A1, b) / det);
            }

            this.Width = half(newWidth);
#endif

            this.SpreadAngle = half(Math::ArcCos(dot(t_u, t_l)) * sign(t_u.x * t_l.y - t_u.y * t_l.x));
        }

        void UpdateConeGeometry_Tr_PrimaryHit(float3 wo, float3 wi, float3 normal, float curvature, float eta, 
            float3 hitPoint, float3 origin)
        {
            this.UpdateConeGeometry_Tr(wo, wi, normal, curvature, eta, hitPoint, origin, origin, origin);
        }

        void UpdateConeGeometry_Tr(float3 wo, float3 wi, float3 normal, float curvature, float eta, 
            float3 hitPoint, float3 origin)
        {
            float3 v = normal - dot(normal, wo) * normal;
            v = normalize(v);
            float3 origin_l = origin + v * this.Width;
            float3 origin_u = origin - v * this.Width;

            this.UpdateConeGeometry_Tr(wo, wi, normal, curvature, eta, hitPoint, origin, origin_l, origin_u);
        }

        static float Lambda(float3 v0, float3 v1, float3 v2, float2 t0, float2 t1, float2 t2, float ndotwo)
        {
            float P_a = length(cross((v1 - v0), (v2 - v0)));
            float T_a = abs((t1.x - t0.x) * (t2.y - t0.y) - (t2.x - t0.x) * (t1.y - t0.y));

            float lambda = T_a;
            lambda /= (P_a * ndotwo * ndotwo);

            return lambda;
        }

        float MipmapLevel(float lambda, float w, float h)
        {
            float mip = lambda * w * h * this.Width * this.Width;
            return 0.5f * log2(mip);
        }

        half Width;
        half SpreadAngle;
    };

    // basis*: view-space basis vectors in world-space coordinates
    float3 GeneratePinholeCameraRay(uint2 pixel, float2 renderDim, float aspectRatio, float tanHalfFOV,
        float3 viewBasisX, float3 viewBasisY, float3 viewBasisZ, float2 jitter = 0)
    {
        float2 uv = (pixel + 0.5f + jitter) / renderDim;
        float2 ndc = Math::NDCFromUV(uv);
        ndc *= tanHalfFOV;
        ndc.x *= aspectRatio;

        float3 dirV = float3(ndc, 1);
        float3 dirW = dirV.x * viewBasisX + dirV.y * viewBasisY + dirV.z * viewBasisZ;

        return normalize(dirW);
    }

    // Geometric Normal points outward for rays exiting the surface, else should be flipped.
    // Ref: C. Wachter and N. Binder, "A Fast and Robust Method for Avoiding Self-Intersection", in Ray Tracing Gems 1, 2019.
    float3 OffsetRayRTG(float3 pos, float3 geometricNormal)
    {
        static const float origin = 1.0f / 32.0f;
        static const float float_scale = 1.0f / 65536.0f;
        static const float int_scale = 256.0f;

        int3 of_i = int_scale * geometricNormal;

        float3 p_i = float3(asfloat(asint(pos.x) + ((pos.x < 0) ? -of_i.x : of_i.x)),
            asfloat(asint(pos.y) + ((pos.y < 0) ? -of_i.y : of_i.y)),
            asfloat(asint(pos.z) + ((pos.z < 0) ? -of_i.z : of_i.z)));

        float3 adjusted = float3(abs(pos.x) < origin ? pos.x + float_scale * geometricNormal.x : p_i.x,
            abs(pos.y) < origin ? pos.y + float_scale * geometricNormal.y : p_i.y,
            abs(pos.z) < origin ? pos.z + float_scale * geometricNormal.z : p_i.z);
        
        return adjusted;
    }

    float3 OffsetRay2(float3 origin, float3 dir, float3 normal, float minNormalBias = 5e-6f, float maxNormalBias = 1e-4)
    {
        const float maxBias = max(minNormalBias, maxNormalBias);
        const float normalBias = lerp(maxBias, minNormalBias, saturate(dot(normal, dir)));

        return origin + dir * normalBias;
    }

    // Returns (n_1 p_1) / (n_1 p_1 + n_2 p_2) * f / p_1
    template<typename T>
    T BalanceHeuristic(float p_1, float p_2, T f, float n_1 = 1, float n_2 = 1)
    {
        return n_1 / max(n_1 * p_1 + n_2 * p_2, 1e-6f) * f;
    }

    // Returns (n_1 p_1)^2 / ((n_1 p_1)^2 + (n_2 p_2)^2) * f / p_1
    template<typename T>
    T PowerHeuristic(float p_1, float p_2, T f, float n_1 = 1, float n_2 = 1)
    {
        float a = n_1 * p_1;
        float b = n_2 * p_2;

        return ((n_1 * n_1 * p_1) / max(a * a + b * b, 1e-6f)) * f;
    }

    //--------------------------------------------------------------------------------------
    // Utility sampling routines
    //--------------------------------------------------------------------------------------

    float3 SampleUnifiedBSDF_NoDiffuse(float3 normal, BSDF::ShadingData surface, inout RNG rng, 
        out float3 f, out float pdf)
    {
        pdf = 1;
        float wh_pdf = 1;
        float3 wh = surface.specular ? normal : 
            BSDF::SampleMicrofacet(surface.wo, surface.alpha, normal, rng.Uniform2D());

        // VNDF
        if(!surface.specular)
        {
            surface.ndotwh = saturate(dot(normal, wh));
            float alphaSq = surface.alpha * surface.alpha;
            float NDF = BSDF::GGX(surface.ndotwh, alphaSq);
            float G1 = BSDF::SmithG1_GGX(alphaSq, surface.ndotwo);
            wh_pdf = (NDF * G1) / surface.ndotwo;
        }

        float3 wi_r = reflect(-surface.wo, wh);
        surface.SetWi_Refl(wi_r, normal, wh);
        f = BSDF::UnifiedBRDF(surface);

        // Account for change of density from half vector to incident vector
        if(!surface.specular)
            pdf = wh_pdf / 4.0f;

        if(surface.IsMetallic() || !surface.HasSpecularTransmission())
            return wi_r;

        float3 wi_t = refract(-surface.wo, wh, 1 / surface.eta);
        float fr0 = BSDF::DielectricF0(surface.eta);
        float whdotwx = surface.eta < 1 ? abs(dot(wh, wi_t)) : saturate(dot(wh, surface.wo));
        // For total internal reflection, all the incident light is reflected
        float fr = dot(wi_t, wi_t) == 0 ? 1 : BSDF::FresnelSchlick_Dielectric(fr0, whdotwx);

        float3 wi;

        if (rng.Uniform() < fr)
        {
            wi = wi_r;
            pdf *= fr;
        }
        else
        {
            wi = wi_t;
            pdf = 1 - fr;

            surface.SetWi_Tr(wi_t, normal, wh);
            f = BSDF::DielectricBTDF(surface);

            if(!surface.specular)
            {
                pdf *= wh_pdf * surface.whdotwo;

                // Account for change of density from half vector to incident vector
                float denom = mad(surface.whdotwi, surface.eta, surface.whdotwo);
                denom *= denom;
                float dwm_dwi = surface.whdotwi / denom;
                pdf *= dwm_dwi;
            }
        }

        return wi;
    }

    float UnifiedBSDFPdf_NoDiffuse(float3 normal, BSDF::ShadingData surface, float3 wi)
    {
        surface.SetWi(wi, normal);
        float wh_pdf = 1;

        if(!surface.specular)
        {
            float alphaSq = surface.alpha * surface.alpha;
            float NDF = BSDF::GGX(surface.ndotwh, alphaSq);
            float G1 = BSDF::SmithG1_GGX(alphaSq, surface.ndotwo);
            wh_pdf = (NDF * G1) / surface.ndotwo;
        }

        if(surface.IsMetallic() || !surface.HasSpecularTransmission())
        {
            if(!surface.specular)
                return wh_pdf / 4.0f;

            return surface.ndotwh >= MIN_N_DOT_H_SPECULAR;
        }

        float fr = surface.Fresnel_Dielectric();
        float pdf = surface.specular ? surface.ndotwh >= MIN_N_DOT_H_SPECULAR : 1;

        if(surface.reflection)
            return fr * pdf * (surface.specular ? 1 : (wh_pdf / 4.0f));

        pdf *= 1 - fr;

        if(!surface.specular)
        {
            pdf *= wh_pdf * surface.whdotwo;

            float denom = mad(surface.whdotwi, surface.eta, surface.whdotwo);
            denom *= denom;
            float dwm_dwi = surface.whdotwi / denom;
            pdf *= dwm_dwi;
        }

        return pdf;
    }
}

#endif