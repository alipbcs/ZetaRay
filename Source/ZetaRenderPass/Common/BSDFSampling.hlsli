#ifndef BSDF_SAMPLING_H
#define BSDF_SAMPLING_H

#include "BSDF.hlsli"
#include "RT.hlsli"

namespace BSDF
{
    //--------------------------------------------------------------------------------------
    // BSDF sampling
    //--------------------------------------------------------------------------------------

    struct BSDFSample
    {
        static BSDFSample Init()
        {
            BSDFSample ret;
            ret.bsdfOverPdf = 0;
            ret.f = 0;
            ret.pdf = 0;

            return ret;
        }

        float3 wi;
        BSDF::LOBE lobe;
        // Joint pdf of sampling the lobe and direction 
        float pdf;
        float3 bsdfOverPdf;
        float3 f;
    };

    struct BSDFSamplerEval
    {
        // Joint pdf of sampling the lobe and direction 
        float pdf;
        float3 bsdfOverPdf;
    };

    struct NoOp 
    {
        float3 operator()(float3 w)
        {
            return 1.0f;
        }
    };

    template<typename Func>
    BSDFSample SampleBSDF_NoDiffuse(float3 normal, ShadingData surface, float2 u_wh, float u_lobe, 
        Func func)
    {
        float3 wh = surface.specular ? normal : BSDF::SampleMicrofacet(surface.wo, surface.alpha, normal, u_wh);
        float wh_pdf = BSDF::MicrofacetPdf(normal, wh, surface);

        float3 wi_r = reflect(-surface.wo, wh);
        surface.SetWi_Refl(wi_r, normal, wh);

        BSDFSample ret;
        ret.wi = wi_r;
        ret.lobe = BSDF::LOBE::GLOSSY_R;
        ret.pdf = 1;

        // Account for change of density from half vector to incident vector
        if(!surface.specular)
            ret.pdf = wh_pdf / 4.0f;

        ret.f = BSDF::UnifiedBSDF(surface) * func(wi_r);
        ret.bsdfOverPdf = ret.f / ret.pdf;

        if(surface.metallic || !surface.transmissive)
            return ret;

        // Transmissive dielectric surface

        float3 wi_t = refract(-surface.wo, wh, 1 / surface.eta);
        float fr0 = BSDF::DielectricF0(surface.eta);
        float whdotwx = surface.eta < 1 ? abs(dot(wh, wi_t)) : saturate(dot(wh, surface.wo));
        // For total internal reflection, all the incident light is reflected
        float fr = dot(wi_t, wi_t) == 0 ? 1 : BSDF::FresnelSchlick_Dielectric(fr0, whdotwx);

        // Use Fresnel to decide between reflection and transmission
        if (u_lobe < fr)
        {
            ret.bsdfOverPdf /= fr;
            ret.pdf *= fr;
        }
        else
        {
            ret.pdf = 1 - fr;
            surface.SetWi_Tr(wi_t, normal, wh);

            if(!surface.specular)
            {
                ret.pdf *= wh_pdf * surface.whdotwo;

                // Account for change of density from half vector to incident vector
                float dwh_dwi = BSDF::JacobianHalfVecToIncident_Tr(surface);
                ret.pdf *= dwh_dwi;
            }

            ret.f = BSDF::DielectricBTDF(surface) * func(wi_t);
            ret.bsdfOverPdf = ret.f / ret.pdf;
            ret.wi = wi_t;
            ret.lobe = BSDF::LOBE::GLOSSY_T;
        }

        return ret;
    }

    // Either surface is transmissive or diffuse lobe is intentionally ignored
    BSDFSample SampleBSDF_NoDiffuse(float3 normal, ShadingData surface, inout RNG rng)
    {
        float2 u_wh = rng.Uniform2D();
        float u_lobe = rng.Uniform();
        NoOp noop;

        return SampleBSDF_NoDiffuse<NoOp>(normal, surface, u_wh, u_lobe, noop);
    }

