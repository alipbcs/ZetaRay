// Refs:
//
// 1. M. Pharr, W. Jakob, and G. Humphreys, Physically Based Rendering: From theory to implementation, Morgan Kaufmann, 2016.
// 2. E. Heitz, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs," Journal of Computer Graphics Techniques, 2014.
// 3. B. Walter, S.R. Marschner1, H. Li, K.E. Torrance, "Microfacet Models for Refraction through Rough Surfaces," in EGSR'07, 2007.
// 4. J. Boksansky, "Crash Course in BRDF Implementation," 2021. [Online]. Available: https://boksajak.github.io/blog/BRDF.
// 5. S. Lagarde and C. de Rousiers, "Moving Frostbite to Physically Based Rendering," 2014.
// 6. Autodesk Standard Surface: https://autodesk.github.io/standard-surface/.
// 7. OpenPBR Surface: https://academysoftwarefoundation.github.io/OpenPBR

#ifndef BSDF_H
#define BSDF_H

#include "Sampling.hlsli"
#include "../../ZetaCore/Core/Material.h"

// Conventions:
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

// To check against (almost) perfect specular reflection or transmission.
#define MIN_N_DOT_H_SPECULAR 0.99998

// Maximum (linear) roughness to treat surface as specular. Helps avoid numerical-precision issues.
#define MAX_ROUGHNESS_SPECULAR 0.035

// TODO buggy
// If camera is placed in the air, this can be ignored as the correction factor cancels out between 
// refraction into the medium and refraction out of the medium.
#define NON_SYMMTERIC_REFRACTION_CORRECTION 0

// Sample from the isotropic VNDF distribution. Should be faster as building a local 
// coordinate system is not needed, but here it's slightly slower.
#define USE_ISOTROPIC_VNDF 0

#define USE_OREN_NAYAR 1
#define ENERGY_PRESERVING_OREN_NAYAR 1

namespace BSDF
{
    enum class LOBE : uint16_t
    {
        DIFFUSE_R = 0,     // Diffuse reflection
        DIFFUSE_T = 1,     // Diffuse transmission
        GLOSSY_R = 2,      // Specular reflection or glossy GGX BRDF
        GLOSSY_T = 3,      // Specular transmission or glossy GGX BTDF
        ALL = 4
    };

    uint16_t LobeToValue(LOBE t)
    {
        switch(t)
        {
            case LOBE::DIFFUSE_R:
                return 0;
            case LOBE::DIFFUSE_T:
                return 1;
            case LOBE::GLOSSY_R:
                return 2;
            case LOBE::GLOSSY_T:
                return 3;
            case LOBE::ALL:
            default:
                return 4;
        }
    }

    LOBE LobeFromValue(uint x)
    {
        if(x == 0)
            return LOBE::DIFFUSE_R;
        if(x == 1)
            return LOBE::DIFFUSE_T;
        if(x == 2)
            return LOBE::GLOSSY_R;
        if(x == 3)
            return LOBE::GLOSSY_T;

        return LOBE::ALL;
    }

    float LobeAlpha(float alpha, LOBE lt)
    {
        if(lt == LOBE::GLOSSY_R || lt == LOBE::GLOSSY_T)
            return alpha;

        return 1.0f;
    }

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

    // ndotwi: Cosine of angle between incident vector and normal
    // eta = eta_i / eta_t (eta_i is IOR of incident medium and eta_t IOR of trasnmitted medium)
    float Fresnel_Dielectric(float ndotwi, float eta)
    {
        float sinTheta_iSq = saturate(mad(-ndotwi, ndotwi, 1.0f));
        float sinTheta_tSq = mad(-eta * eta, sinTheta_iSq, 1.0f);

        // TIR
        if(sinTheta_tSq < 0)
            return 1;

        float cosTheta_t = sqrt(sinTheta_tSq);
        float r_parallel = mad(-eta, cosTheta_t, ndotwi) / mad(eta, cosTheta_t, ndotwi);
        float r_perp = mad(eta, ndotwi, -cosTheta_t) / mad(eta, ndotwi, cosTheta_t);
        float2 r = float2(r_parallel, r_perp);

        return 0.5 * dot(r, r);
    }

