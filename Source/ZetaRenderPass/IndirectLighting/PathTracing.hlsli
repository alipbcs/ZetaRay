#ifndef RESTIR_PATH_TRACING_H
#define RESTIR_PATH_TRACING_H

#include "IndirectLighting_Common.h"
#include "Params.hlsli"
#include "NEE.hlsli"

namespace ReSTIR_Util
{
    template<bool samplePoint>
    float3 SampleBRDF(float3 normal, BSDF::ShadingData surface, inout RNG rng, out float3 pdf)
    {
        // Metals don't have transmission or diffuse components.
        if(surface.IsMetallic())
        {
            float3 wi = BSDF::SampleMicrofacetBRDF(surface, normal, rng.Uniform2D());
            surface.SetWi_Refl(wi, normal);

            if(samplePoint)
                pdf.x = BSDF::VNDFReflectionPdf(surface);
            else
                pdf = BSDF::MicrofacetBRDFDivPdf(surface);

            return wi;
        }

        // Streaming RIS using weighted reservoir sampling to sample aggregate BRDF
        float w_sum;
        float3 wi;
        float3 target;
        float targetLum;

        // Sample specular/glossy reflection/transmission using VNDF
        {
            wi = BSDF::SampleMicrofacetBRDF(surface, normal, rng.Uniform2D());
            surface.SetWi_Refl(wi, normal);

            float pdf_g = BSDF::VNDFReflectionPdf(surface);
            float pdf_d;

            // if(samplePoint)
            //     pdf_d = !surface.backfacing_wi * ONE_OVER_2_PI;
            // else
                pdf_d = !surface.invalid * BSDF::LambertianBRDFPdf(surface);

            target = BSDF::DielectricBRDF(surface);
            targetLum = Math::Luminance(target);
            w_sum = RT::BalanceHeuristic(pdf_g, pdf_d, targetLum);
        }

        // Sample diffuse using hemisphere/cosine-weighted sampling
        {
            float pdf_d;
            float3 wi_d;
            
            // if(samplePoint)
            // {
            //     wi_d = Sampling::UniformSampleHemisphere(rng.Uniform2D(), pdf_d);

            //     // transform to world space
            //     float3 T;
            //     float3 B;
            //     Math::revisedONB(normal, T, B);
            //     wi_d = wi_d.x * T + wi_d.y * B + wi_d.z * normal;
            // }
            // else
                wi_d = BSDF::SampleLambertianBRDF(normal, rng.Uniform2D(), pdf_d);

            surface.SetWi_Refl(wi_d, normal);

            float pdf_g = BSDF::VNDFReflectionPdf(surface);
            float3 target_d = BSDF::DielectricBRDF(surface);
            float targetLum_d = Math::Luminance(target_d);
            float w = RT::BalanceHeuristic(pdf_d, pdf_g, targetLum_d);
            w_sum += w;

            if (rng.Uniform() < (w / max(1e-6f, w_sum)))
            {
                wi = wi_d;
                target = target_d;
                targetLum = targetLum_d;
            }
        }

        if(samplePoint)
            pdf.x = targetLum / max(w_sum, 1e-6);
        else
            pdf = target * (w_sum / max(targetLum, 1e-6));

        return wi;
    }

    // Streaming RIS using weighted reservoir sampling to sample BSDF
    template<bool samplePoint>
    float3 SampleDielectricBSDF_RIS(float3 normal, BSDF::ShadingData surface, inout RNG rng, out float3 pdfOrBsdfDivPdf)
    {
        float pdf;
        float3 bsdfDivPdf;

        float3 target = 0;
        float targetLum = 0;
        float w_sum = 0;

        float3 wh = surface.specular ? 
            normal : 
            BSDF::SampleMicrofacet(surface.wo, surface.alpha, normal, rng.Uniform2D());

        float wh_pdf = 1;

        // Evaluate VNDF
        if(!surface.specular)
        {
            surface.ndotwh = saturate(dot(normal, wh));
            float alphaSq = surface.alpha * surface.alpha;
            float NDF = BSDF::GGX(surface.ndotwh, alphaSq);
            float G1 = BSDF::SmithG1_GGX(alphaSq, surface.ndotwo);
            wh_pdf = (NDF * G1) / surface.ndotwo;
        }

        float3 wi = refract(-surface.wo, wh, 1 / surface.eta);
        bool tir = dot(wi, wi) == 0;

        // Specular/glossy transmission
        if(!tir)
        {
            surface.SetWi_Tr(wi, normal, wh);
            float pdf_t = 1;

            // Account for change of density from half vector to incident vector
            if(!surface.specular)
            {
                pdf_t = wh_pdf * surface.whdotwo;
                float denom = mad(surface.whdotwi, surface.eta, surface.whdotwo);
                denom *= denom;
                float dwm_dwi = surface.whdotwi / denom;
                pdf_t *= dwm_dwi;
            }

            target = BSDF::DielectricBTDF(surface);
            targetLum = Math::Luminance(target);
            w_sum = targetLum / max(pdf_t, 1e-6);
        }

        // Specular/glossy reflection
        {
            float3 wi_r = reflect(-surface.wo, wh);
            surface.SetWi_Refl(wi_r, normal, wh);

            float pdf_r = 1;

            // Account for change of density from half vector to incident vector
            if(!surface.specular)
                pdf_r = wh_pdf / 4.0f;

            // After using the law of reflection, VNDF samples might end up below the surface
            float pdf_d = !surface.invalid * 
                (surface.transmission != 1) * 
                BSDF::LambertianBRDFPdf(surface);

            float3 target_r = BSDF::DielectricBRDF(surface);
            float targetLum_r = Math::Luminance(target_r);
            float w = RT::BalanceHeuristic(pdf_r, pdf_d, targetLum_r);
            w_sum += w;

            if (rng.Uniform() < (w / max(1e-6f, w_sum)))
            {
                wi = wi_r;
                target = target_r;
                targetLum = targetLum_r;
            }
        }

        // Lambertian
        if(surface.transmission < 1)
        {
            float pdf_d;
            float3 wi_d = BSDF::SampleLambertianBRDF(normal, rng.Uniform2D(), pdf_d);
            surface.SetWi_Refl(wi_d, normal);

            float pdf_r = BSDF::VNDFReflectionPdf(surface);
            
            float3 target_d = BSDF::DielectricBRDF(surface);
            float targetLum_d = Math::Luminance(target_d);
            float w = RT::BalanceHeuristic(pdf_d, pdf_r, targetLum_d);
            w_sum += w;

            if (rng.Uniform() < (w / max(1e-6f, w_sum)))
            {
                wi = wi_d;
                target = target_d;
                targetLum = targetLum_d;
            }
        }

        bsdfDivPdf = targetLum > 0 ? target * (w_sum / targetLum) : 0;
        pdf = w_sum > 0 ? targetLum / w_sum : 0;

        if(samplePoint)
            pdfOrBsdfDivPdf.x = pdf;
        else
            pdfOrBsdfDivPdf = bsdfDivPdf;

        return wi;
    }

