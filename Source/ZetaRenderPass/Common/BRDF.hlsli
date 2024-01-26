// Refs:
//
// 1. M. Pharr, W. Jakob, and G. Humphreys, Physically Based Rendering: From theory to implementation, Morgan Kaufmann, 2016.
// 2. E. Heitz, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs," Journal of Computer Graphics Techniques, 2014.
// 3. B. Walter, S.R. Marschner1, H. Li, K.E. Torrance, "Microfacet Models for Refraction through Rough Surfaces," in EGSR'07, 2007.
// 4. J. Boksansky, "Crash Course in BRDF Implementation," 2021. [Online]. Available: https://boksajak.github.io/blog/BRDF.
// 5. S. Lagarde and C. de Rousiers, "Moving Frostbite to Physically Based Rendering," 2014.
// 6. Autodesk Standard Surface: https://autodesk.github.io/standard-surface/.

#ifndef BSDF_H
#define BSDF_H

#include "Sampling.hlsli"
#include "Math.hlsli"

// Illustration of unified BSDF model
//
//  ---------------------------------------------------
// |   Specular   |         Specular                   |
// |  Reflection  |        Reflection                  |
// |   (Metal)    |       (Dielectric)                 |
//  ---------------------------|-----------------------
//                             |   1 - Fresnel
//                             +
//                            / \
//            transmission   /   \   1 - transmission
//                          /     \
//  ----------------------------------------------------
// |   Specular Transmission   |   Diffuse Reflection   |
//  ----------------------------------------------------

// Conventions:
//
// Motivated by the fact that in path tracing, paths are built in reverse, starting from the 
// camera, the following conventions are used:
//
//  - All directions point out of the surface
//  - Suffix o refers to outgoing direction (e.g. wo) - for the first path vertex, the same as eye vector
//  - Suffix i refers to incident direction (e.g. wi)
//  - Suffix t refers to transmitted direction
//  - Normal is assumed to be on the same side as wo. Therefore, it should be reversed for double-sided
//    surfaces if ray hit the backside.
//  - "metallic" property is binary - a blend of metallic BRDF and dielectric BRDF is not performed.
//  - alpha = roughness^2
//  - eta = eta_i / eta_t

// About 1 degrees. To check against (almost) perfect reflection.
#define MIN_N_DOT_H_SPECULAR 0.9998157121216442

// Maximum (linear) roughness to treat surface as specular. Helps avoid numerical-precision issues.
#define MAX_ROUGHNESS_SPECULAR 0.035

// TODO buggy
// If camera is placed in the air, this can be ignored as the correction factor cancels out between 
// refraction into the medium and refraction out of the medium.
#define NON_SYMMTERIC_REFRACTION_CORRECTION 0

namespace BSDF
{
    //--------------------------------------------------------------------------------------
    // Fresnel
    //--------------------------------------------------------------------------------------

    // Note that F0(eta_1, eta_2) = F0(eta_2, eta_1).
    // Inputs:
    //   - eta_i: Refractive index of material travelled through by incident ray
    //   - eta_t: Refractive index of material travelled through by transmitted ray
    float DielectricF0(float eta_i, float eta_t)
    {
        float f0 = (eta_i - eta_t) / (eta_i + eta_t);
        return f0 * f0;
    }

    // eta = eta_i / eta_t
    float DielectricF0(float eta)
    {
        float f0 = (eta - 1) / (eta + 1);
        return f0 * f0;
    }

    // Follows the same input conventions as hlsl refract().
    // eta = eta_i / eta_t
    bool TotalInternalReflection(float3 w, float3 normal, float eta)
    {
        if(eta <= 1)
            return false;

        float cos_theta_i = dot(w, normal);
        return (eta * eta * (1 - cos_theta_i * cos_theta_i)) >= 1;
    }

    // whdotwx: 
    //   - When eta_i < eta_t, cosine of angle between half vector and incident ray
    //   - When eta_t < eta_i, cosine of angle between half vector and transmitted ray
    float3 FresnelSchlick(float3 F0, float whdotwx)
    {
        float tmp = 1.0f - whdotwx;
        float tmpSq = tmp * tmp;
        return mad(tmpSq * tmpSq * tmp, 1 - F0, F0);
    }

