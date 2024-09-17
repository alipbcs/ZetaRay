#ifndef RT_H
#define RT_H

#include "../../ZetaCore/RayTracing/RtCommon.h"
#include "Math.hlsli"
#include "Sampling.hlsli"

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
            r.Width = (half)(pixelSpreadAngle * t);
            r.SpreadAngle = (half)(pixelSpreadAngle);

            return r;
        }

        void NewHit(float t)
        {
            this.Width = (half)((float)this.Width + t * (float)this.SpreadAngle);
        }

        void UpdateConeGeometry_Refl(float curvature)
        {
            this.SpreadAngle = (half)((float)this.SpreadAngle + 2.0f * curvature);
        }

        void UpdateConeGeometry_Tr(float3 wo, float3 wi, float3 normal, float curvature, float eta, 
            float3 p, float3 origin, float3 origin_l, float3 origin_u)
        {
            // Form coordinate system around normal
            float3 m = wo - dot(normal, wo) * normal;
            m = normalize(m);
            float3 u = cross(m, normal);

            // Find off-axis cone directions
            float4 quat = Math::RotationQuaternion_Acute(u, 0.5f * (float)this.SpreadAngle);
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

            this.Width = (half)newWidth;
            this.SpreadAngle = (half)(Math::ArcCos(dot(t_u, t_l)) * sign(t_u.x * t_l.y - t_u.y * t_l.x));
        }

        void UpdateConeGeometry_Tr_CurvatureIs0(float3 wo, float3 wi, float3 normal, float eta, 
            float3 p, float3 origin)
        {
            if(this.SpreadAngle == 0)
                return;

            float3 m = wo - dot(normal, wo) * normal;
            m = normalize(m);
            float3 u = cross(m, normal);

            float4 quat = Math::RotationQuaternion_Acute(u, 0.5f * (float)this.SpreadAngle);
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
            this.Width = (half)(newWidth.x * (dot(t_u, t_u) != 0) + newWidth.y * (dot(t_l, t_l) != 0));

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

            this.Width = (half)newWidth;
#endif

            this.SpreadAngle = (half)(Math::ArcCos(dot(t_u, t_l)) * sign(t_u.x * t_l.y - t_u.y * t_l.x));
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
            float mip = lambda * w * h * (float)this.Width * (float)this.Width;
            return 0.5f * log2(mip);
        }

        half Width;
        half SpreadAngle;
    };

    // basis*: Camera basis vectors in world-space coordinates
    float3 GeneratePinholeCameraRay(uint2 pixel, float2 renderDim, float aspectRatio, float tanHalfFOV,
        float3 viewBasisX, float3 viewBasisY, float3 viewBasisZ, float2 jitter)
    {
        float2 uv = (pixel + 0.5f + jitter) / renderDim;
        float2 ndc = Math::NDCFromUV(uv);
        float3 dirV = float3(ndc.x * aspectRatio * tanHalfFOV, ndc.y * tanHalfFOV, 1);
        float3 dirW = mad(dirV.x, viewBasisX, mad(dirV.y, viewBasisY, dirV.z * viewBasisZ));

        return normalize(dirW);
    }

    float3 GeneratePinholeCameraRay_CS(uint2 pixel, float2 renderDim, float aspectRatio, float tanHalfFOV, 
        float2 jitter)
    {
        float2 uv = (pixel + 0.5f + jitter) / renderDim;
        float2 ndc = Math::NDCFromUV(uv);
        float3 dirV = float3(ndc.x * aspectRatio * tanHalfFOV, ndc.y * tanHalfFOV, 1);

        return dirV;
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

    float3 OffsetRay2(float3 origin, float3 dir, float3 normal, float minNormalBias = 5e-6f, 
        float maxNormalBias = 1e-4)
    {
        const float maxBias = max(minNormalBias, maxNormalBias);
        const float normalBias = lerp(maxBias, minNormalBias, saturate(dot(normal, dir)));

        return origin + dir * normalBias;
    }

    // Returns (n_1 p_1) / (n_1 p_1 + n_2 p_2) * f / p_1
    template<typename T>
    T BalanceHeuristic(float p_1, float p_2, T f, float n_1 = 1, float n_2 = 1)
    {
        float denom = n_1 * p_1 + n_2 * p_2;
        if(denom == 0)
            return 0;

        return (n_1 * f) / denom;
    }

    // Returns (n_1 p_1) / (n_1 p_1 + n_2 p_2 + n_3 p_3) * f / p_1
    template<typename T>
    T BalanceHeuristic3(float p_1, float p_2, float p_3, T f, float n_1 = 1, float n_2 = 1, 
        float n_3 = 1)
    {
        float denom = n_1 * p_1 + n_2 * p_2 + n_3 * p_3;
        if(denom == 0)
            return 0;

        return (n_1 * f) / denom;
    }

    // Returns (n_1 p_1)^2 / ((n_1 p_1)^2 + (n_2 p_2)^2) * f / p_1
    template<typename T>
    T PowerHeuristic(float p_1, float p_2, T f, float n_1 = 1, float n_2 = 1)
    {
        float a = n_1 * p_1;
        float b = n_2 * p_2;
        float denom = a * a + b * b;
        if(denom == 0)
            return 0;

        return ((n_1 * n_1 * p_1 * f) / denom);
    }

    struct RayDifferentials
    {
        static RayDifferentials Init()
        {
            RayDifferentials ret;
            ret.origin_x = 0;
            ret.dir_x = 0;
            ret.origin_y = 0;
            ret.dir_y = 0;
            ret.uv_grads = 0;

            return ret;
        }

        static RayDifferentials Init(int2 DTid, float2 renderDim, float tanHalfFOV, 
            float aspectRatio, float2 jitter, float3 viewBasis_x, float3 viewBasis_y, 
            float3 viewBasis_z, bool thinLens, float focusDepth, float2 lensSample, 
            float3 origin)
        {
            RayDifferentials ret;

            float3 dir_cs_x = RT::GeneratePinholeCameraRay_CS(int2(DTid.x + 1, DTid.y), 
                renderDim, aspectRatio, tanHalfFOV, jitter);
            float3 dir_cs_y = RT::GeneratePinholeCameraRay_CS(int2(DTid.x, DTid.y - 1), 
                renderDim, aspectRatio, tanHalfFOV, jitter);

            if(thinLens)
            {
                float3 focalPoint_x = focusDepth * dir_cs_x;
                dir_cs_x = focalPoint_x - float3(lensSample, 0);

                float3 focalPoint_y = focusDepth * dir_cs_y;
                dir_cs_y = focalPoint_y - float3(lensSample, 0);
            }

            ret.dir_x = normalize(mad(dir_cs_x.x, viewBasis_x, 
                mad(dir_cs_x.y, viewBasis_y, dir_cs_x.z * viewBasis_z)));
            ret.dir_y = normalize(mad(dir_cs_y.x, viewBasis_x, 
                mad(dir_cs_y.y, viewBasis_y, dir_cs_y.z * viewBasis_z)));

            ret.origin_x = origin;
            ret.origin_y = origin;

            return ret;
        }

        // At each hit point, provides an estimate of how hit position changes w.r.t. one pixel
        // screen movement, e.g. for one pixel horizontal movement:
        //      (p_x - p) ~= (dpdx, dpdy).(dx, dy)
        //                = (dpdx, dpdy).(1, 0)
        //                = dpdx
        // where p_x is offset ray's hit point. To estimate p_x, compute the "virtual" intersection
        // with the tangent plane at the actual hit point of the main ray.
        void dpdx_dpdy(float3 hitPoint, float3 normal, out float3 dpdx, out float3 dpdy)
        {
            float d = dot(normal, hitPoint);

            // Intersection with tangent plan
            float numerator_x = d - dot(normal, this.origin_x);
            float denom_x = dot(normal, this.dir_x);
            float t_x = numerator_x / denom_x;
            float3 hitPoint_x = mad(t_x, dir_x, this.origin_x);

            float numerator_y = d - dot(normal, this.origin_y);
            float denom_y = dot(normal, this.dir_y);
            float t_y = numerator_y / denom_y;
            float3 hitPoint_y = mad(t_y, dir_y, this.origin_y);

            // If ray is parallel to tangent plane, set differential to a large value,
            // so that the highest mip would be used. From this point on, this
            // differential is undefined.
            dpdx = denom_x != 0 ? hitPoint_x - hitPoint : FLT16_MAX;
            dpdy = denom_y != 0 ? hitPoint_y - hitPoint : FLT16_MAX;
            // dpdx = denom_x != 0 ? hitPoint_x - hitPoint : 0;
            // dpdy = denom_y != 0 ? hitPoint_y - hitPoint : 0;
        }

        // Notes: 
        //  - Assumes normal and wo are in the hemisphere
        //  - eta: Relative IOR of wi's medium to wo's medium
        void UpdateRays(float3 p, float3 normal, float3 wi, float3 wo, 
            Math::TriDifferentials triDiffs, float3 dpdx, float3 dpdy, 
            bool transmitted, float eta)
        {
            // First-order approximation of position
            this.origin_x = p + dpdx;
            this.origin_y = p + dpdy;

            float3 dwodx = -this.dir_x - wo;
            float3 dwody = -this.dir_y - wo;
            // Chain rule: 
            //      dndx = dndu * dudx + dndv * dvdx
            //      dndy = dndu * dudy + dndv * dvdy
            float3 dndx = triDiffs.dndu * uv_grads.x + triDiffs.dndv * uv_grads.y;
            float3 dndy = triDiffs.dndu * uv_grads.z + triDiffs.dndv * uv_grads.w;
            // For vector-valued expressions a and b: (a.b)' = a'.b + a.b'.
            // Then, d(n.wo)dx = (dndx).wo + n.(dwodx)
            float dndotWodx = dot(dndx, wo) + dot(normal, dwodx);
            float dndotWody = dot(dndy, wo) + dot(normal, dwody);
            float ndotwo = dot(normal, wo);

            if(!transmitted)
            {
                // Law of specular reflection: wi = -wo + 2(n.wo)n
                // dwidx = -dwodx + 2[d(n.wo)dx n + (n.wo) dndx]
                this.dir_x = wi + mad(2.0f, mad(dndotWodx, normal, ndotwo * dndx), -dwodx);
                this.dir_y = wi + mad(2.0f, mad(dndotWody, normal, ndotwo * dndy), -dwody);
            }
            else
            {
                // Let eta be relative IOR of wo's medium to wi's medium, then
                //      wi = -eta wo + qn 
                // where q = eta (n.wo) - cos(theta_i).
                //
                // Taking the partial derivative w.r.t. x,
                //      dwidx = -eta dwodx + dqdx n + q dndx
                // and
                //      dqdx = eta d(n.wo)dx - d(cos theta_i)dx
                //      d(cos theta_i)dx = (n.wo) d(n.wo)dx / eta^2 cos(theta_i)
                // After simplifying 
                //      dqdx = eta d(n.wo)dx - (n.wo) d(n.wo)dx / eta^2 cos(theta_i)
                //           = d(n.wo)dx (eta - (n.wo) / eta^2 cos(theta_i))
                float eta_relative = 1.0f / eta;
                float ndotwi = abs(dot(normal, wi));

                float q = mad(eta_relative, ndotwo, -ndotwi);
                float common = mad(eta_relative, -ndotwo / ndotwi, 1.0f);
                float dqdx = eta_relative * dndotWodx * common;
                float dqdy = eta_relative * dndotWody * common;

                this.dir_x = mad(-eta_relative, dwodx, wi) + mad(q, dndx, dqdx * normal);
                this.dir_y = mad(-eta_relative, dwody, wi) + mad(q, dndy, dqdy * normal);
            }
        }

        void ComputeUVDifferentials(float3 dpdx, float3 dpdy, float3 dpdu, float3 dpdv)
        {
            // determinat of square matrix A^T A
            float dpduDotdpdu = dot(dpdu, dpdu);
            float dpdvDotdpdv = dot(dpdv, dpdv);
            float dpduDotdpdv = dot(dpdu, dpdv);
            float det = dpduDotdpdu * dpdvDotdpdv - dpduDotdpdv * dpduDotdpdv;
            // A^T A is not invertible
            if (abs(det) < 1e-7f)
            {
                this.uv_grads = 0;
                return;
            }

            // division by determinant happens below
            float2x2 A_T_A_Inv = float2x2(dpdvDotdpdv, -dpduDotdpdv,
                                         -dpduDotdpdv, dpduDotdpdu);
            float2 b_x = float2(dot(dpdu, dpdx), dot(dpdv, dpdx));
            float2 grads_x = mul(A_T_A_Inv, b_x) / det;

            float2 b_y = float2(dot(dpdu, dpdy), dot(dpdv, dpdy));
            float2 grads_y = mul(A_T_A_Inv, b_y) / det;

            bool invalid_x = (this.uv_grads.x == FLT16_MAX) || (dpdx.x == FLT16_MAX);
            bool invalid_y = (this.uv_grads.z == FLT16_MAX) || (dpdy.x == FLT16_MAX);
            this.uv_grads.xy = invalid_x ? FLT16_MAX : grads_x;
            this.uv_grads.zw = invalid_y ? FLT16_MAX : grads_y;
        }

        float3 origin_x;
        float3 dir_x;
        float3 origin_y;
        float3 dir_y;
        // (ddx(uv), ddy(uv))
        float4 uv_grads;
    };
}

#endif