    //--------------------------------------------------------------------------------------
    // Normal Distribution Function
    //--------------------------------------------------------------------------------------

    float GGX(float ndotwh, float alphaSq)
    {
        float denom = mad(ndotwh * ndotwh, alphaSq - 1.0f, 1.0f);
        return alphaSq / max(PI * denom * denom, 1e-8);
    }

    //--------------------------------------------------------------------------------------
    // Smith Shadowing-Masking Functions
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

    // Ref: E. Heitz and J. Dupuy, "Implementing a Simple Anisotropic Rough Diffuse 
    // Material with Stochastic Evaluation," 2015.
    float SmithHeightCorrelatedG2OverG1_GGX(float alphaSq, float ndotwi, float ndotwo)
    {
        float G1wi = SmithG1_GGX(alphaSq, ndotwi);
        float G1wo = SmithG1_GGX(alphaSq, ndotwo);

        return G1wi / (G1wi + G1wo - G1wi * G1wo);
    }

    // Approximates how closely the specular lobe dominant direction is aligned with 
    // the reflected direction. Dominant direction tends to shift away from reflected 
    // direction as roughness or view angle increases (off-specular peak).
    float MicrofacetBRDFGGXSmithDominantFactor(float ndotwo, float roughness)
    {
        float a = 0.298475f * log(39.4115f - 39.0029f * roughness);
        float f = pow(1 - ndotwo, 10.8649f) * (1 - a) + a;

        return saturate(f);
    }

    // Approximates the directional-hemispherical reflectance of the microfacet BRDF 
    // with GGX distribution.
    // Ref: https://github.com/boksajak/brdf/
    float3 GGXReflectanceApprox(float3 f0, float alpha, float ndotwo)
    {
        const float2x2 A = float2x2(0.995367f, -1.38839f,
            -0.24751f, 1.97442f);

        const float3x3 B = float3x3(1.0f, 2.68132f, 52.366f,
            16.0932f, -3.98452f, 59.3013f,
            -5.18731f, 255.259f, 2544.07f);

        const float2x2 C = float2x2(-0.0564526f, 3.82901f,
            16.91f, -11.0303f);

        const float3x3 D = float3x3(1.0f, 4.11118f, -1.37886f,
            19.3254f, -28.9947f, 16.9514f,
            0.545386f, 96.0994f, -79.4492f);

        const float alpha2 = alpha * alpha;
        const float alpha3 = alpha * alpha2;
        const float ndotwo2 = ndotwo * ndotwo;
        const float ndotwo3 = ndotwo * ndotwo2;

        const float E = dot(mul(A, float2(1.0f, ndotwo)), float2(1.0f, alpha));
        const float F = dot(mul(B, float3(1.0f, ndotwo, ndotwo3)), float3(1.0f, alpha, alpha3));
        const float G = dot(mul(C, float2(1.0f, ndotwo)), float2(1.0f, alpha));
        const float H = dot(mul(D, float3(1.0f, ndotwo2, ndotwo3)), float3(1.0f, alpha, alpha3));

        // Turn the bias off for near-zero specular 
        const float biasModifier = saturate(dot(f0, float3(0.333333f, 0.333333f, 0.333333f)) * 50.0f);

        const float bias = max(0.0f, (E / F)) * biasModifier;
        const float scale = max(0.0f, (G / H));

        return bias + scale * f0;
    }

    //--------------------------------------------------------------------------------------
    // Sampling
    //--------------------------------------------------------------------------------------

