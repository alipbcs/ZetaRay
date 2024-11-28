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
#include "StaticTextureSamplers.hlsli"
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
#define MAX_ROUGHNESS_SPECULAR 0.04
#define MAX_ALPHA_SPECULAR 0.0016

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
        GLOSSY_R = 2,      // Specular or glossy reflection
        GLOSSY_T = 3,      // Specular or glossy transmission
        COAT = 4,          // Coating
        ALL = 5
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
            case LOBE::COAT:
                return 4;
            case LOBE::ALL:
            default:
                return 5;
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
        if(x == 4)
            return LOBE::COAT;

        return LOBE::ALL;
    }

    //--------------------------------------------------------------------------------------
    // Fresnel
    //--------------------------------------------------------------------------------------

    // Note that F0(eta_i, eta_t) = F0(eta_t, eta_i).
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
    // eta = eta_i / eta_t (eta_i is IOR of incident medium and eta_t IOR of transmitted medium)
    float Fresnel_Dielectric(float ndotwi, float eta)
    {
        float sinTheta_iSq = saturate(mad(-ndotwi, ndotwi, 1.0f));
        float cosTheta_tSq = mad(-eta * eta, sinTheta_iSq, 1.0f);

        // TIR
        if(cosTheta_tSq <= 0)
            return 1;

        float cosTheta_t = sqrt(cosTheta_tSq);
        float r_parallel = mad(-eta, cosTheta_t, ndotwi) / mad(eta, cosTheta_t, ndotwi);
        float r_perp = mad(eta, ndotwi, -cosTheta_t) / mad(eta, ndotwi, cosTheta_t);
        float2 r = float2(r_parallel, r_perp);

        return 0.5 * dot(r, r);
    }

    float Fresnel_Dielectric(float ndotwi, float eta, float cosTheta_t)
    {
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
        return alphaSq / (PI * denom * denom);
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
    float SmithG1(float alphaSq, float ndotx)
    {
        float ndotxSq = ndotx * ndotx;
        float tanThetaSq = (1.0f - ndotxSq) / ndotxSq;
        return 2.0f / (sqrt(mad(alphaSq, tanThetaSq, 1.0f)) + 1.0f);
    }

    // G2 is the height-correlated Smith shadowing-masking function defined as:
    //      1 / (1 + SmithG1_GGX(alpha^2, ndotwo) + SmithG1_GGX(alpha^2, ndotwi))
    float SmithHeightCorrelatedG2(float alphaSq, float ndotwi, float ndotwo)
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
    float SmithHeightCorrelatedG2_Opt(float alphaSq, float ndotwi, float ndotwo)
    {
        float denomWo = ndotwi * sqrt((-ndotwo * alphaSq + ndotwo) * ndotwo + alphaSq);
        float denomWi = ndotwo * sqrt((-ndotwi * alphaSq + ndotwi) * ndotwi + alphaSq);

        return (0.5f * n) / (denomWo + denomWi);
    }

    // Ref: E. Heitz and J. Dupuy, "Implementing a Simple Anisotropic Rough Diffuse 
    // Material with Stochastic Evaluation," 2015.
    float SmithHeightCorrelatedG2OverG1(float alphaSq, float ndotwi, float ndotwo)
    {
        float G1wi = SmithG1(alphaSq, ndotwi);
        float G1wo = SmithG1(alphaSq, ndotwo);

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
    float3 GGXReflectance_Metal(float3 f0, float alpha, float ndotwo)
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

    // Approximates the directional-hemispherical reflectance of the microfacet BRDF 
    // with GGX distribution using a precomputed 3D LUT
    float GGXReflectance_Dielectric(float alpha, float ndotwo, float eta)
    {
        Texture3D<float> g_rho = ResourceDescriptorHeap[0];
        float3 uvw;
        uvw.x = ndotwo;
        uvw.y = ((alpha - 0.002025f) / (1.0f - 0.002025f));
        // Don't interpolate between eta < 1 and eta > 1
        // TODO For some reason, following causes frame spikes with dxc versions 1.8.2405 and newer
#if 0
        if(eta > 1.0f)
            eta = max(1.1f, eta);
        else
            eta = min(0.99f, eta);
#endif
        uvw.z = ((eta - 0.5f) / (1.99f - 0.5f));
        float rho = g_rho.SampleLevel(g_samLinearClamp, uvw, 0);
        return saturate(rho);
    }

    //--------------------------------------------------------------------------------------
    // Lambertian
    //--------------------------------------------------------------------------------------

    // Note that multiplication by n.wi is not part of the Lambertian BRDF and is included 
    // for convenience.
    float3 Lambertian(float3 baseColor, float ndotwi)
    {
        return ONE_OVER_PI * ndotwi * baseColor;
    }

    // Lambertain BRDF times n.wi divided by pdf of cosine-weighted hemisphere sampling
    float3 LambertianOverPdf(float3 baseColor)
    {
        return baseColor;
    }

    //--------------------------------------------------------------------------------------
    // Microfacet Models
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

    // Refs: Improved Oren-Nayar model from https://mimosa-pudica.net/improved-oren-nayar.html, 
    // energy-preserving multi-scattering term from the OpenPBR specs.
    // 
    // Params:
    //  - sigma: diffuse roughness
    //  - rho: base color
    //  - g_wo: OrenNayar_G(cos \theta_o)
    template<bool AccountForMultiScattering>
    float3 OrenNayar(float3 rho, float sigma, float ndotwo, float ndotwi, float wodotwi,
        float g_wo, bool reflection)
    {
        // Reduces to Lambertian
        if(sigma == 0)
            return ONE_OVER_PI * ndotwi * rho;

        float A = 1.0f / mad(0.287793409f, sigma, 1.0f);
        float B = sigma * A;
        float s_over_t = mad(-ndotwi, ndotwo, wodotwi);
        s_over_t = s_over_t > 0 ? s_over_t / max(ndotwi, ndotwo) : s_over_t;
        float3 f = ONE_OVER_PI * mad(B, s_over_t, A) * rho;
        float3 f_comp = 0;

        if(AccountForMultiScattering && reflection)
        {
            float avgReflectance = mad(0.072488212f, B, A);
            float one_min_avgReflectance = 1 - avgReflectance;
            float tmp = ONE_OVER_PI * (avgReflectance / one_min_avgReflectance);
            float3 rho_ms_over_piSq = tmp / (1 - rho * one_min_avgReflectance);
            rho_ms_over_piSq *= rho * rho;

            float B_over_pi = ONE_OVER_PI * B; 
            float E_wo = mad(B_over_pi, g_wo, A);
            float E_wi = mad(B_over_pi, OrenNayar_G(ndotwi), A);
            f_comp = (1 - E_wo) * (1 - E_wi) * rho_ms_over_piSq;
        }

        return ndotwi * (f + f_comp);
    }

    // Includes multiplication by n.wi from the rendering equation
    float3 GGXMicrofacetBRDF(float alpha, float ndotwh, float ndotwo, float ndotwi,
        float3 fr, bool specular)
    {
        if(specular)
        {
            // For specular surfaces, total radiance reflected back towards w_o (L_o(w_o)) 
            // should be F * L_i(w_r), where w_r = reflect(-w_o, n). Plugging into the rendering 
            // equation:
            //      L_o(w_o) = /int f(w_o, w_i) * L_i(w_i) * ndotwi dw_i = F * L_i(w_r).
            // Now in order for the above to hold, we must have
            //      f(w_o, w_i) = F * delta(n - wh) / ndotwi
            // Note that ndotwi cancels out.
            return (ndotwh >= MIN_N_DOT_H_SPECULAR) * fr;
        }

        float alphaSq = alpha * alpha;
        float NDF = GGX(ndotwh, alphaSq);
        float G2DivDenom = SmithHeightCorrelatedG2_Opt<1>(alphaSq, ndotwi, ndotwo);
        float f = NDF * G2DivDenom * ndotwi;

        return f * fr;
    }

    // Given the bijective mapping w_i = f(w_h) where w_h denotes half vector,
    // the pdfs are related as follows:
    //      p(w_i) = p(w_h) / |J_f(w_h)|,
    // where J_f() is the Jacobian of f, from which dw_i = |J_f(w_h)| dw_h.
    // Putting everything together, p(w_i) = p(w_h) * dw_h / dw_i.
    float JacobianHalfVecToIncident_Tr(float eta, float whdotwo, float whdotwi)
    {
        float denom = mad(whdotwo, 1 / eta, whdotwi);
        denom *= denom;
        float dwh_dwi = whdotwi / denom;

        return dwh_dwi;
    }

    // Includes multiplication by n.wi from the rendering equation
    float GGXMicrofacetBTDF(float alpha, float ndotwh, float ndotwo, float ndotwi,
        float whdotwo, float whdotwi, float eta, float fr, bool specular)
    {
        if(specular)
        {
            // For specular surfaces, total radiance reflected back towards w_o (L_o(w_o)) 
            // should be (1 - F) * L_i(w_t), where w_t = refract(-w_o, n). Plugging into the 
            // rendering equation:
            //      L_o(w_o) = /int f(w_o, w_i) * L_i(w_i) * ndotwi dw_i = (1 - F) * L_i(w_t).
            // Now in order for the above to hold, we must have
            //      f(w_o, w_i) = (1 - F) * delta(n - wh) / ndotwi
            // Note that ndotwi cancels out.
            float f = ndotwh >= MIN_N_DOT_H_SPECULAR;
            // f *= 1 / (surface.eta * surface.eta);
            return f * (1 - fr);
        }

        float alphaSq = alpha * alpha;
        float NDF = GGX(ndotwh, alphaSq);
        float G2opt = SmithHeightCorrelatedG2_Opt<4>(alphaSq, ndotwi, ndotwo);

        float f = NDF * G2opt * whdotwo;
        float dwh_dwi = JacobianHalfVecToIncident_Tr(eta, whdotwo, whdotwi);
        f *= dwh_dwi;
        f *= ndotwi;
        // f *= 1 / (surface.eta * surface.eta);

        return f * (1 - fr);
    }

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

    // D(w_h) (distribution of visible normals) is defined as:
    //      D(w_h) = GGX(w_h) * max(w_h.w_o, 0) * G1(n.w_o) / n.w_o.
    // 
    // Note: The following returns D(w_h) / w_o.w_h as w_o.w_h is canceled out
    // after change of variable from w_h to w_i for reflection. For transmission, 
    // it has to be mutiplied back.
    float GGXMicrofacetPdf(float alpha, float ndotwh, float ndotwo)
    {
        float alphaSq = alpha * alpha;
        float NDF = GGX(ndotwh, alphaSq);
        float G1 = SmithG1(alphaSq, ndotwo);
        float wh_pdf = (NDF * G1) / ndotwo;

        return wh_pdf;
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
            ret.baseColor_Fr0_TrCol = 0;
            ret.eta = DEFAULT_ETA_MAT / ETA_AIR;
            ret.specTr = false;
            ret.trDepth = 0;
            ret.subsurface = 0;
            ret.g_wo = 0;
            ret.coat_weight = 0;
            ret.coat_color = 0;
            ret.coat_alpha = 0;
            ret.coat_eta = DEFAULT_ETA_COAT;

            return ret;
        }

        static ShadingData Init(float3 shadingNormal, float3 wo, bool metallic, float roughness, 
            float3 baseColor, float eta_curr = ETA_AIR, float eta_next = DEFAULT_ETA_MAT, 
            bool specTr = false, half transmissionDepth = 0, half subsurface = 0,
            float coat_weight = 0, float3 coat_color = 0.0f, float coat_roughness = 0,
            float eta_coat = DEFAULT_ETA_COAT)
        {
            // Coat roughening
            float2 roughness4 = float2(roughness, coat_roughness);
            if(coat_weight > 0 && coat_roughness > 0)
            {
                roughness4 *= roughness4;
                roughness4 *= roughness4;
                float roughness_coated = min(roughness4.x + 2 * roughness4.y, 1);
                // = roughness_coated^(1 / 4)
                roughness_coated = rsqrt(rsqrt(roughness_coated));
                roughness = Math::Lerp(roughness, roughness_coated, coat_weight);
            }

            ShadingData si;

            si.wo = wo;
            float ndotwo = dot(shadingNormal, wo);
            si.backfacing_wo = ndotwo <= 0;
            // Clamp to a small value to avoid division by zero
            si.ndotwo = max(ndotwo, 1e-5f);

            si.metallic = metallic;
            si.alpha = roughness * roughness;
            si.baseColor_Fr0_TrCol = baseColor;
            si.specTr = specTr;
            si.trDepth = transmissionDepth;
            si.subsurface = subsurface;
            // TODO surrounding medium is assumed to be air
            float eta_base = eta_curr == ETA_AIR ? eta_next : eta_curr;
            float eta_no_coat = eta_next / eta_curr;
            // To avoid spurious TIR
            float eta_coated = eta_base >= eta_coat ? eta_base / eta_coat : eta_coat / eta_base;
            // Adjust eta when surface is coated
            si.eta = Math::Lerp(eta_no_coat, eta_coated, coat_weight);

            // Precompute as it only depends on wo
            si.g_wo = !metallic && !specTr ? OrenNayar_G(max(ndotwo, 1e-4f)) : 0;

            si.coat_weight = coat_weight;
            si.coat_color = coat_color;
            si.coat_alpha = coat_roughness * coat_roughness;
            // TODO surrounding medium is assumed to be air
            si.coat_eta = eta_curr == ETA_AIR ? eta_coat / ETA_AIR : ETA_AIR / eta_coat;

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

        float3 SetWi(float3 wi, float3 shadingNormal)
        {
            // Transmission happens when wi and wo are on opposite sides of the surface
            float ndotwi_n = dot(shadingNormal, wi);
            this.reflection = ndotwi_n >= 0;

            // For reflection:
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

            return wh;
        }

        float3 Fresnel(float3 fr0, out bool tir)
        {
            float cosTheta_i = this.whdotwo;
            tir = false;

            // Use Schlick's approximation for metals
            if(this.metallic)
                return FresnelSchlick(fr0, cosTheta_i);

            float eta_relative = 1.0f / this.eta;
            float sinTheta_iSq = saturate(mad(-cosTheta_i, cosTheta_i, 1.0f));
            float cosTheta_tSq = mad(-eta_relative * eta_relative, sinTheta_iSq, 1.0f);

            // Check for TIR
            tir = cosTheta_tSq <= 0;
            if(tir)
                return 1;

            float cosTheta_t = sqrt(cosTheta_tSq);

            return Fresnel_Dielectric(cosTheta_i, eta_relative, cosTheta_t);
        }

        float3 Fresnel(out bool tir)
        {
            float3 fr0 = this.metallic ? this.baseColor_Fr0_TrCol : 
                DielectricF0(this.eta);

            return Fresnel(fr0, tir);
        }

        float3 Fresnel()
        {
            float3 fr0 = this.metallic ? this.baseColor_Fr0_TrCol : 
                DielectricF0(this.eta);
            bool unused;

            return Fresnel(fr0, unused);
        }

        float Fresnel_Coat(out float cosTheta_t)
        {
            cosTheta_t = 0;
            float cosTheta_i = this.whdotwo;
            float eta_relative = 1.0f / this.coat_eta;
            float sinTheta_iSq = saturate(mad(-cosTheta_i, cosTheta_i, 1.0f));
            float cosTheta_tSq = mad(-eta_relative * eta_relative, sinTheta_iSq, 1.0f);

            // Check for TIR
            if(cosTheta_tSq <= 0)
                return 1;

            cosTheta_t = sqrt(cosTheta_tSq);
            float Fr0 = DielectricF0(this.coat_eta);
            float cosTheta = this.coat_eta > 1 ? cosTheta_i : cosTheta_t;

            return FresnelSchlick_Dielectric(Fr0, cosTheta);
        }

        void Regularize()
        {
            // i.e. to linear roughness in [~0.3, 0.5]
            this.alpha = this.alpha < 0.25f ? clamp(2.0f * this.alpha, 0.1f, 0.25f) : this.alpha;
        }

        float3 TransmissionTint()
        {
            return trDepth > 0 ? 1 : baseColor_Fr0_TrCol;
        }

        bool ThinWalled()
        {
            return subsurface > 0;
        }

        bool Transmissive()
        {
            return specTr || ThinWalled();
        }

        bool Coated()
        {
            return coat_weight != 0;
        }

        bool GlossSpecular()
        {
            return this.alpha <= MAX_ALPHA_SPECULAR;
        }

        bool CoatSpecular()
        {
            return this.coat_alpha <= MAX_ALPHA_SPECULAR;
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
        //  - Base color for dielectrics
        //  - Fresnel at normal incidence for metals
        //  - Transmission color for dielectrics with specular transmission
        float3 baseColor_Fr0_TrCol;
        float eta;      // eta_i / eta_t
        bool specTr;
        bool metallic;
        bool backfacing_wo;
        bool invalid;
        bool reflection;
        half trDepth;
        half subsurface;
        float coat_weight;
        float3 coat_color;
        float coat_alpha;
        float coat_eta;
    };

    bool IsLobeValid(ShadingData surface, LOBE lt)
    {
        if(lt == LOBE::ALL)
            return true;

        if(surface.metallic && (lt != LOBE::GLOSSY_R) && (lt != LOBE::COAT))
            return false;

        if(!surface.specTr && (lt == LOBE::GLOSSY_T))
            return false;

        if(surface.specTr && (lt == LOBE::DIFFUSE_R))
            return false;

        if(!surface.ThinWalled() && (lt == LOBE::DIFFUSE_T))
            return false;

        if(!surface.Coated() && (lt == LOBE::COAT))
            return false;

        return true;
    }

    float LobeAlpha(ShadingData surface, LOBE lt)
    {
        if(lt == LOBE::GLOSSY_R || lt == LOBE::GLOSSY_T)
            return surface.alpha;
        if(lt == LOBE::COAT)
            return surface.coat_alpha;

        return 1.0f;
    }

    //--------------------------------------------------------------------------------------
    // Slabs
    //--------------------------------------------------------------------------------------

    // Use the same routine for diffuse reflection and diffuse transmission to avoid
    // a branching code path.
    // Includes multiplication by n.wi from the rendering equation
    template<bool EON>
    float3 EvalDiffuse(ShadingData surface)
    {
        float s = surface.subsurface == 0 ? 1 : (float)surface.subsurface * 0.5f;

#if USE_OREN_NAYAR == 1
        // Diffuse roughness should be a separate parameter, but for now
        // use specular roughness
        float diffuseRoughness = sqrt(surface.alpha);
        float3 diffuse = OrenNayar<EON>(surface.baseColor_Fr0_TrCol,
            diffuseRoughness, surface.ndotwo, surface.ndotwi, surface.wodotwi,
            surface.g_wo, surface.reflection);
#else
        float3 diffuse = Lambertian(surface.baseColor_Fr0_TrCol, surface.ndotwi);
#endif

        return s * diffuse;
    }

    float3 SampleDiffuse(float3 normal, float2 u, out float pdf)
    {
        float3 wiLocal = Sampling::SampleCosineWeightedHemisphere(u, pdf);

        float3 T;
        float3 B;
        Math::revisedONB(normal, T, B);
        float3 wiWorld = mad(wiLocal.x, T, mad(wiLocal.y, B, wiLocal.z * normal));

        return wiWorld;
    }

    float3 SampleDiffuse(float3 normal, float2 u)
    {
        float unused;
        return SampleDiffuse(normal, u, unused);
    }

    // = Pdf of cosine-weighted hemisphere sampling
    float DiffusePdf(ShadingData surface)
    {
        return surface.ndotwi * ONE_OVER_PI;
    }

    // Includes multiplication by n.wi from the rendering equation
    float3 EvalGloss(ShadingData surface, float3 fr)
    {
        return GGXMicrofacetBRDF(surface.alpha, surface.ndotwh, surface.ndotwo, 
            surface.ndotwi, fr, surface.GlossSpecular());
    }

    float3 SampleGloss(ShadingData surface, float3 shadingNormal, float2 u)
    {
        // Reminder: reflect(w, n) = reflect(w, -n)

        // Fast path for specular surfaces
        if(surface.GlossSpecular())
            return reflect(-surface.wo, shadingNormal);

        // As a convention, microfacet normals point into the upper hemisphere (in the coordinate
        // system aligned with normal). Since here it's assumed n.wo > 0, this is always true.
        float3 wh = SampleGGXMicrofacet(surface.wo, surface.alpha, shadingNormal, u);

        // Reflect wo about the plane with normal wh (each microsurface is a perfect mirror)
        float3 wi = reflect(-surface.wo, wh);

        return wi;
    }

    // Evaluates distribution of visible normals for reflection
    //      D(w_h) = GGX(w_h) * max(w_h.w_o, 0) * G1(n.w_o) / n.w_o.
    //
    // After correction for change of variable from w_h to w_i, it becomes
    //      p(wi) = D(w_h) * 1 / (4 * w_h.w_i)
    //            = GGX(w_h) * G1(n.w_o) / (4 * n.w_o)
    // (Note that w_h.w_o = w_h.w_i).
    float GlossPdf(ShadingData surface)
    {
        if(surface.GlossSpecular())
            return (surface.ndotwh >= MIN_N_DOT_H_SPECULAR);

        float pdf = GGXMicrofacetPdf(surface.alpha, surface.ndotwh, surface.ndotwo);
        return pdf / 4.0f;
    }

    float EvalTranslucentTr(ShadingData surface, float fr)
    {
        return GGXMicrofacetBTDF(surface.alpha, surface.ndotwh, surface.ndotwo, 
            surface.ndotwi, surface.whdotwo, surface.whdotwi, surface.eta, fr, 
            surface.GlossSpecular());
    }

    float3 SampleTranslucentTr(ShadingData surface, float3 shadingNormal, float2 u)
    {
        // Note that: 
        //  - refract(w, n, eta) requires w.n > 0 and ||n|| = 1. Here it's assumed 
        //    n.wo > 0, so this is always true.
        //  - For sampling, transmitted direction is known (wo) and the goal is to sample 
        //    incident direction (wi). Therefore, refract() should be called with inputs
        //    for the reverse direction -- eta = 1 / eta.

        // Fast path for specular surfaces
        if(surface.GlossSpecular())
            return refract(-surface.wo, shadingNormal, 1 / surface.eta);

        float3 wh = SampleGGXMicrofacet(surface.wo, surface.alpha, shadingNormal, u);

        // Refract wo about the plane with normal wh using Snell's law.
        float3 wi = refract(-surface.wo, wh, 1 / surface.eta);

        return wi;
    }

    // Evaluates distribution of visible normals for transmission
    //      D(w_h) = GGX(w_h) * max(w_h.wo, 0) * G1(n.w_o) / n.w_o.
    //
    // After correction for change of variable from wh to wi, it becomes
    //      p(wi) = D(w_h) * w_h.w_i / (w_h.w_i + w_h.w_o / eta)^2
    //            = GGX(wh) * w_h.w_o * w_h.w_i * G1(n.w_o) / (n.w_o * w_h.w_i + w_h.w_o / eta)^2)
    float TranslucentTrPdf(ShadingData surface)
    {
        if(surface.GlossSpecular())
            return (surface.ndotwh >= MIN_N_DOT_H_SPECULAR);

        float pdf = GGXMicrofacetPdf(surface.alpha, surface.ndotwh, surface.ndotwo);
        pdf *= surface.whdotwo;

        float dwh_dwi = JacobianHalfVecToIncident_Tr(surface.eta, surface.whdotwo,
            surface.whdotwi);
        pdf *= dwh_dwi;

        return pdf;
    }

    float EvalCoat(ShadingData surface, float Fr)
    {
        return surface.coat_weight * GGXMicrofacetBRDF(surface.coat_alpha, surface.ndotwh, 
            surface.ndotwo, surface.ndotwi, Fr, surface.CoatSpecular()).x;
    }

    float3 SampleCoat(ShadingData surface, float3 shadingNormal, float2 u)
    {
        float3 wh = surface.CoatSpecular() ? shadingNormal :
            SampleGGXMicrofacet(surface.wo, surface.coat_alpha, shadingNormal, u);
        float3 wi = reflect(-surface.wo, wh);

        return wi;
    }

    float CoatPdf(ShadingData surface)
    {
        if(surface.CoatSpecular())
            return (surface.ndotwh >= MIN_N_DOT_H_SPECULAR);

        float pdf = GGXMicrofacetPdf(surface.coat_alpha, surface.ndotwh, surface.ndotwo);
        return pdf / 4.0f;
    }

    // Includes multiplication by n.wi from the rendering equation
    float3 GlossOverPdf(ShadingData surface, float3 fr)
    {
        if(surface.GlossSpecular())
        {
            // Note that ndotwi cancels out.
            return fr;
        }

        // When VNDF is used for sampling the incident direction (wi), the expression 
        //        f * cos(theta) / Pdf(wi)
        // is simplified to Fr * G2 / G1.
        float alphaSq = surface.alpha * surface.alpha;
        return SmithHeightCorrelatedG2OverG1(alphaSq, surface.ndotwi, surface.ndotwo) * 
            fr;
    }

    // Includes multiplication by n.wi from the rendering equation
    float3 TranslucentTrOverPdf(ShadingData surface, float fr)
    {
        if(surface.GlossSpecular())
        {
            // Note that ndotwi cancels out.
            return (1 - fr) * surface.TransmissionTint();
        }

        // When VNDF is used for sampling the incident direction (wi), the expression 
        //        f * cos(theta) / Pdf(wi)
        // is simplified to (1 - Fr) * G2 / G1.
        float alphaSq = surface.alpha * surface.alpha;
        return SmithHeightCorrelatedG2OverG1(alphaSq, surface.ndotwi, surface.ndotwo) * 
            (1 - fr) * surface.TransmissionTint();
    }

    float3 BaseWeight(ShadingData surface)
    {
        float3 base_weight = 1;
        if(surface.Coated())
        {
            float cosTheta_t;
            float Fr_coat = surface.Fresnel_Coat(cosTheta_t);
            bool tir_c = cosTheta_t <= 0;

            if(tir_c)
                return 0;

            float reflectance_c = surface.CoatSpecular() ? Fr_coat : 
                GGXReflectance_Dielectric(surface.coat_alpha, surface.ndotwo, surface.coat_eta);

            // View-dependent absorption
            float c = 0.5f / cosTheta_t + 0.5f / surface.whdotwo;
            // = coat_color^c
            float3 coat_tr = exp(c * log(surface.coat_color));

            // f = mix(base, layer(base, coat), coat_weight)
            base_weight = Math::Lerp(1.0f.xxx, (1 - reflectance_c) * coat_tr, surface.coat_weight);
        }

        return base_weight;
    }

    float3 TransmittanceToDielectricBaseTr(ShadingData surface)
    {
        float3 base_weight = BaseWeight(surface);
        // For specular, 1 - reflectance becomes 1 - Fr, which is accounted for separately
        float reflectance_g = surface.GlossSpecular() ? 0 : 
            GGXReflectance_Dielectric(surface.alpha, surface.ndotwo, surface.eta);

        return (1 - reflectance_g) * base_weight;
    }

    float3 DielectricBaseSpecularTr(ShadingData surface, float Fr_g)
    {
        if(surface.invalid || !surface.specTr)
            return 0;

        float3 transmittance = TransmittanceToDielectricBaseTr(surface);
        float glossyTr = EvalTranslucentTr(surface, Fr_g);

        return glossyTr * surface.TransmissionTint() * transmittance;
    }

    float3 DielectricBaseDiffuseTr(ShadingData surface, float Fr_g)
    {
        if(surface.invalid)
            return 0;

        float3 base_weight = BaseWeight(surface);
        float reflectance_g = surface.GlossSpecular() ? Fr_g : 
            GGXReflectance_Dielectric(surface.alpha, surface.ndotwo, surface.eta);

        return (1 - reflectance_g) * BSDF::EvalDiffuse<false>(surface) * base_weight;
    }

    //--------------------------------------------------------------------------------------
    // Surface Shader
    //--------------------------------------------------------------------------------------

    struct BSDFEval
    {
        static BSDFEval Init()
        {
            BSDFEval ret;
            ret.f = 0;
            ret.Fr_g = 0;
            ret.tir = false;

            return ret;
        }

        float3 f;
        float3 Fr_g;
        bool tir;
    };

    // Includes multiplication by n.wi from the rendering equation
    BSDFEval Unified(ShadingData surface)
    {
        BSDFEval ret = BSDFEval::Init();
        if(surface.invalid)
            return ret;

        // Coat
        float3 base_weight = 1;
        if(surface.Coated())
        {
            float cosThetaT_o;
            float Fr_coat = surface.Fresnel_Coat(cosThetaT_o);
            bool tir_c = cosThetaT_o <= 0;

            if(!surface.reflection && tir_c)
                return ret;

            if(surface.reflection)
            {
                ret.f = EvalCoat(surface, Fr_coat);
                if(tir_c)
                    return ret;
            }

            float reflectance_c = surface.CoatSpecular() ? Fr_coat : 
                GGXReflectance_Dielectric(surface.coat_alpha, surface.ndotwo, surface.coat_eta);

            // View-dependent absorption
            float c = 1.0f / cosThetaT_o;
            // Note: Incorrect for transmission as wh.wo != wh.wi. Additionally needs 
            // the transmitted angle of incident ray using the relative IOR of coat 
            // layer. 
#if 0
            if(!surface.reflection)
            {
                float sinThetaISq = saturate(mad(-this.whdotwi, this.whdotwi, 1.0f));
                float cosThetaTSq = saturate(mad(-this.coat_eta * this.coat_eta, sinThetaISq, 1.0f));
                float cosThetaT_i = sqrt(cosThetaTSq);
                c = 0.5f * c + (0.5f / cosThetaT_i);
            }
#endif
            // = coat_color^c
            float3 coat_tr = exp(c * log(surface.coat_color));

            // f = mix(base, layer(base, coat), coat_weight)
            base_weight = Math::Lerp(1.0f.xxx, (1 - reflectance_c) * coat_tr, surface.coat_weight);
        }

        float3 fr0 = surface.metallic ? surface.baseColor_Fr0_TrCol : 
            DielectricF0(surface.eta);
        ret.Fr_g = surface.Fresnel(fr0, ret.tir);

        // = Metal = reflection from Translucent Base
        float3 glossyRefl = EvalGloss(surface, ret.Fr_g);

        // Metal or TIR
        if(surface.metallic || ret.tir)
        {
            ret.f += base_weight * glossyRefl;
            return ret;
        }

        float reflectance_g = surface.GlossSpecular() ? ret.Fr_g.x : 
            GGXReflectance_Dielectric(surface.alpha, surface.ndotwo, surface.eta);

        // Opaque Base, possibly in thin walled mode
        if(!surface.specTr)
        {
            float3 diffuse = EvalDiffuse<ENERGY_PRESERVING_OREN_NAYAR>(surface);

            // For diffuse transmission, all other lobes are excluded
            ret.f += base_weight * ((1 - reflectance_g) * diffuse + 
                glossyRefl * surface.reflection);

            return ret;
        }

        // Translucent Base
        if(surface.reflection)
        {
            ret.f += glossyRefl * base_weight;
            return ret;
        }

        // For specular, (1 - Fresnel) factor is already accounted for
        reflectance_g = surface.GlossSpecular() ? 0 : reflectance_g;
        float glossyTr = EvalTranslucentTr(surface, ret.Fr_g.x);
        ret.f = ((1 - reflectance_g) * glossyTr * surface.TransmissionTint()) * base_weight;

        return ret;
    }
}

#endif