    float3 SampleDielectricBTDF(float3 normal, BSDF::ShadingData surface, inout RNG rng, out float3 bsdfDivPdf)
    {
        float3 wi = BSDF::SampleMicrofacetBTDF(surface, normal, rng.Uniform2D());
        surface.SetWi_Tr(wi, normal);

        float pdf = BSDF::VNDFTransmissionPdf(surface);
        float3 f = BSDF::DielectricBTDF(surface);
        bsdfDivPdf = pdf == 0 ? 0 : f / pdf;

        return wi;
    }

    float3 SampleBRDF_NoDiffuse(float3 normal, BSDF::ShadingData surface, inout RNG rng, out float3 brdfDivPdf)
    {
        float3 wi = BSDF::SampleMicrofacetBRDF(surface, normal, rng.Uniform2D());
        surface.SetWi_Refl(wi, normal);

        if(surface.IsMetallic())
            brdfDivPdf = BSDF::MicrofacetBRDFDivPdf(surface);
        else
        {
            float pdf = BSDF::VNDFReflectionPdf(surface);
            float3 f = BSDF::DielectricBRDF(surface);
            brdfDivPdf = pdf == 0 ? 0 : f / pdf;
        }

        return wi;
    }

    bool GetMaterialData(float3 wo, StructuredBuffer<Material> g_materials, ConstantBuffer<cbFrameConstants> g_frame, 
        float eta_curr, RT::RayCone rayCone, inout ReSTIR_RT::HitSurface hitInfo, 
        out BSDF::ShadingData surface, out float eta)
    {
        const Material mat = g_materials[hitInfo.matIdx];
        const bool hitBackface = dot(wo, hitInfo.normal) < 0;
        // Set to an arbitrary value to fix compiler error
        eta = DEFAULT_ETA_I;

        // Ray hit the backside of an opaque surface, no radiance can be reflected back.
        if(!mat.IsDoubleSided() && hitBackface)
            return false;

        // Reverse normal for double-sided surfaces
        if(mat.IsDoubleSided() && hitBackface)
            hitInfo.normal *= -1;

        float3 baseColor = Math::UnpackRGB(mat.BaseColorFactor);
        float metalness = mat.GetMetallic();
        float roughness = mat.GetRoughnessFactor();

        if (mat.BaseColorTexture != uint32_t(-1))
        {
            uint offset = NonUniformResourceIndex(g_frame.BaseColorMapsDescHeapOffset + mat.BaseColorTexture);
            BASE_COLOR_MAP g_baseCol = ResourceDescriptorHeap[offset];

            uint w;
            uint h;
            g_baseCol.GetDimensions(w, h);
            float mip = rayCone.MipmapLevel(hitInfo.lambda, w, h);
            mip += g_frame.MipBias;

            baseColor *= g_baseCol.SampleLevel(g_samLinearWrap, hitInfo.uv, mip).rgb;
        }

        const uint16_t metallicRoughnessTex = mat.GetMetallicRoughnessTex();

        if (metallicRoughnessTex != uint16_t(-1))
        {
            uint offset = NonUniformResourceIndex(g_frame.MetallicRoughnessMapsDescHeapOffset + metallicRoughnessTex);
            METALLIC_ROUGHNESS_MAP g_metalnessRoughnessMap = ResourceDescriptorHeap[offset];

            uint w;
            uint h;
            g_metalnessRoughnessMap.GetDimensions(w, h);
            float mip = rayCone.MipmapLevel(hitInfo.lambda, w, h);
            mip += g_frame.MipBias;

            float2 mr = g_metalnessRoughnessMap.SampleLevel(g_samLinearWrap, hitInfo.uv, mip);
            metalness *= mr.x;
            roughness *= mr.y;
        }

        float tr = mat.GetTransmission();
        eta = mat.GetIOR();

        // TODO surrounding medium is assumed to be always air
        float eta_t = eta_curr == 1.0f ? 1.0f : eta;
        float eta_i = eta_curr == 1.0f ? eta : 1.0f;

        surface = BSDF::ShadingData::Init(hitInfo.normal, wo, metalness >= MIN_METALNESS_METAL, 
            roughness, baseColor, eta_i, eta_t, tr);

        return true;
    }
}

#endif