    // Samples half vector wh in a coordinate system where z is aligned with the shading 
    // normal. PDF is GGX(wh) * max(0, whdotwo) * G1(ndotwo) / ndotwo.
    // Ref: J. Dupuy and A. Benyoub, "Sampling Visible GGX Normals with Spherical Caps," 
    // High Performance Graphics, 2023.
    float3 SampleGGXVNDF(float3 wo, float alpha_x, float alpha_y, float2 u)
    {
        // Section 3.2: transforming the view direction to the hemisphere configuration
        float3 Vh = normalize(float3(alpha_x * wo.x, alpha_y * wo.y, wo.z));

        // Sample a spherical cap in (-Vh.z, 1]
        float phi = TWO_PI * u.x;
        float z = mad((1.0f - u.y), (1.0f + Vh.z), -Vh.z);
        float sinTheta = sqrt(saturate(1.0f - z * z));
        float x = sinTheta * cos(phi);
        float y = sinTheta * sin(phi);
        float3 c = float3(x, y, z);

        // Compute halfway direction
        float3 Nh = c + Vh;

        // Section 3.4: transforming the normal back to the ellipsoid configuration
        float3 Ne = normalize(float3(alpha_x * Nh.x, alpha_y * Nh.y, max(0.0f, Nh.z)));

        return Ne;
    }

    // Ref: https://auzaiffe.wordpress.com/2024/04/15/vndf-importance-sampling-an-isotropic-distribution/
    float3 SampleGGXVNDF_Isotropic(float3 wi, float alpha, float3 n, float2 u)
    {
        // decompose the floattor in parallel and perpendicular components
        float3 wi_z = -n * dot(wi, n);
        float3 wi_xy = wi + wi_z;

        // warp to the hemisphere configuration
        float3 wiStd = -normalize(alpha * wi_xy + wi_z);

        // sample a spherical cap in (-wiStd.z, 1]
        float wiStd_z = dot(wiStd, n);
        float z = 1.0 - u.y * (1.0 + wiStd_z);
        float sinTheta = sqrt(saturate(1.0f - z * z));
        float phi = TWO_PI * u.x - PI;
        float x = sinTheta * cos(phi);
        float y = sinTheta * sin(phi);
        float3 cStd = float3(x, y, z);

        // reflect sample to align with normal
        float3 up = float3(0, 0, 1.000001); // Used for the singularity
        float3 wr = n + up;
        float3 c = dot(wr, cStd) * wr / wr.z - cStd;

        // compute halfway direction as standard normal
        float3 wmStd = c + wiStd;
        float3 wmStd_z = n * dot(n, wmStd);
        float3 wmStd_xy = wmStd_z - wmStd;

        // return final normal
        return normalize(alpha * wmStd_xy + wmStd_z);
    }

    float3 SampleGGXMicrofacet(float3 wo, float alpha, float3 shadingNormal, float2 u)
    {
#if USE_ISOTROPIC_VNDF == 0
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
        float3 wh = mad(whLocal.x, b1, mad(whLocal.y, b2, whLocal.z * shadingNormal));
#else
        float3 wh = SampleGGXVNDF_Isotropic(wo, alpha, shadingNormal, u);
#endif

        return wh;
    }

    float3 SampleDiffuseRefl(float3 normal, float2 u, out float pdf)
    {
        float3 wiLocal = Sampling::SampleCosineWeightedHemisphere(u, pdf);

        float3 T;
        float3 B;
        Math::revisedONB(normal, T, B);
        float3 wiWorld = mad(wiLocal.x, T, mad(wiLocal.y, B, wiLocal.z * normal));

        return wiWorld;
    }

    float3 SampleDiffuseRefl(float3 normal, float2 u)
    {
        float unused;
        return SampleDiffuseRefl(normal, u, unused);
    }

    //--------------------------------------------------------------------------------------
    // Energy-preserving Oren-Nayar
    //--------------------------------------------------------------------------------------