    // Specialization for dielectrics where Fresnel is assumed to be wavelength-independent
    float FresnelSchlick_Dielectric(float F0, float whdotwx)
    {
        float tmp = 1.0f - whdotwx;
        float tmpSq = tmp * tmp;
        return mad(tmpSq * tmpSq * tmp, 1 - F0, F0);
    }

    //--------------------------------------------------------------------------------------
    // Normal Distribution Function
    //--------------------------------------------------------------------------------------
    float GGX(float ndotwh, float alphaSq)
    {
        float denom = ndotwh * ndotwh * (alphaSq - 1.0f) + 1.0f;
        return alphaSq / max(PI * denom * denom, 1e-8);
    }

    //--------------------------------------------------------------------------------------
    // Smith Geometry Shadowing-Masking Functions
    //--------------------------------------------------------------------------------------
    // G1 is the Smith masking function. Output is in [0, 1].
    //
    // theta            = angle between wi or wo with normal
    // ndotx            = n.wi or n.wo
    // GGXLambda        = (sqrt(1 + alpha^2 tan^2(theta)) - 1) / 2
    // G1(GGXLambda)    = 1 / (1 + GGXLambda)
    float SmithG1_GGX(float alphaSq, float ndotx)
    {
        float ndotxSq = ndotx * ndotx;
        float tanThetaSq = (1.0f - ndotxSq) / ndotxSq;
        return 2.0f / (sqrt(mad(alphaSq, tanThetaSq, 1.0f)) + 1.0f);
    }

    // G2 is the height-correlated Smith shadowing-masking function defined as:
    //      1 / (1 + SmithG1_GGX(alpha^2, ndotwo) + SmithG1_GGX(alpha^2, ndotwi))
    float SmithHeightCorrelatedG2_GGX(float alphaSq, float ndotwi, float ndotwo)
    {
        float denomWo = ndotwi * sqrt((-ndotwo * alphaSq + ndotwo) * ndotwo + alphaSq);
        float denomWi = ndotwo * sqrt((-ndotwi * alphaSq + ndotwi) * ndotwi + alphaSq);

        return (2.0f * ndotwo * ndotwi) / (denomWo + denomWi);
    }

    // G2 can be further optimized for microfacet evaluation since some terms cancel out 
    // as follows:
    //
    //  - BRDF: Move 1 / (4.0 * ndotwi * ndotwo) from BRDF to G2 to return
    //    G2 / (4.0 * ndotwi * ndotwo).
    //  - BTDF: Move 1 / (ndotwi * ndotwo) from BTDF to G2 to return
    //    G2 / (ndotwi * ndotwo).
    template<int n>
    float SmithHeightCorrelatedG2_GGX_Opt(float alphaSq, float ndotwi, float ndotwo)
    {
        float denomWo = ndotwi * sqrt((-ndotwo * alphaSq + ndotwo) * ndotwo + alphaSq);
        float denomWi = ndotwo * sqrt((-ndotwi * alphaSq + ndotwi) * ndotwi + alphaSq);

        return (0.5f * n) / (denomWo + denomWi);
    }

    // Ref: E. Heitz and J. Dupuy, "Implementing a Simple Anisotropic Rough Diffuse Material 
    // with Stochastic Evaluation," 2015.
    float SmithHeightCorrelatedG2OverG1_GGX(float alphaSq, float ndotwi, float ndotwo)
    {
        float G1wi = SmithG1_GGX(alphaSq, ndotwi);
        float G1wo = SmithG1_GGX(alphaSq, ndotwo);

        return G1wi / (G1wi + G1wo - G1wi * G1wo);
    }

    // Approximates how closely the specular lobe dominant direction is aligned with the reflected
    // direction. Dominant direction tends to shift away from reflected direction as roughness
    // or view angle increases (off-specular peak).
    float MicrofacetBRDFGGXSmithDominantFactor(float ndotwo, float roughness)
    {
        float a = 0.298475f * log(39.4115f - 39.0029f * roughness);
        float f = pow(1 - ndotwo, 10.8649f) * (1 - a) + a;

        return saturate(f);
    }