    template<typename Func>
    BSDFSample SampleBRDF(float3 normal, ShadingData surface, float2 u_r, float2 u_d, float u_wrs_d, 
        Func func)
    {
        float3 wi_r = BSDF::SampleMicrofacetBRDF(surface, normal, u_r);
        surface.SetWi_Refl(wi_r, normal);

        // Streaming RIS using weighted reservoir sampling to sample aggregate BRDF
        float w_sum;
        float3 target = BSDF::BaseBRDF(surface);
        target *= func(wi_r);

        BSDFSample ret;
        ret.wi = wi_r;
        ret.lobe = BSDF::LOBE::GLOSSY_R;
        ret.f = target;

        // Metals don't have transmission or diffuse lobes.
        if(surface.metallic)
        {
            ret.pdf = BSDF::VNDFReflectionPdf(surface);
            ret.bsdfOverPdf = target / ret.pdf;

            return ret;
        }

        // Specular/glossy reflection
        {
            float targetLum_r = Math::Luminance(target);

            float pdf_r = BSDF::VNDFReflectionPdf(surface);
            float pdf_d = (dot(wi_r, normal) >= 0) * BSDF::LambertianBRDFPdf(surface);
            w_sum = RT::BalanceHeuristic(pdf_r, pdf_d, targetLum_r);
        }

        // Lambertian diffuse
        {
            float pdf_d;
            float3 wi_d = BSDF::SampleLambertianBRDF(normal, u_d, pdf_d);
            surface.SetWi_Refl(wi_d, normal);

            float3 target_d = BSDF::OpaqueBase(surface);
            target_d *= func(wi_d);
            float targetLum_d = Math::Luminance(target_d);
            float pdf_r = BSDF::VNDFReflectionPdf(surface);
            float w = RT::BalanceHeuristic(pdf_d, pdf_r, targetLum_d);
            w_sum += w;

            if ((w_sum > 0) && (u_wrs_d < (w / w_sum)))
            {
                target = target_d;

                ret.wi = wi_d;
                ret.lobe = BSDF::LOBE::DIFFUSE_R;
                ret.f = target_d;
            }
        }

        float targetLum = Math::Luminance(target);
        ret.bsdfOverPdf = targetLum > 0 ? target * w_sum / targetLum : 0;
        ret.pdf = w_sum > 0 ? targetLum / w_sum : 0;

        return ret;
    }

    template<typename Func>
    BSDFSample SampleBSDF(float3 normal, ShadingData surface, float2 u_wh, float2 u_d,
        float u_wrs_r, float u_wrs_d, Func func)
    {
        if(!surface.transmissive)
            return SampleBRDF(normal, surface, u_wh, u_d, u_wrs_d, func);

        return BSDF::SampleBSDF_NoDiffuse(normal, surface, u_wh, u_wrs_r, func);
    }

    BSDFSample SampleBSDF(float3 normal, ShadingData surface, inout RNG rng)
    {
        // Make sure the number of random numbers used in all code paths is the same. Since
        // .UniformX() calls change the internal state of rng, they're not compiled out.
        float2 u_wh = rng.Uniform2D();
        float2 u_d = rng.Uniform2D();
        float u_wrs_r = rng.Uniform();
        float u_wrs_d = rng.Uniform();
        NoOp noop;

        if(!surface.transmissive)
            return SampleBRDF(normal, surface, u_wh, u_d, u_wrs_d, noop);

        return BSDF::SampleBSDF_NoDiffuse(normal, surface, u_wh, u_wrs_r, noop);
    }

    template<typename Func>
    BSDFSamplerEval EvalBRDFSampler(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, float2 u_r, float2 u_d, Func func)
    {
        BSDFSamplerEval ret;
        float3 targetScale = func(wi);

        if(surface.metallic)
        {
            surface.SetWi_Refl(wi, normal);
            ret.pdf = BSDF::VNDFReflectionPdf(surface);
            ret.bsdfOverPdf = !surface.invalid * BSDF::MicrofacetBRDFOverPdf(surface);
            ret.bsdfOverPdf *= targetScale;

            return ret;
        }

        float w_sum;
        float3 target;

        // Specular/glossy reflection
        {
            bool isZ_r = lobe == BSDF::LOBE::GLOSSY_R;
            float3 wi_r = isZ_r ? wi : BSDF::SampleMicrofacetBRDF(surface, normal, u_r);
            surface.SetWi_Refl(wi_r, normal);

            target = BSDF::OpaqueBase(surface) * targetScale;
            float targetLum_r = Math::Luminance(target);

            float pdf_r = BSDF::VNDFReflectionPdf(surface);
            float pdf_d = (dot(normal, wi_r) >= 0) * BSDF::LambertianBRDFPdf(surface);
            w_sum = RT::BalanceHeuristic(pdf_r, pdf_d, targetLum_r);
        }

        // Lambertian
        {
            bool isZ_d = lobe == BSDF::LOBE::DIFFUSE_R;
            float3 wi_d = isZ_d ? wi : BSDF::SampleLambertianBRDF(normal, u_d);
            surface.SetWi_Refl(wi_d, normal);

            float3 target_d = BSDF::OpaqueBase(surface) * func(wi_d);
            float targetLum_d = Math::Luminance(target_d);

            float pdf_r = BSDF::VNDFReflectionPdf(surface);
            float pdf_d = BSDF::LambertianBRDFPdf(surface);
            w_sum += RT::BalanceHeuristic(pdf_d, pdf_r, targetLum_d);
            target = isZ_d ? target_d : target;
        }

        float targetLum = Math::Luminance(target);
        ret.bsdfOverPdf = targetLum > 0 ? target * w_sum / targetLum : 0;
        ret.pdf = w_sum > 0 ? targetLum / w_sum : 0;

        return ret;
    }