    // G(theta) = sin(theta) (theta - sin(theta) cos(theta) - 2 / 3)
    //          + (2 / 3) tan(theta) (1 - sin^3(theta))
    float OrenNayar_G(float cosTheta)
    {
        float sinThetaSq = saturate(1 - cosTheta * cosTheta);
        float sinTheta = sqrt(sinThetaSq);
        float theta = Math::ArcCos(cosTheta);
        float tanTheta = sinTheta / cosTheta;
        float multiplier_sin = mad(-sinTheta, cosTheta, theta - (2.0f / 3.0f));
        float multiplier_tan = (2.0f / 3.0f) * mad(-sinTheta, sinThetaSq, 1.0f);

        return sinTheta * multiplier_sin + tanTheta * multiplier_tan;
    }

    //--------------------------------------------------------------------------------------
    // Utility structure with data needed for BSDF evaluation
    //--------------------------------------------------------------------------------------

    struct ShadingData
    {
        static ShadingData Init()
        {
            ShadingData ret;
            ret.wo = 0;
            ret.backfacing_wo = false;
            ret.ndotwo = 0;
            ret.metallic = false;
            ret.alpha = 0;
            ret.diffuseReflectance_Fr0_TrCol = 0;
            ret.eta = DEFAULT_ETA_I / DEFAULT_ETA_T;
            ret.specTr = false;
            ret.trDepth = 0;
            ret.subsurface = 0;
            ret.specular = true;
            ret.g_wo = 0;

            return ret;
        }

        static ShadingData Init(float3 shadingNormal, float3 wo, bool metallic, float roughness, 
            float3 baseColor, float eta_i = DEFAULT_ETA_I, float eta_t = DEFAULT_ETA_T, 
            bool specTr = false, half transmissionDepth = 0, half subsurface = 0)
        {
            ShadingData si;

            si.wo = wo;
            float ndotwo = dot(shadingNormal, wo);
            si.backfacing_wo = ndotwo <= 0;
            // Clamp to a small value to avoid division by zero
            si.ndotwo = max(ndotwo, 1e-5f);

            si.metallic = metallic;
            si.alpha = roughness * roughness;
            si.diffuseReflectance_Fr0_TrCol = baseColor;
            si.eta = eta_i / eta_t;
            si.specTr = specTr;
            si.trDepth = transmissionDepth;
            si.subsurface = subsurface;

            // Specular reflection and microfacet model are different surface reflection
            // models, but both are handled by the microfacet routines below for convenience.
            si.specular = roughness <= MAX_ROUGHNESS_SPECULAR;
            // Precompute as it only depends on wo
            si.g_wo = !metallic && !specTr ? OrenNayar_G(max(ndotwo, 1e-4f)) : 0;

            return si;
        }