    //--------------------------------------------------------------------------------------
    // Sampling Distribution of Visible Normals (VNDF)
    //--------------------------------------------------------------------------------------
    // Samples half vector wh in a coordinate system where z is aligned with the shading normal.
    // PDF is GGX(wh) * max(0, whdotwo) * G1(ndotwo) / ndotwo.
    // Ref: J. Dupuy and A. Benyoub, "Sampling Visible GGX Normals with Spherical Caps," High Performance Graphics, 2023.
    float3 SampleGGXVNDF(float3 wo, float alpha_x, float alpha_y, float2 u)
    {
        // Section 3.2: transforming the view direction to the hemisphere configuration
        float3 Vh = normalize(float3(alpha_x * wo.x, alpha_y * wo.y, wo.z));

        // sample a spherical cap in (-wi.z, 1]
        float phi = TWO_PI * u.x;
        float z = mad((1.0f - u.y), (1.0f + wo.z), -wo.z);
        float sinTheta = sqrt(saturate(1.0f - z * z));
        float x = sinTheta * cos(phi);
        float y = sinTheta * sin(phi);
        float3 c = float3(x, y, z);
        // compute halfway direction;
        float3 Nh = c + Vh;

        // Section 3.4: transforming the normal back to the ellipsoid configuration
        float3 Ne = normalize(float3(alpha_x * Nh.x, alpha_y * Nh.y, max(0.0f, Nh.z)));

        return Ne;
    }

    float3 SampleMicrofacet(float3 wo, float alpha, float3 shadingNormal, float2 u)
    {
        // Build an orthonormal basis C around the normal such that it points towards +Z.
        float3 b1;
        float3 b2;
        Math::revisedONB(shadingNormal, b1, b2);

        // Transform wo from world space to C. M = [b1 b2 n] goes from C to world space, so 
        // we need its inverse. Since M is an orthogonal matrix, its inverse is just its transpose.
        float3x3 worldToLocal = float3x3(b1, b2, shadingNormal);
        float3 woLocal = mul(worldToLocal, wo);

        float3 whLocal = SampleGGXVNDF(woLocal, alpha, alpha, u);

        // Go from local space back to world space.
        float3 wh = whLocal.x * b1 + whLocal.y * b2 + whLocal.z * shadingNormal;

        return wh;
    }

    //--------------------------------------------------------------------------------------
    // Utility structure with data needed for BSDF evaluation
    //--------------------------------------------------------------------------------------

    struct ShadingData
    {
        static ShadingData Init(float3 shadingNormal, float3 wo, bool metallic, float roughness, 
            float3 baseColor, float eta_i = 1.5f, float eta_t = 1.0f, float transmission = 0.0)
        {
            ShadingData si;

            si.wo = wo;
            float ndotwo = dot(shadingNormal, wo);
            si.backfacing_wo = ndotwo <= 0;
            // Clamp to a small value to avoid division by zero
            si.ndotwo = max(ndotwo, 1e-5f);

            si.metallic = metallic;
            si.alpha = roughness * roughness;
            si.diffuseReflectance_Fr0_Metal = baseColor;
            si.eta = eta_i / eta_t;
            si.transmission = transmission;

            // Specular reflection and microfacet model are different surface reflection
            // models, but both are handled by the microfacet routines below for convenience.
            si.specular = roughness <= MAX_ROUGHNESS_SPECULAR;

            return si;
        }

        void SetWi_Refl(float3 wi, float3 shadingNormal, float3 wh)
        {
            this.reflection = true;

            float ndotwi_n = dot(shadingNormal, wi);
            this.ndotwh = saturate(dot(shadingNormal, wh));
            this.whdotwo = saturate(dot(wh, wo));
            this.whdotwi = this.whdotwo;

            bool isInvalid = this.backfacing_wo || this.ndotwh == 0 || this.whdotwo == 0;
            this.invalid = isInvalid || ndotwi_n <= 0;

            this.ndotwi = max(ndotwi_n, 1e-5f);
        }