    template<typename Func>
    BSDFSamplerEval EvalBSDFSampler_NoDiffuse(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, float u_lobe, Func func)
    {
        surface.SetWi(wi, normal);
        float wh_pdf = BSDF::MicrofacetPdf(surface);
        float3 targetScale = func(wi);

        BSDFSamplerEval ret;
        ret.bsdfOverPdf = !surface.invalid * surface.reflection * BSDF::MicrofacetBRDFOverPdf(surface);
        ret.bsdfOverPdf *= targetScale;
        ret.pdf = !surface.specular ? wh_pdf / 4.0f : surface.ndotwh >= MIN_N_DOT_H_SPECULAR;

        if(surface.metallic || !surface.transmissive)
            return ret;

        float fr = surface.Fresnel_Dielectric();

        if(lobe == BSDF::LOBE::GLOSSY_R)
        {
            ret.bsdfOverPdf /= fr;
            ret.pdf *= fr;
            return ret;
        }

        ret.bsdfOverPdf = !surface.invalid * !surface.reflection * BSDF::MicrofacetBTDFOverPdf(surface) / (1 - fr);
        ret.bsdfOverPdf *= targetScale;
        ret.pdf = 1 - fr;
        ret.pdf *= surface.specular ? surface.ndotwh >= MIN_N_DOT_H_SPECULAR : wh_pdf * surface.whdotwo;

        if(!surface.specular)
        {
            float dwh_dwi = BSDF::JacobianHalfVecToIncident_Tr(surface);
            ret.pdf *= dwh_dwi;
        }

        return ret;
    }

    template<typename Func>
    BSDFSamplerEval EvalBSDFSampler_NoDiffuse(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, Func func, inout RNG rng)
    {
        float2 u_wh = rng.Uniform2D();
        float u_lobe = rng.Uniform();

        return EvalBSDFSampler_NoDiffuse(normal, surface, wi, lobe, u_lobe, func);
    }

    BSDFSamplerEval EvalBSDFSampler_NoDiffuse(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, inout RNG rng)
    {
        float2 u_wh = rng.Uniform2D();
        float u_lobe = rng.Uniform();
        NoOp noop;

        return EvalBSDFSampler_NoDiffuse(normal, surface, wi, lobe, u_lobe, noop);
    }

    template<typename Func>
    BSDFSamplerEval EvalBSDFSampler(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, float2 u_wh, float2 u_d, float u_wrs_r, Func func)
    {
        if(!surface.transmissive)
            return EvalBRDFSampler(normal, surface, wi, lobe, u_wh, u_d, func);

        return EvalBSDFSampler_NoDiffuse(normal, surface, wi, lobe, u_wrs_r, func);
    }

    // Mirrors the computation in SampleBSDF() to return joint probability of sampling lobe l_z 
    // and direction wi given the specific random numbers u_0, ..., u_n.
    template<typename Func>
    BSDFSamplerEval EvalBSDFSampler(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, Func func, inout RNG rng)
    {
        float2 u_wh = rng.Uniform2D();
        float2 u_d = rng.Uniform2D();
        float u_wrs_r = rng.Uniform();
        float u_wrs_d = rng.Uniform();

        if(!surface.transmissive)
            return EvalBRDFSampler(normal, surface, wi, lobe, u_wh, u_d, func);

        return EvalBSDFSampler_NoDiffuse(normal, surface, wi, lobe, u_wrs_r, func);
    }    
    