        static bool IsSpecular(float roughness)
        {
            return roughness <= MAX_ROUGHNESS_SPECULAR;
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
            this.wodotwi = dot(wo, wi);
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

            bool isInvalid = this.backfacing_wo || (this.specTr && (this.ndotwh == 0 || this.whdotwo == 0));
            this.invalid = isInvalid || ndotwi_n >= 0 || !Transmissive() || this.metallic;

            this.ndotwi = max(abs(ndotwi_n), 1e-5f);
            this.wodotwi = dot(wo, wi);
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
            bool backfacing_t = ndotwi_n >= 0 || !Transmissive() || this.metallic;

            bool isInvalid = this.backfacing_wo || (this.specTr && (this.ndotwh == 0 || this.whdotwo == 0));
            this.invalid = isInvalid || (this.reflection && backfacing_r) || (!this.reflection && backfacing_t);

            // Clamp to a small value to avoid division by zero
            this.ndotwi = max(abs(ndotwi_n), 1e-5f);
            this.whdotwi = abs(dot(wh, wi));
            this.wodotwi = dot(wo, wi);
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
            float s = this.reflection ? 1 : this.eta;
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

        float3 Fresnel()
        {
            // When specular, wh = n and wh.wo = n.wo. Besides, when there are
            // other lobes besides microfacet (e.g. diffuse reflection) and
            // surface is specular, Fresnel must be computed with n.wo.
            float cosIncident = this.specular ? this.ndotwo : this.whdotwo;

            // Use Schlick's approximation for metals
            if(this.metallic)
            {
                float3 fr0 = this.metallic ? this.diffuseReflectance_Fr0_TrCol : 
                    DielectricF0(this.eta);

                return FresnelSchlick(fr0, cosIncident);
            }

            return Fresnel_Dielectric(cosIncident, 1 / this.eta);
        }

        float3 Fresnel(float3 fr0)
        {
            float cosIncident = this.specular ? this.ndotwo : this.whdotwo;

            if(this.metallic)
                return FresnelSchlick(fr0, cosIncident);

            return Fresnel_Dielectric(cosIncident, 1 / this.eta);
        }

        void Regularize()
        {
            // i.e. to linear roughness in [~0.3, 0.5]
            this.alpha = this.alpha < 0.25f ? clamp(2.0f * this.alpha, 0.1f, 0.25f) : this.alpha;
            this.specular = false;
        }

        float3 TransmissionTint()
        {
            return trDepth > 0 ? 1 : diffuseReflectance_Fr0_TrCol;
        }

        bool ThinWalled()
        {
            return subsurface > 0;
        }

        bool Transmissive()
        {
            return specTr || ThinWalled();
        }

        float SpecularReflectance()
        {
            // Reflectance only depends on wo. When surface is specular, it's equal 
            // to Fresnel, which requires the incident and half vectors. But in that 
            // case, n = wh and Fresnel is computed by passing n.wo, which doesn't 
            // depend on any SetWi*() calls.
            float fr0 = DielectricF0(this.eta);
            return this.specular ? this.Fresnel(fr0).x : 
                GGXReflectanceApprox(fr0, this.alpha, this.ndotwo).x;
        }

        // Roughness textures actually contain an "interface" value of roughness that 
        // is perceptively linear. That value needs to be remapped to the alpha value 
        // that's used in BSDF equations. Literature suggests alpha = roughness^2.
        float alpha;

        float3 wo;
        float ndotwi;
        float ndotwo;
        float ndotwh;
        float whdotwi;
        float whdotwo;
        float wodotwi;
        float g_wo;
        // Union of:
        //  - Diffuse reflectance for dielectrics
        //  - Fresnel at normal incidence for metals
        //  - Transmission color for dielectrics with specular transmission
        float3 diffuseReflectance_Fr0_TrCol;
        float eta;      // eta_i / eta_t
        bool specTr;
        bool metallic;
        bool specular;  // delta BSDF
        bool backfacing_wo;
        bool invalid;
        bool reflection;
        half trDepth;
        half subsurface;
    };

    bool IsLobeValid(ShadingData surface, LOBE lt)
    {
        if(lt == LOBE::ALL)
            return true;

        if(surface.metallic && (lt != LOBE::GLOSSY_R))
            return false;

        if(!surface.specTr && (lt == LOBE::GLOSSY_T))
            return false;

        if(surface.specTr && (lt == LOBE::DIFFUSE_R))
            return false;

        return true;
    }

    //--------------------------------------------------------------------------------------
    // Diffuse Reflection
    //--------------------------------------------------------------------------------------

    // Note that multiplication by n.wi is not part of the Lambertian BRDF and is included 
    // for convenience.
    float3 Lambertian(ShadingData surface)
    {
        return ONE_OVER_PI * surface.ndotwi * surface.diffuseReflectance_Fr0_TrCol;
    }

    // Lambertain BRDF times n.wi divided by pdf of cosine-weighted hemisphere sampling
    float3 LambertianOverPdf(ShadingData surface)
    {
        return surface.diffuseReflectance_Fr0_TrCol;
    }

    // Pdf of cosine-weighted hemisphere sampling
    float DiffuseReflectiondPdf(ShadingData surface)
    {
        return surface.ndotwi * ONE_OVER_PI;
    }

