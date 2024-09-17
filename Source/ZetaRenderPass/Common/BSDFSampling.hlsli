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
        static BSDFSamplerEval Init()
        {
            BSDFSamplerEval ret;
            ret.bsdfOverPdf = 0;
            ret.pdf = 0;

            return ret;
        }

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
    BSDFSample SampleBSDF_NoDiffuse(float3 normal, ShadingData surface, float2 u_c, 
        float2 u_g, float u_wrs_0, float u_wrs_1, Func func)
    {
        BSDFSample ret;
        float pdf_base = 1;

        if(surface.Coated())
        {
            float Fr0 = DielectricF0(surface.coat_eta);
            float reflectance_c = GGXReflectanceApprox(Fr0, surface.coat_alpha, surface.ndotwo).x;
            float pdf_coat = reflectance_c * surface.coat_weight;
            pdf_base = 1 - pdf_coat;

            if(u_wrs_0 < pdf_coat)
            {
                float3 wi_c = BSDF::SampleCoat(surface, normal, u_c);
                surface.SetWi_Refl(wi_c, normal);

                BSDFEval eval = BSDF::Unified(surface);
                float3 target = eval.f * func(wi_c);

                ret.wi = wi_c;
                ret.lobe = BSDF::LOBE::COAT;
                ret.f = target;
                ret.pdf = BSDF::CoatPdf(surface) * pdf_coat;
                ret.bsdfOverPdf = ret.f / ret.pdf;

                return ret;
            }
        }

        float3 wh = surface.GlossSpecular() ? normal : BSDF::SampleGGXMicrofacet(surface.wo, 
            surface.alpha, normal, u_g);
        float3 wi_r = reflect(-surface.wo, wh);
        surface.SetWi_Refl(wi_r, normal, wh);
        float wh_pdf = BSDF::GGXMicrofacetPdf(surface.alpha, surface.ndotwh, surface.ndotwo);

        ret.wi = wi_r;
        ret.lobe = BSDF::LOBE::GLOSSY_R;
        // Account for change of density from half vector to incident vector
        ret.pdf = surface.GlossSpecular() ? 1 : wh_pdf / 4.0f;
        ret.pdf *= pdf_base;

        BSDFEval eval = BSDF::Unified(surface);
        ret.f = eval.f * func(wi_r);
        ret.bsdfOverPdf = ret.f / ret.pdf;

        // TIR is for both coat and gloss layers
        if(surface.metallic || !surface.specTr || eval.tir)
            return ret;

        // Transmissive dielectric surface

        // Use Fresnel to decide between reflection and transmission
        if (u_wrs_1 < eval.Fr_g.x)
        {
            ret.bsdfOverPdf /= eval.Fr_g.x;
            ret.pdf *= eval.Fr_g.x;
        }
        else
        {
            float3 wi_t = refract(-surface.wo, wh, 1 / surface.eta);
            surface.SetWi_Tr(wi_t, normal, wh);
            // 1 - fr is multiplied below
            ret.pdf = 1;

            if(!surface.GlossSpecular())
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

            // BSDF and pdf both contain 1 - fr, by passing 0 it cancels out
            ret.f = BSDF::DielectricBaseSpecularTr(surface, 0) * func(wi_t);
            ret.bsdfOverPdf = ret.f / ret.pdf;
            ret.bsdfOverPdf /= pdf_base;
            ret.pdf *= 1 - eval.Fr_g.x;
            ret.pdf *= pdf_base;
            ret.wi = wi_t;
            ret.lobe = BSDF::LOBE::GLOSSY_T;
        }

        return ret;
    }

    // Either surface is translucent or diffuse lobe is intentionally ignored
    BSDFSample SampleBSDF_NoDiffuse(float3 normal, ShadingData surface, inout RNG rng)
    {
        float2 u_c = rng.Uniform2D();
        float2 u_g = rng.Uniform2D();
        float u_wrs_0 = rng.Uniform();
        float u_wrs_1 = rng.Uniform();
        NoOp noop;

        return SampleBSDF_NoDiffuse<NoOp>(normal, surface, u_c, u_g, u_wrs_0, u_wrs_1, 
            noop);
    }

    // For surfaces without specular transmission
    template<typename Func>
    BSDFSample SampleBSDF_NoSpecTr(float3 normal, ShadingData surface, float2 u_coat, float2 u_g, 
        float2 u_d, float u_wrs_g, float u_wrs_dr, float u_wrs_dt, Func func)
    {
        BSDFSample ret = BSDFSample::Init();

        // Streaming RIS using weighted reservoir sampling to sample aggregate BSDF
        float w_sum = 0;
        float3 target = 0;

        // Sample coat layer
        if(surface.Coated())
        {
            float3 wi_c = BSDF::SampleCoat(surface, normal, u_coat);
            surface.SetWi_Refl(wi_c, normal);

            BSDFEval eval = BSDF::Unified(surface);
            target = eval.f * func(wi_c);

            ret.wi = wi_c;
            ret.lobe = BSDF::LOBE::COAT;
            ret.f = target;

            float pdf_c = BSDF::CoatPdf(surface);
            float pdf_g = BSDF::GlossPdf(surface);
            float pdf_d = !surface.metallic ? BSDF::DiffusePdf(surface) : 0;
            float targetLum_c = Math::Luminance(target);
            w_sum = RT::BalanceHeuristic3(pdf_c, pdf_g, pdf_d, targetLum_c);
        }

        // Specular/glossy reflection
        {
            float3 wi_g = BSDF::SampleGloss(surface, normal, u_g);
            surface.SetWi_Refl(wi_g, normal);

            BSDFEval eval = Unified(surface);
            float3 target_g = eval.f * func(wi_g);
            
            float pdf_g = BSDF::GlossPdf(surface);
            float pdf_d = !surface.metallic && !eval.tir ? BSDF::DiffusePdf(surface) : 0;
            float pdf_c = surface.Coated() ? BSDF::CoatPdf(surface) : 0;
            float w_g = RT::BalanceHeuristic3(pdf_g, pdf_d, pdf_c, Math::Luminance(target_g));
            w_sum += w_g;

            if ((w_sum > 0) && (u_wrs_g < (w_g / w_sum)))
            {
                target = target_g;

                ret.wi = wi_g;
                ret.lobe = BSDF::LOBE::GLOSSY_R;
                ret.f = target_g;
            }
        }

        // Metals don't have transmission or diffuse lobes
        if(!surface.metallic)
        {
            float pdf_d;
            float3 wi_d = BSDF::SampleDiffuse(normal, u_d, pdf_d);
            float Fr_g;

            // Diffuse reflection
            {
                surface.SetWi_Refl(wi_d, normal);

                BSDFEval eval = Unified(surface);
                float3 target_dr = eval.f * func(wi_d);
                Fr_g = eval.Fr_g.x;

                float pdf_g = BSDF::GlossPdf(surface);
                float pdf_c = surface.Coated() ? BSDF::CoatPdf(surface) : 0;
                float w_dr = RT::BalanceHeuristic3(pdf_d, pdf_g, pdf_c, Math::Luminance(target_dr));
                w_sum += w_dr;

                if ((w_sum > 0) && (u_wrs_dr < (w_dr / w_sum)))
                {
                    target = target_dr;

                    ret.wi = wi_d;
                    ret.lobe = BSDF::LOBE::DIFFUSE_R;
                    ret.f = target_dr;
                }
            }

            // Diffuse transmission
            if(surface.ThinWalled())
            {
                float3 wi_dt = -wi_d;

                // Notes: 
                // 1. |n.wi_dt| = |n.wi_dr| and following computation happens right after 
                //    diffuse reflection above, another SetWi*() call isn't needed. 
                // 2. SetWi*() calls don't change reflectance.
                float3 target_dt = BSDF::DielectricBaseDiffuseTr(surface, Fr_g);
                target_dt *= func(wi_dt);

                float w_dt = Math::Luminance(target_dt) / pdf_d;
                w_sum += w_dt;

                if ((w_sum > 0) && (u_wrs_dt < (w_dt / w_sum)))
                {
                    target = target_dt;

                    ret.wi = wi_dt;
                    ret.lobe = BSDF::LOBE::DIFFUSE_T;
                    ret.f = target_dt;
                }
            }
        }

        float targetLum = Math::Luminance(target);
        ret.bsdfOverPdf = targetLum > 0 ? target * w_sum / targetLum : 0;
        ret.pdf = w_sum > 0 ? targetLum / w_sum : 0;

        return ret;
    }

    template<typename Func>
    BSDFSample SampleBSDF(float3 normal, ShadingData surface, Func func, inout RNG rng)
    {
        float2 u_c = rng.Uniform2D();
        float2 u_g = rng.Uniform2D();
        float2 u_d = rng.Uniform2D();
        float u_wrs_0 = rng.Uniform();
        float u_wrs_1 = rng.Uniform();
        float u_wrs_2 = rng.Uniform();

        if(!surface.specTr)
        {
            return SampleBSDF_NoSpecTr(normal, surface, u_c, u_g, u_d, u_wrs_0, 
                u_wrs_1, u_wrs_2, func);
        }

        return BSDF::SampleBSDF_NoDiffuse(normal, surface, u_c, u_g, u_wrs_0, 
            u_wrs_1, func);
    }

    BSDFSample SampleBSDF(float3 normal, ShadingData surface, inout RNG rng)
    {
        // Make sure the number of random numbers used in all code paths is the same. Since
        // .UniformX() calls change the internal state of rng, they're not compiled out.
        float2 u_c = rng.Uniform2D();
        float2 u_g = rng.Uniform2D();
        float2 u_d = rng.Uniform2D();
        float u_wrs_0 = rng.Uniform();
        float u_wrs_1 = rng.Uniform();
        float u_wrs_2 = rng.Uniform();
        NoOp noop;

        if(!surface.specTr)
        {
            return SampleBSDF_NoSpecTr(normal, surface, u_c, u_g, u_d, u_wrs_0, 
                u_wrs_1, u_wrs_2, noop);
        }

        return BSDF::SampleBSDF_NoDiffuse(normal, surface, u_c, u_g, u_wrs_0, 
            u_wrs_1, noop);
    }

    template<typename Func>
    BSDFSamplerEval EvalBSDFSampler_NoSpecTr(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, float2 u_c, float2 u_g, float2 u_d, Func func)
    {
        BSDFSamplerEval ret;
        const float3 targetScale_z = func(wi);
        float w_sum = 0;
        float3 target = 0;

        // Coat
        if(surface.Coated())
        {
            const bool isZ_c = lobe == BSDF::LOBE::COAT;
            const float3 wi_c = isZ_c ? wi : BSDF::SampleCoat(surface, normal, u_c);
            surface.SetWi_Refl(wi_c, normal);

            const float3 targetScale_c = isZ_c ? targetScale_z : func(wi_c);
            target = BSDF::Unified(surface).f * targetScale_c;
            const float targetLum_c = Math::Luminance(target);

            const float pdf_c = BSDF::CoatPdf(surface);
            const float pdf_g = BSDF::GlossPdf(surface);
            const float pdf_d = !surface.metallic ? BSDF::DiffusePdf(surface) : 0;
            w_sum = RT::BalanceHeuristic3(pdf_c, pdf_g, pdf_d, targetLum_c);
        }

        // Specular/glossy reflection
        {
            const bool isZ_g = lobe == BSDF::LOBE::GLOSSY_R;
            const float3 wi_g = isZ_g ? wi : BSDF::SampleGloss(surface, normal, u_g);
            surface.SetWi_Refl(wi_g, normal);

            const float3 targetScale_g = isZ_g ? targetScale_z : func(wi_g);
            const float3 target_g = BSDF::Unified(surface).f * targetScale_g;
            const float targetLum_g = Math::Luminance(target_g);

            const float pdf_g = BSDF::GlossPdf(surface);
            const float pdf_d = !surface.metallic ? BSDF::DiffusePdf(surface) : 0;
            const float pdf_c = surface.Coated() ? BSDF::CoatPdf(surface) : 0;
            w_sum += RT::BalanceHeuristic3(pdf_g, pdf_d, pdf_c, targetLum_g);
            target = isZ_g ? target_g : target;
        }

        if(!surface.metallic)
        {
            float3 w_d = BSDF::SampleDiffuse(normal, u_d);
            float Fr_g;

            // Diffuse reflection
            {
                const bool isZ_dr = lobe == BSDF::LOBE::DIFFUSE_R;
                const float3 wi_d = isZ_dr ? wi : w_d;
                surface.SetWi_Refl(wi_d, normal);

                const float3 targetScale_dr = isZ_dr ? targetScale_z : func(wi_d);
                BSDFEval eval = BSDF::Unified(surface);
                const float3 target_dr = eval.f * targetScale_dr;
                Fr_g = eval.Fr_g.x;
                const float targetLum_dr = Math::Luminance(target_dr);

                const float pdf_d = BSDF::DiffusePdf(surface);
                const float pdf_g = BSDF::GlossPdf(surface);
                const float pdf_c = surface.Coated() ? BSDF::CoatPdf(surface) : 0;
                w_sum += RT::BalanceHeuristic3(pdf_d, pdf_g, pdf_c, targetLum_dr);
                target = isZ_dr ? target_dr : target;
            }

            if(surface.ThinWalled())
            {
                const bool isZ_dt = lobe == BSDF::LOBE::DIFFUSE_T;
                const float3 wi_dt = isZ_dt ? wi : -w_d;

                const float3 targetScale_dt = isZ_dt ? targetScale_z : func(wi_dt);
                const float3 target_dt = BSDF::DielectricBaseDiffuseTr(surface, Fr_g) * targetScale_dt;
                const float targetLum_dt = Math::Luminance(target_dt);

                const float pdf_d = BSDF::DiffusePdf(surface);
                w_sum += targetLum_dt / pdf_d;
                target = isZ_dt ? target_dt : target;
            }
        }

        float targetLum = Math::Luminance(target);
        ret.bsdfOverPdf = targetLum > 0 ? target * w_sum / targetLum : 0;
        ret.pdf = w_sum > 0 ? targetLum / w_sum : 0;

        return ret;
    }

    template<typename Func>
    BSDFSamplerEval EvalBSDFSampler_NoDiffuse(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, float unused, Func func)
    {
        BSDFSamplerEval ret;

        surface.SetWi(wi, normal);
        BSDFEval eval = BSDF::Unified(surface);
        const float3 targetScale = func(wi);
        float pdf_base = 1;

        if(surface.Coated())
        {
            float Fr0 = DielectricF0(surface.coat_eta);
            float reflectance_c = GGXReflectanceApprox(Fr0, surface.coat_alpha, surface.ndotwo).x;
            float pdf_coat = reflectance_c * surface.coat_weight;
            pdf_base = 1 - pdf_coat;

            if(lobe == BSDF::LOBE::COAT)
            {
                ret.pdf = BSDF::CoatPdf(surface) * pdf_coat;
                ret.bsdfOverPdf = (eval.f * targetScale) / ret.pdf;

                return ret;
            }
        }

        const float wh_pdf = BSDF::GGXMicrofacetPdf(surface.alpha, surface.ndotwh, surface.ndotwo);
        ret.pdf = !surface.GlossSpecular() ? wh_pdf / 4.0f : surface.ndotwh >= MIN_N_DOT_H_SPECULAR;
        ret.pdf *= pdf_base;
        ret.bsdfOverPdf = (eval.f * targetScale) / ret.pdf;

        if(surface.metallic || !surface.specTr || eval.tir)
            return ret;

        if(lobe == BSDF::LOBE::GLOSSY_R)
        {
            ret.bsdfOverPdf /= eval.Fr_g.x;
            ret.pdf *= eval.Fr_g.x;

            return ret;
        }

        // Since we're dividing by 1 - fr, by passing 0 causes 1 - fr cancels out
        ret.bsdfOverPdf = !surface.invalid * !surface.reflection * 
            BSDF::TranslucentTrOverPdf(surface, 0);
        ret.bsdfOverPdf *= BSDF::TransmittanceToDielectricBaseTr(surface);
        ret.bsdfOverPdf *= targetScale;
        ret.bsdfOverPdf /= pdf_base;
        ret.pdf = 1 - eval.Fr_g.x;
        ret.pdf *= surface.GlossSpecular() ? surface.ndotwh >= MIN_N_DOT_H_SPECULAR : wh_pdf * surface.whdotwo;
        ret.pdf *= pdf_base;

        if(!surface.GlossSpecular())
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
        float2 u_c = rng.Uniform2D();
        float2 u_g = rng.Uniform2D();
        float u_wrs_0 = rng.Uniform();
        float u_wrs_1 = rng.Uniform();

        return EvalBSDFSampler_NoDiffuse(normal, surface, wi, lobe, u_wrs_0, func);
    }

    BSDFSamplerEval EvalBSDFSampler_NoDiffuse(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, inout RNG rng)
    {
        float2 u_c = rng.Uniform2D();
        float2 u_g = rng.Uniform2D();
        float u_wrs_0 = rng.Uniform();
        float u_wrs_1 = rng.Uniform();
        NoOp noop;

        // toto
        return EvalBSDFSampler_NoDiffuse(normal, surface, wi, lobe, u_wrs_0, noop);
    }

    // Mirrors the computation in SampleBSDF() to return joint probability of sampling lobe l_z 
    // and direction wi given the specific random numbers u_0, ..., u_n.
    template<typename Func>
    BSDFSamplerEval EvalBSDFSampler(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, Func func, inout RNG rng)
    {
        float2 u_c = rng.Uniform2D();
        float2 u_g = rng.Uniform2D();
        float2 u_d = rng.Uniform2D();
        float u_wrs_0 = rng.Uniform();
        float u_wrs_1 = rng.Uniform();
        float u_wrs_2 = rng.Uniform();

        if(!surface.specTr)
            return EvalBSDFSampler_NoSpecTr(normal, surface, wi, lobe, u_c, u_g, u_d, func);

        return EvalBSDFSampler_NoDiffuse(normal, surface, wi, lobe, u_wrs_1, func);
    }    
    
    BSDFSamplerEval EvalBSDFSampler(float3 normal, BSDF::ShadingData surface, float3 wi, 
        BSDF::LOBE lobe, inout RNG rng)
    {
        float2 u_c = rng.Uniform2D();
        float2 u_g = rng.Uniform2D();
        float2 u_d = rng.Uniform2D();
        float u_wrs_0 = rng.Uniform();
        float u_wrs_1 = rng.Uniform();
        float u_wrs_2 = rng.Uniform();
        NoOp noop;

        if(!surface.specTr)
            return EvalBSDFSampler_NoSpecTr(normal, surface, wi, lobe, u_c, u_g, u_d, noop);

        return EvalBSDFSampler_NoDiffuse(normal, surface, wi, lobe, u_wrs_1, noop);
    }

    float BSDFSamplerPdf_NoDiffuse(float3 normal, BSDF::ShadingData surface, float3 wi)
    {
        surface.SetWi(wi, normal);
        float pdf_base = 1;
        float pdf_c = 0;

        if(surface.Coated())
        {
            float Fr0 = DielectricF0(surface.coat_eta);
            float reflectance_c = GGXReflectanceApprox(Fr0, surface.coat_alpha, surface.ndotwo).x;
            float pdf_coat = reflectance_c * surface.coat_weight;
            pdf_base = 1 - pdf_coat;

            if(surface.reflection)
                pdf_c = BSDF::CoatPdf(surface) * pdf_coat;
        }

        const float wh_pdf = BSDF::GGXMicrofacetPdf(surface.alpha, surface.ndotwh, surface.ndotwo);

        // P(wi) = P(wi | lobe = gloss) + P(wi | lobe = coat)
        if(surface.metallic || !surface.specTr)
        {
            float pdf_gr = surface.GlossSpecular() ? 
                surface.ndotwh >= MIN_N_DOT_H_SPECULAR : 
                wh_pdf / 4.0f;
            pdf_gr *= pdf_base;

            return (pdf_c + pdf_gr) * surface.reflection;
        }

        // Transmissive dielectric

        float Fr_g = surface.Fresnel().x;
        float pdf_g = surface.GlossSpecular() ? surface.ndotwh >= MIN_N_DOT_H_SPECULAR : 1;
        pdf_g *= pdf_base;

        if(surface.reflection)
            return Fr_g * pdf_g * (surface.GlossSpecular() ? 1 : (wh_pdf / 4.0f));

        pdf_g *= 1 - Fr_g;

        if(!surface.GlossSpecular())
        {
            pdf_g *= wh_pdf * surface.whdotwo;
            float dwh_dwi = BSDF::JacobianHalfVecToIncident_Tr(surface.eta, 
                surface.whdotwo, surface.whdotwi);
            pdf_g *= dwh_dwi;
        }

        return pdf_g;
    }

    float BSDFSamplerPdf(float3 normal, BSDF::ShadingData surface, float3 wi_z, inout RNG rng)
    {
        if(surface.specTr)
            return BSDFSamplerPdf_NoDiffuse(normal, surface, wi_z);

        surface.SetWi(wi_z, normal);
        if(!surface.reflection && !surface.ThinWalled())
            return 0;

        BSDFEval eval_z = BSDF::Unified(surface);
        float targetLum = Math::Luminance(eval_z.f);

        if(targetLum == 0)
            return 0;

        // Surface is opaque dielectric or thin walled

        // Using the law of total probability:
        //      p(wi_z) = p(wi_z, lobe = lobe_0) + ... + p(wi_z, lobe = lobe_k)
        float w_sum_c;
        float w_sum_g;
        float w_sum_dr;
        float w_sum_dt;

        // wi_z
        {
            float pdf_g = BSDF::GlossPdf(surface);
            float pdf_d = !surface.metallic ? BSDF::DiffusePdf(surface) : 0;
            float pdf_c = surface.Coated() ? BSDF::CoatPdf(surface) : 0;
            float w = surface.reflection ? RT::BalanceHeuristic3(pdf_g, pdf_d, pdf_c, targetLum) : 
                (targetLum / pdf_d) * !surface.metallic;
            w_sum_g = w;
            w_sum_dr = w;
            w_sum_dt = w;
            w_sum_c = w;
        }

        if(w_sum_g == 0)
            return 0;

        float pdf_d;
        float3 wi_d = BSDF::SampleDiffuse(normal, rng.Uniform2D(), pdf_d);
        float Fr_g;

        // wi_dr
        if(!surface.metallic)
        {
            surface.SetWi_Refl(wi_d, normal);
            BSDFEval eval = BSDF::Unified(surface);
            Fr_g = eval.Fr_g.x;
            float3 target_dr = eval.f;
            float targetLum_dr = Math::Luminance(target_dr);

            float pdf_g = BSDF::GlossPdf(surface);
            float pdf_c = surface.Coated() ? BSDF::CoatPdf(surface) : 0;
            float w = RT::BalanceHeuristic3(pdf_d, pdf_g, pdf_c, targetLum_dr);
            w_sum_g += w;
            w_sum_dt += w;
            w_sum_c += w;
        }

        // wi_dt
        if(!surface.metallic && surface.ThinWalled())
        {
            float3 target_dt = BSDF::DielectricBaseDiffuseTr(surface, Fr_g);
            float targetLum_dt = Math::Luminance(target_dt);

            float w = targetLum_dt / pdf_d;
            w_sum_g += w;
            w_sum_dr += w;
            w_sum_c += w;
        }

        // wi_g
        {
            float3 wi_g = BSDF::SampleGloss(surface, normal, rng.Uniform2D());
            surface.SetWi_Refl(wi_g, normal);
            float3 target_g = BSDF::Unified(surface).f;
            float targetLum_g = Math::Luminance(target_g);

            // Account for change of density from half vector to incident vector
            float pdf_g = BSDF::GlossPdf(surface);
            float pdf_d = BSDF::DiffusePdf(surface);
            float pdf_c = surface.Coated() ? BSDF::CoatPdf(surface) : 0;
            float w = RT::BalanceHeuristic3(pdf_g, pdf_d, pdf_c, targetLum_g);
            w_sum_dr += w;
            w_sum_dt += w;
            w_sum_c += w;
        }

        // wi_c
        if(surface.Coated())
        {
            float3 wi_c = BSDF::SampleCoat(surface, normal, rng.Uniform2D());
            surface.SetWi_Refl(wi_c, normal);
            float3 target_c = BSDF::Unified(surface).f;
            float targetLum_c = Math::Luminance(target_c);

            // Account for change of density from half vector to incident vector
            float pdf_g = BSDF::GlossPdf(surface);
            float pdf_d = BSDF::DiffusePdf(surface);
            float pdf_c = BSDF::CoatPdf(surface);
            float w = RT::BalanceHeuristic3(pdf_g, pdf_d, pdf_c, targetLum_c);
            w_sum_g += w;
            w_sum_dr += w;
            w_sum_dt += w;
        }

        // p(wi_z, lobe = gloss)
        float pdf = w_sum_g > 0 ? targetLum / w_sum_g : 0;
        // p(wi_z, lobe = diffuse reflection)
        pdf += w_sum_dr > 0 ? targetLum / w_sum_dr : 0;
        // p(wi_z, lobe = coat)
        pdf += w_sum_c > 0 ? targetLum / w_sum_c : 0;
        // p(wi_z, lobe = diffuse transmission)
        pdf += surface.ThinWalled() && (w_sum_dt > 0) ? targetLum / w_sum_dt : 0;

        return pdf;
    }
}

#endif