    BSDFSamplerEval EvalBSDFSampler(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, inout RNG rng)
    {
        float2 u_wh = rng.Uniform2D();
        float2 u_d = rng.Uniform2D();
        float u_wrs_r = rng.Uniform();
        float u_wrs_d = rng.Uniform();
        NoOp noop;

        if(!surface.transmissive)
            return EvalBRDFSampler(normal, surface, wi, lobe, u_wh, u_d, noop);

        return EvalBSDFSampler_NoDiffuse(normal, surface, wi, lobe, u_wrs_r, noop);
    }

    float BSDFSamplerPdf_NoDiffuse(float3 normal, BSDF::ShadingData surface, float3 wi)
    {
        surface.SetWi(wi, normal);
        float wh_pdf = BSDF::MicrofacetPdf(surface);

        if(surface.metallic || !surface.transmissive)
        {
            float pdf = surface.specular ? 
                surface.ndotwh >= MIN_N_DOT_H_SPECULAR : 
                wh_pdf / 4.0f;
            return pdf * surface.reflection;
        }

        float fr = surface.Fresnel_Dielectric();
        float pdf = surface.specular ? surface.ndotwh >= MIN_N_DOT_H_SPECULAR : 1;

        if(surface.reflection)
            return fr * pdf * (surface.specular ? 1 : (wh_pdf / 4.0f));

        pdf *= 1 - fr;

        if(!surface.specular)
        {
            pdf *= wh_pdf * surface.whdotwo;
            float dwh_dwi = BSDF::JacobianHalfVecToIncident_Tr(surface);
            pdf *= dwh_dwi;
        }

        return pdf;
    }

    float BSDFSamplerPdf(float3 normal, BSDF::ShadingData surface, float3 wi_z, inout RNG rng)
    {
        if(surface.transmissive)
            return BSDFSamplerPdf_NoDiffuse(normal, surface, wi_z);

        surface.SetWi(wi_z, normal);
        if(!surface.reflection)
            return 0;

        // Microfacet reflection is the only lobe
        if(surface.metallic)
            return BSDF::VNDFReflectionPdf(surface);

        // Surface is opaque dielectric

        // Using the law of total probability:
        //      p(wi_z) = p(wi_z, lobe = lobe_0) + ... + p(wi_z, lobe = lobe_k)
        float w_sum_r;
        float w_sum_d;
        float targetLum;

        // wi_z
        {
            float3 target = BSDF::OpaqueBase(surface);
            targetLum = Math::Luminance(target);

            float wh_z_pdf = BSDF::MicrofacetPdf(surface);
            float pdf_r = surface.specular ? surface.ndotwh >= MIN_N_DOT_H_SPECULAR : 
                wh_z_pdf / 4.0f;
            float pdf_d = !surface.invalid * BSDF::LambertianBRDFPdf(surface);
            float w = RT::BalanceHeuristic(pdf_r, pdf_d, targetLum);
            w_sum_r = w;
            w_sum_d = w;
        }

        // wi_d
        {
            float pdf_d;
            float3 wi_d = BSDF::SampleLambertianBRDF(normal, rng.Uniform2D(), pdf_d);
            surface.SetWi_Refl(wi_d, normal);
            float3 target_d = BSDF::OpaqueBase(surface);
            float targetLum_d = Math::Luminance(target_d);

            float pdf_r = BSDF::VNDFReflectionPdf(surface);
            float w = RT::BalanceHeuristic(pdf_d, pdf_r, targetLum_d);
            w_sum_r += w;
        }

        float3 wh = surface.specular ? normal : 
            BSDF::SampleMicrofacet(surface.wo, surface.alpha, normal, rng.Uniform2D());
        float wh_pdf = BSDF::MicrofacetPdf(normal, wh, surface);

        // wi_r
        {
            float3 wi_r = reflect(-surface.wo, wh);
            surface.SetWi_Refl(wi_r, normal, wh);
            float3 target_r = BSDF::OpaqueBase(surface);
            float targetLum_r = Math::Luminance(target_r);

            // Account for change of density from half vector to incident vector
            float pdf_r = surface.specular ? 1 : wh_pdf / 4.0f;
            float pdf_d = BSDF::LambertianBRDFPdf(surface);
            float w = RT::BalanceHeuristic(pdf_r, pdf_d, targetLum_r);
            w_sum_d += w;
        }

        // p(wi_z, lobe = microfacet)
        float pdf = w_sum_r > 0 ? targetLum / w_sum_r : 0;
        // p(wi_z, lobe = lambertian)
        pdf += w_sum_d > 0 ? targetLum / w_sum_d : 0;

        return pdf;
    }
}

#endif