        void SetWi_Refl(float3 wi, float3 shadingNormal)
        {
            float3 wh = normalize(wi + this.wo);
            this.SetWi_Refl(wi, shadingNormal, wh);
        }

        void SetWi_Tr(float3 wi, float3 shadingNormal)
        {
            float3 wh = normalize(mad(wi, eta, this.wo));
            wh = this.eta > 1 ? -wh : wh;
            this.SetWi_Tr(wi, shadingNormal, wh);
        }

        void SetWi_Tr(float3 wi, float3 shadingNormal, float3 wh)
        {
            this.reflection = false;

            float ndotwi_n = dot(shadingNormal, wi);
            this.ndotwh = saturate(dot(shadingNormal, wh));
            this.whdotwo = saturate(dot(wh, wo));
            this.whdotwi = abs(dot(wh, wi));

            bool isInvalid = this.backfacing_wo || this.ndotwh == 0 || this.whdotwo == 0;
            this.invalid = isInvalid || ndotwi_n >= 0 || this.transmission == 0 || this.metallic;

            this.ndotwi = max(abs(ndotwi_n), 1e-5f);
        }

        void SetWi(float3 wi, float3 shadingNormal, float3 wh)
        {
            float ndotwi_n = dot(shadingNormal, wi);
            this.reflection = ndotwi_n >= 0;

            // Backfacing half vectors are invalid
            this.ndotwh = saturate(dot(shadingNormal, wh));
            this.whdotwo = saturate(dot(wh, wo));

            // reflection - wi and wo have to be on the same side as normal
            bool backfacing_r = ndotwi_n <= 0;
            // transmission - wi and wo have to be on the opposite sides w.r.t. normal. Furthermore,
            // in case of TIR, wi = 0, from which n.wi = 0 and therefore also covered by condition below.
            bool backfacing_t = ndotwi_n >= 0 || this.transmission == 0 || this.metallic;

            bool isInvalid = this.backfacing_wo || this.ndotwh == 0 || this.whdotwo == 0;
            this.invalid = isInvalid || (this.reflection && backfacing_r) || (!this.reflection && backfacing_t);

            // Clamp to a small value to avoid division by zero
            this.ndotwi = max(abs(ndotwi_n), 1e-5f);
            this.whdotwi = abs(dot(wh, wi));
        }

        void SetWi(float3 wi, float3 shadingNormal)
        {
            // Transmission happens when wi and wo are on opposite sides of the surface
            float ndotwi_n = dot(shadingNormal, wi);
            this.reflection = ndotwi_n >= 0;

            // For relfection:
            //    wh = normalize(wi + wo)
            // For transmission: 
            //  - wh = normalize(eta * wi + wo),    eta > 1
            //  - wh = normalize(-eta * wi - wo),   eta < 1
            float s = this.reflection ? 1 : eta;
            float3 wh = normalize(mad(wi, s, this.wo));
#if 0
            wh = this.reflection || this.eta > 1 ? wh : -wh;
            // For transmission, the half vector computed above always point towards the 
            // incident material, whereas the convention here is to assume normal is on 
            // the same side as wo.
            wh = !this.reflection ? -wh : wh;
#else
            // Combines two expressions above
            wh = !this.reflection && this.eta > 1 ? -wh : wh;
#endif
            SetWi(wi, shadingNormal, wh);
        }

        bool IsMetallic()
        {
            return this.metallic;
        }

        bool HasSpecularTransmission()
        {
            return this.transmission > 0;
        }

        float3 Fresnel()
        {
            // When eta > 1 (e.g. water to air), the Schlick approximation has to be used with
            // the transmission angle.
            float whdotwx = this.reflection || this.eta < 1 ? this.whdotwi : this.whdotwo;
            float3 fr0 = IsMetallic() ? this.diffuseReflectance_Fr0_Metal : DielectricF0(this.eta);
            return FresnelSchlick(fr0, whdotwx);
        }

        float Fresnel_Dielectric()
        {
            float whdotwx = this.reflection || this.eta < 1 ? this.whdotwi : this.whdotwo;
            float fr0 = DielectricF0(this.eta);
            return FresnelSchlick_Dielectric(fr0, whdotwx);
        }