    // Refs: Improved Oren-Nayar model from https://mimosa-pudica.net/improved-oren-nayar.html, 
    // energy-preserving multi-scattering term from the OpenPBR specs.
    template<bool AccountForMultiScattering>
    float3 OrenNayar(ShadingData surface)
    {
        // Reduces to Lambertian
        if(surface.alpha == 0)
            return ONE_OVER_PI * surface.ndotwi * surface.diffuseReflectance_Fr0_TrCol;

        float sigma = sqrt(surface.alpha);
        float3 rho = surface.diffuseReflectance_Fr0_TrCol;
        float A = 1.0f / mad(0.287793409f, sigma, 1.0f);
        float B = sigma * A;
        float s_over_t = mad(-surface.ndotwi, surface.ndotwo, surface.wodotwi);
        s_over_t = s_over_t > 0 ? s_over_t / max(surface.ndotwi, surface.ndotwo) : s_over_t;
        float3 f = ONE_OVER_PI * mad(B, s_over_t, A) * rho;
        float3 f_comp = 0;

        if(AccountForMultiScattering && surface.reflection)
        {
            float avgReflectance = mad(0.072488212f, B, A);
            float one_min_avgReflectance = 1 - avgReflectance;
            float tmp = ONE_OVER_PI * (avgReflectance / one_min_avgReflectance);
            float3 rho_ms_over_piSq = tmp / (1 - rho * one_min_avgReflectance);
            rho_ms_over_piSq *= rho * rho;

            float B_over_pi = ONE_OVER_PI * B; 
            float E_wo = mad(B_over_pi, surface.g_wo, A);
            float E_wi = mad(B_over_pi, OrenNayar_G(surface.ndotwi), A);
            f_comp = (1 - E_wo) * (1 - E_wi) * rho_ms_over_piSq;
        }

        return surface.ndotwi * (f + f_comp);
    }

    //--------------------------------------------------------------------------------------
    // GGX Microfacet Model
    //--------------------------------------------------------------------------------------

    // Includes multiplication by n.wi from the rendering equation
    float3 EvalGGXMicrofacetBRDF(ShadingData surface, float3 fr)
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

    // Given the bijective mapping wi = f(wh) where wh denotes half vector,
    // the pdfs are related as follows:
    //      p(wi) = p(wh) / |J_f(wh)|,
    // where J_f() is the Jacobian determinat of f, from which dwi = |J_f(wh)| dwh.
    // Putting everything together, p(wi) = p(wh) * dwh / dwi.
    float JacobianHalfVecToIncident_Tr(ShadingData surface)
    {
        float denom = mad(surface.whdotwo, 1 / surface.eta, surface.whdotwi);
        denom *= denom;
        float dwh_dwi = surface.whdotwi / denom;

        return dwh_dwi;
    }

    // Includes multiplication by n.wi from the rendering equation, but multiplication 
    // by (1 - F) is done later.
    float EvalGGXMicrofacetBTDF(ShadingData surface)
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

        float f = NDF * G2opt * surface.whdotwo;
        float dwh_dwi = JacobianHalfVecToIncident_Tr(surface);
        f *= dwh_dwi;
        f *= surface.ndotwi;
        // f *= 1 / (surface.eta * surface.eta);

        return f;
    }

    // Note: The following returns D(w_h) * max(0, w_o.w_h), where
    // D(w_h) (distribution of visible normals) is defined as:
    //      D(w_h) = G1(w_o) / n.w_o GGX(w_h) max(0, w_o.w_h),
    float GGXMicrofacetPdf(ShadingData surface)
    {
        float wh_pdf = 1;

        if(!surface.specular)
        {
            float alphaSq = surface.alpha * surface.alpha;
            float NDF = BSDF::GGX(surface.ndotwh, alphaSq);
            float G1 = BSDF::SmithG1_GGX(alphaSq, surface.ndotwo);
            wh_pdf = (NDF * G1) / surface.ndotwo;
        }

        return wh_pdf;
    }

