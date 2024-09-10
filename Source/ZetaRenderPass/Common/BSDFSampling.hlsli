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
    BSDFSample SampleBSDF_NoDiffuse(float3 normal, ShadingData surface, float2 u_wh, 
        float u_lobe, Func func)
    {
        float3 wh = surface.specular ? normal : BSDF::SampleGGXMicrofacet(surface.wo, 
            surface.alpha, normal, u_wh);

        float3 wi_r = reflect(-surface.wo, wh);
        surface.SetWi_Refl(wi_r, normal, wh);
        float wh_pdf = BSDF::GGXMicrofacetPdf(surface.alpha, surface.ndotwh, surface.ndotwo);

        BSDFSample ret;
        ret.wi = wi_r;
        ret.lobe = BSDF::LOBE::GLOSSY_R;
        // Account for change of density from half vector to incident vector
        ret.pdf = surface.specular ? 1 : wh_pdf / 4.0f;

        BSDFEval eval = BSDF::Unified(surface);
        ret.f = eval.f * func(wi_r);
        ret.bsdfOverPdf = ret.f / ret.pdf;

        if(surface.metallic || !surface.specTr || eval.tir)
            return ret;

        // Transmissive dielectric surface

        // Use Fresnel to decide between reflection and transmission
        if (u_lobe < eval.Fr.x)
        {
            ret.bsdfOverPdf /= eval.Fr.x;
            ret.pdf *= eval.Fr.x;
        }
        else
        {
            float3 wi_t = refract(-surface.wo, wh, 1 / surface.eta);
            surface.SetWi_Tr(wi_t, normal, wh);
            // 1 - fr is multiplied below
            ret.pdf = 1;

            if(!surface.specular)
            {
                ret.pdf *= wh_pdf * surface.whdotwo;

                // Account for change of density from half vector to incident vector
                float dwh_dwi = BSDF::JacobianHalfVecToIncident_Tr(surface.eta, 
                    surface.whdotwo, surface.whdotwi);
                ret.pdf *= dwh_dwi;
            }

            // Corner case: Fresnel returned no TIR, so sampling transmission is possible,
            // yet refract() detected TIR and returned 0 (possibly due to floating-point 
            // errors). In that case, surface.invalid becomes true, causing bsdf to evaluate 
            // to 0 and the path-tracing loop is terminated.

            // BSDF and pdf both contain 1 - fr, so passing 0 causes it to cancel out
            ret.f = BSDF::DielectricSpecularTr(surface, 0) * func(wi_t);
            ret.bsdfOverPdf = ret.f / ret.pdf;
            ret.pdf *= 1 - eval.Fr.x;
            ret.wi = wi_t;
            ret.lobe = BSDF::LOBE::GLOSSY_T;
        }

        return ret;
    }

    // Either surface is translucent or diffuse lobe is intentionally ignored
    BSDFSample SampleBSDF_NoDiffuse(float3 normal, ShadingData surface, inout RNG rng)
    {
        float2 u_wh = rng.Uniform2D();
        float u_lobe = rng.Uniform();
        NoOp noop;

        return SampleBSDF_NoDiffuse<NoOp>(normal, surface, u_wh, u_lobe, noop);
    }

    // For surfaces without specular transmission
    template<typename Func>
    BSDFSample SampleBSDF_NoSpecTr(float3 normal, ShadingData surface, float2 u_r, float2 u_d, 
        float u_wrs_dr, float u_wrs_dt, Func func)
    {
        // Samples wh = n when surface is specular
        float3 wi_r = BSDF::SampleGloss(surface, normal, u_r);
        surface.SetWi_Refl(wi_r, normal);

        float pdf_r = BSDF::GlossPdf(surface);
        BSDFEval eval = BSDF::Unified(surface);
        float3 target = eval.f * func(wi_r);

        BSDFSample ret;
        ret.wi = wi_r;
        ret.lobe = BSDF::LOBE::GLOSSY_R;
        ret.f = target;

        // Metals don't have transmission or diffuse lobes
        if(surface.metallic || eval.tir)
        {
            ret.pdf = pdf_r;
            ret.bsdfOverPdf = target / ret.pdf;

            return ret;
        }

        // Streaming RIS using weighted reservoir sampling to sample aggregate BSDF
        float w_sum;

        // Specular/glossy reflection
        {
            float targetLum_r = Math::Luminance(target);
            float pdf_d = BSDF::DiffusePdf(surface);

            w_sum = RT::BalanceHeuristic(pdf_r, pdf_d, targetLum_r);
        }

        float pdf_d;
        float3 wi_d = BSDF::SampleDiffuse(normal, u_d, pdf_d);
        float fr_d;

        // Diffuse reflection
        {
            surface.SetWi_Refl(wi_d, normal);

            BSDFEval eval = BSDF::Unified(surface);
            float3 target_d = eval.f * func(wi_d);
            float targetLum_d = Math::Luminance(target_d);
            fr_d = eval.Fr.x;

            pdf_r = BSDF::GlossPdf(surface);
            float w = RT::BalanceHeuristic(pdf_d, pdf_r, targetLum_d);
            w_sum += w;

            if ((w_sum > 0) && (u_wrs_dr < (w / w_sum)))
            {
                target = target_d;

                ret.wi = wi_d;
                ret.lobe = BSDF::LOBE::DIFFUSE_R;
                ret.f = target_d;
            }
        }

        // Diffuse transmission
        if(surface.ThinWalled())
        {
            float3 wi_dt = -wi_d;
            surface.reflection = false;

            // Notes: 
            // 1. |n.wi_dt| = |n.wi_dr| so as following computation happens right after 
            //    diffuse reflection above, another SetWi*() call isn't needed. 
            // 2. SetWi*() calls don't change reflectance (refer to notes in SpecularReflectance()).
            float3 target_dt = (1 - BSDF::GGXReflectance_Dielectric(surface, fr_d)) * 
                BSDF::EvalDiffuse<false>(surface);
            target_dt *= func(wi_dt);
            float targetLum_dt = Math::Luminance(target_dt);

            float w = targetLum_dt / pdf_d;
            w_sum += w;

            if ((w_sum > 0) && (u_wrs_dt < (w / w_sum)))
            {
                target = target_dt;

                ret.wi = wi_dt;
                ret.lobe = BSDF::LOBE::DIFFUSE_T;
                ret.f = target_dt;
            }
        }

        float targetLum = Math::Luminance(target);
        ret.bsdfOverPdf = targetLum > 0 ? target * w_sum / targetLum : 0;
        ret.pdf = w_sum > 0 ? targetLum / w_sum : 0;

        return ret;
    }

    template<typename Func>
    BSDFSample SampleBSDF(float3 normal, ShadingData surface, float2 u_wh, float2 u_d,
        float u_wrs_1, float u_wrs_2, Func func)
    {
        if(!surface.specTr)
            return SampleBSDF_NoSpecTr(normal, surface, u_wh, u_d, u_wrs_1, u_wrs_2, func);

        return BSDF::SampleBSDF_NoDiffuse(normal, surface, u_wh, u_wrs_1, func);
    }

    template<typename Func>
    BSDFSample SampleBSDF(float3 normal, ShadingData surface, Func func, inout RNG rng)
    {
        float2 u_wh = rng.Uniform2D();
        float2 u_d = rng.Uniform2D();
        float u_wrs_1 = rng.Uniform();
        float u_wrs_2 = rng.Uniform();

        if(!surface.specTr)
            return SampleBSDF_NoSpecTr(normal, surface, u_wh, u_d, u_wrs_1, u_wrs_2, func);

        return BSDF::SampleBSDF_NoDiffuse(normal, surface, u_wh, u_wrs_1, func);
    }

    BSDFSample SampleBSDF(float3 normal, ShadingData surface, inout RNG rng)
    {
        // Make sure the number of random numbers used in all code paths is the same. Since
        // .UniformX() calls change the internal state of rng, they're not compiled out.
        float2 u_wh = rng.Uniform2D();
        float2 u_d = rng.Uniform2D();
        float u_wrs_1 = rng.Uniform();
        float u_wrs_2 = rng.Uniform();
        NoOp noop;

        if(!surface.specTr)
            return SampleBSDF_NoSpecTr(normal, surface, u_wh, u_d, u_wrs_1, u_wrs_2, noop);

        return BSDF::SampleBSDF_NoDiffuse(normal, surface, u_wh, u_wrs_1, noop);
    }

    template<typename Func>
    BSDFSamplerEval EvalBSDFSampler_NoSpecTr(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, float2 u_r, float2 u_d, Func func)
    {
        BSDFSamplerEval ret;
        const float3 targetScale_z = func(wi);

        if(surface.metallic)
        {
            surface.SetWi_Refl(wi, normal);
            const float3 fr = surface.Fresnel();

            ret.pdf = BSDF::GlossPdf(surface);
            ret.bsdfOverPdf = !surface.invalid * BSDF::GlossOverPdf(surface, fr);
            ret.bsdfOverPdf *= targetScale_z;

            return ret;
        }

        float w_sum;
        float3 target;

        // Specular/glossy reflection
        {
            const bool isZ_r = lobe == BSDF::LOBE::GLOSSY_R;
            const float3 wi_r = isZ_r ? wi : BSDF::SampleGloss(surface, normal, u_r);
            surface.SetWi_Refl(wi_r, normal);

            const float3 targetScale_r = isZ_r ? targetScale_z : func(wi_r);
            target = BSDF::Unified(surface).f * targetScale_r;
            const float targetLum_r = Math::Luminance(target);

            const float pdf_r = BSDF::GlossPdf(surface);
            const float pdf_d = BSDF::DiffusePdf(surface);
            w_sum = RT::BalanceHeuristic(pdf_r, pdf_d, targetLum_r);
        }

        float3 w_d = BSDF::SampleDiffuse(normal, u_d);
        float fr_d;

        // Diffuse reflection
        {
            const bool isZ_dr = lobe == BSDF::LOBE::DIFFUSE_R;
            const float3 wi_d = isZ_dr ? wi : w_d;
            surface.SetWi_Refl(wi_d, normal);

            const float3 targetScale_dr = isZ_dr ? targetScale_z : func(wi_d);
            BSDFEval eval = BSDF::Unified(surface);
            const float3 target_dr = eval.f * targetScale_dr;
            fr_d = eval.Fr.x;
            const float targetLum_dr = Math::Luminance(target_dr);

            const float pdf_r = BSDF::GlossPdf(surface);
            const float pdf_d = BSDF::DiffusePdf(surface);
            w_sum += RT::BalanceHeuristic(pdf_d, pdf_r, targetLum_dr);
            target = isZ_dr ? target_dr : target;
        }

        if(surface.ThinWalled())
        {
            const bool isZ_dt = lobe == BSDF::LOBE::DIFFUSE_T;
            const float3 wi_dt = isZ_dt ? wi : -w_d;

            const float3 targetScale_dt = isZ_dt ? targetScale_z : func(wi_dt);
            const float3 target_dt = (1 - BSDF::GGXReflectance_Dielectric(surface, fr_d)) * 
                BSDF::EvalDiffuse<false>(surface) * targetScale_dt;
            const float targetLum_dt = Math::Luminance(target_dt);

            const float pdf_d = BSDF::DiffusePdf(surface);
            w_sum += targetLum_dt / pdf_d;
            target = isZ_dt ? target_dt : target;
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
        const float wh_pdf = BSDF::GGXMicrofacetPdf(surface.alpha, surface.ndotwh, surface.ndotwo);

        bool tir;
        const float3 fr = surface.Fresnel(tir);
        const float3 targetScale = func(wi);

        BSDFSamplerEval ret;
        ret.bsdfOverPdf = !surface.invalid * surface.reflection * BSDF::GlossOverPdf(surface, fr);
        ret.bsdfOverPdf *= targetScale;
        ret.pdf = !surface.specular ? wh_pdf / 4.0f : surface.ndotwh >= MIN_N_DOT_H_SPECULAR;

        if(surface.metallic || !surface.specTr || tir)
            return ret;

        if(lobe == BSDF::LOBE::GLOSSY_R)
        {
            ret.bsdfOverPdf /= fr;
            ret.pdf *= fr.x;
            return ret;
        }

        // Since we're dividing by 1 - fr, passing 0 for fr causes 1 - fr to cancel out
        ret.bsdfOverPdf = !surface.invalid * !surface.reflection * 
            BSDF::TranslucentTrOverPdf(surface, 0);

        ret.bsdfOverPdf *= surface.specular ? 1 : 1 - BSDF::GGXReflectance_Dielectric(surface, fr.x);
        ret.bsdfOverPdf *= targetScale;
        ret.pdf = 1 - fr.x;
        ret.pdf *= surface.specular ? surface.ndotwh >= MIN_N_DOT_H_SPECULAR : wh_pdf * surface.whdotwo;

        if(!surface.specular)
        {
            float dwh_dwi = BSDF::JacobianHalfVecToIncident_Tr(surface.eta, 
                surface.whdotwo, surface.whdotwi);
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

        if(!surface.specTr)
            return EvalBSDFSampler_NoSpecTr(normal, surface, wi, lobe, u_wh, u_d, func);

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

        if(!surface.specTr)
            return EvalBSDFSampler_NoSpecTr(normal, surface, wi, lobe, u_wh, u_d, noop);

        return EvalBSDFSampler_NoDiffuse(normal, surface, wi, lobe, u_wrs_r, noop);
    }

    float BSDFSamplerPdf_NoDiffuse(float3 normal, BSDF::ShadingData surface, float3 wi)
    {
        surface.SetWi(wi, normal);
        const float wh_pdf = BSDF::GGXMicrofacetPdf(surface.alpha, surface.ndotwh, surface.ndotwo);

        if(surface.metallic || !surface.specTr)
        {
            float pdf = surface.specular ? 
                surface.ndotwh >= MIN_N_DOT_H_SPECULAR : 
                wh_pdf / 4.0f;
            return pdf * surface.reflection;
        }

        float3 fr = surface.Fresnel();
        float pdf = surface.specular ? surface.ndotwh >= MIN_N_DOT_H_SPECULAR : 1;

        if(surface.reflection)
            return fr.x * pdf * (surface.specular ? 1 : (wh_pdf / 4.0f));

        pdf *= 1 - fr.x;

        if(!surface.specular)
        {
            pdf *= wh_pdf * surface.whdotwo;
            float dwh_dwi = BSDF::JacobianHalfVecToIncident_Tr(surface.eta, 
                surface.whdotwo, surface.whdotwi);
            pdf *= dwh_dwi;
        }

        return pdf;
    }

    float BSDFSamplerPdf(float3 normal, BSDF::ShadingData surface, float3 wi_z, inout RNG rng)
    {
        if(surface.specTr)
            return BSDFSamplerPdf_NoDiffuse(normal, surface, wi_z);

        surface.SetWi(wi_z, normal);
        if(!surface.reflection && !surface.ThinWalled())
            return 0;

        BSDFEval eval_z = BSDF::Unified(surface);

        // GGX reflection is the only lobe
        if(surface.metallic || eval_z.tir)
            return BSDF::GlossPdf(surface);

        // Surface is opaque dielectric or thin walled

        // Using the law of total probability:
        //      p(wi_z) = p(wi_z, lobe = lobe_0) + ... + p(wi_z, lobe = lobe_k)
        float w_sum_gr;
        float w_sum_dr;
        float w_sum_dt;
        float targetLum;

        // wi_z
        {
            targetLum = Math::Luminance(eval_z.f);

            float pdf_r = BSDF::GlossPdf(surface);
            float pdf_d = BSDF::DiffusePdf(surface);
            float w = surface.reflection ? RT::BalanceHeuristic(pdf_r, pdf_d, targetLum) : 
                targetLum / pdf_d;
            w_sum_gr = w;
            w_sum_dr = w;
            w_sum_dt = w;
        }

        float pdf_d;
        float3 wi_d = BSDF::SampleDiffuse(normal, rng.Uniform2D(), pdf_d);
        float fr_d;

        // wi_dr
        {
            surface.SetWi_Refl(wi_d, normal);
            BSDFEval eval = BSDF::Unified(surface);
            fr_d = eval.Fr.x;
            float3 target_dr = eval.f;
            float targetLum_dr = Math::Luminance(target_dr);

            float pdf_gr = BSDF::GlossPdf(surface);
            float w = RT::BalanceHeuristic(pdf_d, pdf_gr, targetLum_dr);
            w_sum_gr += w;
            w_sum_dt += w;
        }

        // wi_dt
        if(surface.ThinWalled())
        {
            float3 target_dt = (1 - BSDF::GGXReflectance_Dielectric(surface, fr_d)) * 
                BSDF::EvalDiffuse<false>(surface);
            float targetLum_dt = Math::Luminance(target_dt);

            float w = targetLum_dt / pdf_d;
            w_sum_gr += w;
            w_sum_dr += w;
        }

        // wi_gr
        {
            float3 wi_r = BSDF::SampleGloss(surface, normal, rng.Uniform2D());
            surface.SetWi_Refl(wi_r, normal);
            float3 target_r = BSDF::Unified(surface).f;
            float targetLum_r = Math::Luminance(target_r);

            // Account for change of density from half vector to incident vector
            float pdf_r = BSDF::GlossPdf(surface);
            float pdf_d = BSDF::DiffusePdf(surface);
            float w = RT::BalanceHeuristic(pdf_r, pdf_d, targetLum_r);
            w_sum_dr += w;
            w_sum_dt += w;
        }

        // p(wi_z, lobe = microfacet)
        float pdf = w_sum_gr > 0 ? targetLum / w_sum_gr : 0;
        // p(wi_z, lobe = diffuse reflection)
        pdf += w_sum_dr > 0 ? targetLum / w_sum_dr : 0;
        // p(wi_z, lobe = diffuse transmission)
        pdf += surface.ThinWalled() && (w_sum_dt > 0) ? targetLum / w_sum_dt : 0;

        return pdf;
    }

    void AdvanceRNGDeterministic(int numBounces, int maxDiffuseBounces, inout RNG rng)
    {
        if(numBounces > 0)
        {
            int bounce = 0;

            while(true)
            {
                bounce++;

                if(bounce >= numBounces)
                    break;

                if(bounce < maxDiffuseBounces)
                {
                    rng.Uniform3D();
                    rng.Uniform3D();
                }
                else
                    rng.Uniform3D();
            }
        }
    }
}

#endif