        // Roughness textures actually contain an "interface" value of roughness that is perceptively 
        // linear. That value needs to be remapped to the alpha value that's used in BRDF equations. 
        // Literature suggests alpha = roughness^2.
        float alpha;

        float3 wo;
        float ndotwi;
        float ndotwo;
        float ndotwh;
        float whdotwi;
        float whdotwo;
        // Diffuse reflectance for dielectrics, Fresnel at normal incidence for metals
        float3 diffuseReflectance_Fr0_Metal;
        float eta;      // eta_i / eta_t
        float transmission;
        bool metallic;
        bool specular;  // delta BSDF
        bool backfacing_wo;
        bool invalid;
        bool reflection;
    };

    //--------------------------------------------------------------------------------------
    // Lambertian
    //--------------------------------------------------------------------------------------

    // Note that multiplication by n.wi is not part of the Lambertian BRDF and is included for convenience.
    float3 LambertianBRDF(ShadingData surface)
    {
        return ONE_OVER_PI * surface.ndotwi * surface.diffuseReflectance_Fr0_Metal;
    }

    // Pdf of cosine-weighted hemisphere sampling
    float LambertianBRDFPdf(ShadingData surface)
    {
        return surface.ndotwi * ONE_OVER_PI;
    }

    // Lambertain BRDF times n.wi divided by pdf of cosine-weighted hemisphere sampling
    float3 LambertianBRDFDivPdf(ShadingData surface)
    {
        return surface.diffuseReflectance_Fr0_Metal;
    }

    float3 SampleLambertianBRDF(float3 normal, float2 u, out float pdf)
    {
        float3 wiLocal = Sampling::SampleCosineWeightedHemisphere(u, pdf);

        float3 T;
        float3 B;
        Math::revisedONB(normal, T, B);
        float3 wiWorld = wiLocal.x * T + wiLocal.y * B + wiLocal.z * normal;

        return wiWorld;
    }    

    //--------------------------------------------------------------------------------------
    // Microfacet Models
    //--------------------------------------------------------------------------------------

    // Includes multiplication by n.wi from the rendering equation
    float3 MicrofacetBRDFGGXSmith(ShadingData surface, float3 fr)
    {
        if(surface.invalid)
            return 0;

        if(surface.specular)
        {
            // For specular surfaces, total radiance reflected back towards w_o (L_o(w_o)) 
            // should be F * L_i(w_r), where w_r = reflect(-w_o, n). Plugging into the rendering 
            // equation:
            //      L_o(w_o) = /int f(w_o, w_i) * L_i(w_i) * ndotwi dw_i = F * L_i(w_r).
            // Now in order for the above to hold, we must have
            //      f(w_o, w_i) = F * delta(n - wh) / ndotwi
            // Note that ndotwi cancels out.
            return (surface.ndotwh >= MIN_N_DOT_H_SPECULAR) * fr;
        }

        float alphaSq = surface.alpha * surface.alpha;
        float NDF = GGX(surface.ndotwh, alphaSq);
        float G2DivDenom = SmithHeightCorrelatedG2_GGX_Opt<1>(alphaSq, surface.ndotwi, surface.ndotwo);
        float f = NDF * G2DivDenom * surface.ndotwi;

        return f * fr;
    }

    // Includes multiplication by n.wi from the rendering equation, but multiplication 
    // by (1 - F) is done later.
    float MicrofacetBTDFGGXSmith(ShadingData surface)
    {
        if(surface.specular)
        {
            // For specular surfaces, total radiance reflected back towards w_o (L_o(w_o)) 
            // should be (1 - F) * L_i(w_t), where w_t = refract(-w_o, n). Plugging into the 
            // rendering equation:
            //      L_o(w_o) = /int f(w_o, w_i) * L_i(w_i) * ndotwi dw_i = (1 - F) * L_i(w_t).
            // Now in order for the above to hold, we must have
            //      f(w_o, w_i) = (1 - F) * delta(n - wh) / ndotwi
            // Note that ndotwi cancels out.
            float f = surface.ndotwh >= MIN_N_DOT_H_SPECULAR;
            // f *= 1 / (surface.eta * surface.eta);
            return f;
        }

        float alphaSq = surface.alpha * surface.alpha;
        float NDF = GGX(surface.ndotwh, alphaSq);
        float G2opt = SmithHeightCorrelatedG2_GGX_Opt<4>(alphaSq, surface.ndotwi, surface.ndotwo);

        float denom = mad(surface.whdotwi, surface.eta, surface.whdotwo);
        denom *= denom;
        float dwm_dwi = surface.whdotwi / denom;

        float f = NDF * G2opt * surface.whdotwo;
        f *= dwm_dwi;
        f *= surface.ndotwi;
        // f *= 1 / (surface.eta * surface.eta);

        return f;
    }