    float GGXMicrofacetPdf(float3 normal, float3 wh, inout ShadingData surface)
    {
        surface.ndotwh = saturate(dot(normal, wh));
        return GGXMicrofacetPdf(surface);
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
        if(!surface.specTr)
            return 0;

        if(surface.specular)
            return (surface.ndotwh >= MIN_N_DOT_H_SPECULAR);

        float alphaSq = surface.alpha * surface.alpha;
        float NDF = GGX(surface.ndotwh, alphaSq);
        float G1 = SmithG1_GGX(alphaSq, surface.ndotwo);
        float pdf = (NDF * G1 * surface.whdotwo) / surface.ndotwo;

        float dwh_dwi = JacobianHalfVecToIncident_Tr(surface);
        pdf *= dwh_dwi;

        return pdf;
    }

    float3 SampleGGXMicrofacet_Refl(ShadingData surface, float3 shadingNormal, float2 u)
    {
        // Reminder: reflect(w, n) = reflect(w, -n)

        // Fast path for specular surfaces
        if(surface.specular)
            return reflect(-surface.wo, shadingNormal);

        // As a convention, microfacet normals point into the upper hemisphere (in the coordinate
        // system aligned with normal). Since here it's assumed n.wo > 0, this is always true.
        float3 wh = SampleGGXMicrofacet(surface.wo, surface.alpha, shadingNormal, u);

        // Reflect wo about the plane with normal wh (each microsurface is a perfect mirror).
        float3 wi = reflect(-surface.wo, wh);

        return wi;
    }

    float3 SampleGGXMicrofacet_Tr(ShadingData surface, float3 shadingNormal, float2 u)
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

        float3 wh = SampleGGXMicrofacet(surface.wo, surface.alpha, shadingNormal, u);

        // Refract wo about the plane with normal wh using Snell's law.
        float3 wi = refract(-surface.wo, wh, 1 / surface.eta);