    // Evaluates distribution of visible normals for reflection
    //      VNDF(wh) = GGX(wh) * wh.wo * G1(n.wo) / n.wo.
    //
    // After correction for change of variable from wh to wi, it becomes
    //      VNDF(wi) = VNDF(wh) * 1 / (4 * wh.wi)
    //               = GGX(wh) * G1(n.wo) / (4 * n.wo)
    // (Note that wh.wo = wh.wi).
    float VNDFReflectionPdf(ShadingData surface)
    {
        if(surface.specular)
            return (surface.ndotwh >= MIN_N_DOT_H_SPECULAR);

        float alphaSq = surface.alpha * surface.alpha;
        float NDF = GGX(surface.ndotwh, alphaSq);
        float G1 = SmithG1_GGX(alphaSq, surface.ndotwo);
        float pdf = (NDF * G1) / (4.0f * surface.ndotwo);
    
        return pdf;
    }

    // Evaluates distribution of visible normals for transmission
    //      VNDF(wh) = GGX(wh) * wh.wo * G1(n.wo) / n.wo.
    //
    // After correction for change of variable from wh to wi, it becomes
    //      VNDF(wi) = VNDF(wh) * wh.wi / (wh.wi + wh.wo * eta)^2
    //               = GGX(wh) * wh.wo * wh.wi * G1(n.wo) / (n.wo * (wh.wi + wh.wo * eta)^2)
    float VNDFTransmissionPdf(ShadingData surface)
    {
        if(!surface.HasSpecularTransmission())
            return 0;

        if(surface.specular)
            return (surface.ndotwh >= MIN_N_DOT_H_SPECULAR);

        float alphaSq = surface.alpha * surface.alpha;
        float NDF = GGX(surface.ndotwh, alphaSq);
        float G1 = SmithG1_GGX(alphaSq, surface.ndotwo);
        float pdf = (NDF * G1 * surface.whdotwo) / surface.ndotwo;

        float denom = mad(surface.whdotwi, surface.eta, surface.whdotwo);
        denom *= denom;
        float dwm_dwi = surface.whdotwi / denom;
        pdf *= dwm_dwi;

        return pdf;
    }

    float3 SampleMicrofacetBRDF(ShadingData surface, float3 shadingNormal, float2 u)
    {
        // Reminder: reflect(w, n) = reflect(w, -n)

        // Fast path for specular surfaces
        if(surface.specular)
            return reflect(-surface.wo, shadingNormal);

        // As a convention, microfacet normals point into the upper hemisphere (in the coordinate
        // system aligned with normal). Since here it's assumed n.wo > 0, this is always true.
        float3 wh = SampleMicrofacet(surface.wo, surface.alpha, shadingNormal, u);

        // Reflect wo about the plane with normal wh (each microsurface is a perfect mirror).
        float3 wi = reflect(-surface.wo, wh);

        return wi;
    }

    float3 SampleMicrofacetBTDF(ShadingData surface, float3 shadingNormal, float2 u)
    {
        // Note that: 
        //  - refract(w, n, eta) requires w.n > 0 and ||n|| = 1. Here it's assumed 
        //    n.wo > 0, so this is always true.
        //  - For sampling, trasnmitted direction is known (wo) and the goal is to sample 
        //    incident direction (wi). Therefore, refract() should be called with inputs
        //    for the reverse direction -- eta = 1 / eta.

        // Fast path for specular surfaces
        if(surface.specular)
            return refract(-surface.wo, shadingNormal, 1 / surface.eta);

        float3 wh = SampleMicrofacet(surface.wo, surface.alpha, shadingNormal, u);

        // Refract wo about the plane with normal wh using the Snell's law.
        float3 wi = refract(-surface.wo, wh, 1 / surface.eta);

        return wi;
    }

    // Includes multiplication by n.wi from the rendering equation
    float3 MicrofacetBRDFDivPdf(ShadingData surface)
    {
        float3 fr = surface.Fresnel();

        if(surface.specular)
        {
            // Note that ndotwi cancels out.
            return fr;
        }

        // When VNDF is used for sampling the incident direction (wi), the expression 
        //        f * cos(theta) / Pdf(wi)
        // is simplified to Fr * G2 / G1.
        float alphaSq = surface.alpha * surface.alpha;
        return SmithHeightCorrelatedG2OverG1_GGX(alphaSq, surface.ndotwi, surface.ndotwo) * fr;
    }

    // Includes multiplication by n.wi from the rendering equation
    float MicrofacetBTDFDivPdf(ShadingData surface)
    {
        float fr = surface.Fresnel_Dielectric();

        if(surface.specular)
        {
            // Note that ndotwi cancels out.
            return 1 - fr;
        }

        // When VNDF is used for sampling the incident direction (wi), the expression 
        //        f * cos(theta) / Pdf(wi)
        // is simplified to (1 - Fr) * G2 / G1.
        float alphaSq = surface.alpha * surface.alpha;
        return SmithHeightCorrelatedG2OverG1_GGX(alphaSq, surface.ndotwi, surface.ndotwo) * (1 - fr);
    }

    //--------------------------------------------------------------------------------------
    // Aggregate BSDFs
    //--------------------------------------------------------------------------------------

    // Includes multiplication by n.wi from the rendering equation
    float3 DielectricBRDF(ShadingData surface, float fr)
    {
        float3 microfacetBrdf = MicrofacetBRDFGGXSmith(surface, fr);
        float3 diffuse = (1 - surface.transmission) * (1 - fr) * LambertianBRDF(surface);

        return diffuse + microfacetBrdf;
    }

    float3 DielectricBRDF(ShadingData surface)
    {
        if(surface.invalid)
            return 0;

        float fr = surface.Fresnel_Dielectric();
        return DielectricBRDF(surface, fr);
    }

    // Includes multiplication by n.wi from the rendering equation
    float3 DielectricBTDF(ShadingData surface, float fr)
    {
        float microfacetBtdf = MicrofacetBTDFGGXSmith(surface);
        return microfacetBtdf * surface.transmission * (1 - fr) * surface.diffuseReflectance_Fr0_Metal;
    }

    float3 DielectricBTDF(ShadingData surface)
    {
        if(surface.invalid)
            return 0;

        float fr = surface.Fresnel_Dielectric();
        return DielectricBTDF(surface, fr);
    }

    // Includes multiplication by n.wi from the rendering equation
    float3 UnifiedBRDF(ShadingData surface)
    {
        if(surface.invalid)
            return 0;

        float3 fr = surface.Fresnel();

        if(surface.IsMetallic())
            return MicrofacetBRDFGGXSmith(surface, fr);

        return DielectricBRDF(surface, fr.x);
    }

    // Includes multiplication by n.wi from the rendering equation
    float3 UnifiedBSDF_Dielectric(ShadingData surface)
    {
        if(surface.invalid)
            return 0;

        float fr = surface.Fresnel_Dielectric();

        if(surface.reflection)
            return DielectricBRDF(surface, fr);

        return DielectricBTDF(surface, fr);
    }

    // Includes multiplication by n.wi from the rendering equation
    float3 UnifiedBSDF(ShadingData surface)
    {
        if(surface.invalid)
            return 0;

        float3 fr = surface.Fresnel();

        if(surface.IsMetallic())
            return MicrofacetBRDFGGXSmith(surface, fr);

        if(surface.reflection)
            return DielectricBRDF(surface, fr.x);

        return DielectricBTDF(surface, fr.x);
    }
}

#endif