        return wi;
    }

    // Includes multiplication by n.wi from the rendering equation
    float3 GGXMicrofacetBRDFOverPdf(ShadingData surface, float3 fr)
    {
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
    float3 GGXMicrofacetBTDFOverPdf(ShadingData surface, float fr)
    {
        if(surface.specular)
        {
            // Note that ndotwi cancels out.
            return (1 - fr) * surface.TransmissionTint();
        }

        // When VNDF is used for sampling the incident direction (wi), the expression 
        //        f * cos(theta) / Pdf(wi)
        // is simplified to (1 - Fr) * G2 / G1.
        float alphaSq = surface.alpha * surface.alpha;
        return SmithHeightCorrelatedG2OverG1_GGX(alphaSq, surface.ndotwi, surface.ndotwo) * 
            (1 - fr) * surface.TransmissionTint();
    }

    //--------------------------------------------------------------------------------------
    // Surface BSDF
    //--------------------------------------------------------------------------------------

    // All routines below include multiplication by n.wi from the rendering equation

    float3 OpaqueBase(ShadingData surface, float fr, float reflectance)
    {
        if(surface.invalid)
            return 0;

        // mul. by reflection is to handle thin walled case
        float3 gloss = EvalGGXMicrofacetBRDF(surface, fr) * surface.reflection;
        float s = surface.subsurface == 0 ? 1 : (float)surface.subsurface * 0.5f;
#if USE_OREN_NAYAR == 1
        float3 diffuse = (1 - reflectance) * s * OrenNayar<ENERGY_PRESERVING_OREN_NAYAR>(surface);
#else
        float3 diffuse = (1 - reflectance) * s * Lambertian(surface);
#endif
        return diffuse + gloss;
    }

    float3 OpaqueBase(ShadingData surface)
    {
        if(surface.invalid)
            return 0;

        float fr0 = DielectricF0(surface.eta);
        float fr = surface.Fresnel(fr0).x;
        float reflectance = surface.specular ? fr : 
            GGXReflectanceApprox(fr0, surface.alpha, surface.ndotwo).x;

        return OpaqueBase(surface, fr, reflectance);
    }

    float3 MetalBase(ShadingData surface)
    {
        if(surface.invalid)
            return 0;

        float3 fr = FresnelSchlick(surface.diffuseReflectance_Fr0_TrCol, 
            surface.whdotwo);
        return EvalGGXMicrofacetBRDF(surface, fr);
    }

    float3 DielectricSpecularTr(ShadingData surface, float fr)
    {
        if(surface.invalid || !surface.specTr)
            return 0;

        float fr0 = DielectricF0(surface.eta);
        float reflectance = surface.specular ? 0 : 
            GGXReflectanceApprox(fr0, surface.alpha, surface.ndotwo).x;

        float gloss = EvalGGXMicrofacetBTDF(surface);
        return (1 - reflectance) * gloss * (1 - fr) * surface.TransmissionTint();
    }

    float3 DielectricSpecularTr(ShadingData surface)
    {
        if(surface.invalid || !surface.specTr)
            return 0;

        float fr = Fresnel_Dielectric(surface.whdotwo, 1 / surface.eta);
        return DielectricSpecularTr(surface, fr);
    }

    // Excludes translucent base. Useful when we know surface doesn't have specular transmission.
    float3 NonTranslucentBase(ShadingData surface, float3 fr, float reflectance)
    {
        if(surface.invalid)
            return 0;

        if(surface.metallic)
            return EvalGGXMicrofacetBRDF(surface, fr);

        return OpaqueBase(surface, fr.x, reflectance);
    }

    float3 NonTranslucentBase(ShadingData surface)
    {
        if(surface.invalid)
            return 0;

        float3 fr0 = surface.metallic ? surface.diffuseReflectance_Fr0_TrCol : 
            DielectricF0(surface.eta);
        float3 fr = surface.Fresnel(fr0);

        if(surface.metallic)
            return EvalGGXMicrofacetBRDF(surface, fr);

        float reflectance = surface.specular ? fr.x : 
            GGXReflectanceApprox(fr0, surface.alpha, surface.ndotwo).x;

        return OpaqueBase(surface, fr.x, reflectance);
    }

    float3 UnifiedBSDF(ShadingData surface, float3 fr, float fr0_d)
    {
        if(surface.invalid)
            return 0;

        float3 glossyRefl = EvalGGXMicrofacetBRDF(surface, fr);

        // Metal
        if(surface.metallic)
            return glossyRefl;

        float reflectance = surface.specular ? fr.x : 
            GGXReflectanceApprox(fr0_d, surface.alpha, surface.ndotwo).x;

        // Opaque Base
        if(!surface.specTr)
        {
            float s = surface.subsurface == 0 ? 1 : (float)surface.subsurface * 0.5f;
#if USE_OREN_NAYAR == 1
            float3 diffuse = (1 - reflectance) * s * OrenNayar<ENERGY_PRESERVING_OREN_NAYAR>(surface);
#else
            float3 diffuse = (1 - reflectance) * s * Lambertian(surface);
#endif

            return diffuse * s + glossyRefl * surface.reflection;
        }

        // Translucent Base
        if(surface.reflection)
            return glossyRefl;

        // For specular, (1 - Fresnel) factor is already accounted for
        reflectance = surface.specular ? 0 : reflectance;
        float glossyTr = EvalGGXMicrofacetBTDF(surface);

        return (1 - reflectance) * glossyTr * (1 - fr.x) * surface.TransmissionTint();
    }

    float3 UnifiedBSDF(ShadingData surface, out float fr_d)
    {
        float3 fr0 = surface.metallic ? surface.diffuseReflectance_Fr0_TrCol : 
            DielectricF0(surface.eta);
        float3 fr = surface.Fresnel(fr0);
        fr_d = fr.x;

        return UnifiedBSDF(surface, fr, fr0.x);
    }

    float3 UnifiedBSDF(ShadingData surface)
    {
        float3 fr0 = surface.metallic ? surface.diffuseReflectance_Fr0_TrCol : 
            DielectricF0(surface.eta);
        float3 fr = surface.Fresnel(fr0);

        return UnifiedBSDF(surface, fr, fr0.x);
    }
}

#endif