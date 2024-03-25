#ifndef RESTIR_NEE_H
#define RESTIR_NEE_H

#include "../Common/GBuffers.hlsli"
#include "../Common/LightVoxelGrid.hlsli"
#include "RayQuery.hlsli"
#include "../Common/BSDFSampling.hlsli"

namespace ReSTIR_Util
{
    struct Globals
    {
        RaytracingAccelerationStructure bvh;
        StructuredBuffer<RT::MeshInstance> frameMeshData;
        StructuredBuffer<Vertex> vertices;
        StructuredBuffer<uint> indices;
        StructuredBuffer<Material> materials;
        StructuredBuffer<RT::EmissiveTriangle> emissives;
        StructuredBuffer<RT::PresampledEmissiveTriangle> sampleSets;
        StructuredBuffer<RT::EmissiveLumenAliasTableEntry> aliasTable;
        StructuredBuffer<RT::VoxelSample> lvg;
        uint16 maxDiffuseBounces;
        uint16 maxGlossyBounces_NonTr;
        uint16 maxGlossyBounces_Tr;
        uint16 maxNumBounces;
        uint16 sampleSetSize;
        uint16_t3 gridDim;
        half3 extents;
        half offset_y;
        bool russianRoulette;
    };

    struct DirectLightingEstimate
    {
        static DirectLightingEstimate Init()
        {
            DirectLightingEstimate ret;
            ret.ld = 0;     // le x BSDF x cos(theta) x dwdA / pdf
            ret.le = 0;
            ret.wi = 0;
            ret.pdf_solidAngle = 0;
            ret.dwdA = 1;
            ret.lt = Light::TYPE::NONE;
            ret.ID = UINT32_MAX;
            ret.pos = 0;
            ret.pdf_light = 0;
            ret.twoSided = true;

            return ret;
        }

        bool Empty()
        {
            return this.lt == Light::TYPE::NONE;
        }

        float3 ld;
        float3 le;
        float3 wi;
        float3 pos;
        float3 normal;
        float pdf_solidAngle;
        float dwdA;
        Light::TYPE lt;
        BSDF::LOBE lobe;
        uint ID;
        // p(lightSource, lightPos)
        float pdf_light;
        bool twoSided;
    };

    struct SkyIncidentRadiance 
    {
        static SkyIncidentRadiance Init(uint descHeapOffset)
        {
            SkyIncidentRadiance ret;
            ret.skyViewDescHeapOffset = descHeapOffset;
            return ret;
        }

        float3 operator()(float3 w)
        {
            return Light::Le_Sky(w, skyViewDescHeapOffset);
        }

        uint skyViewDescHeapOffset;
    };

    template<bool TestVisibility>
    DirectLightingEstimate NEE_Sun(float3 pos, float3 normal, BSDF::ShadingData surface, 
        RaytracingAccelerationStructure g_bvh, ConstantBuffer<cbFrameConstants> g_frame, inout RNG rng)
    {
        DirectLightingEstimate ret = DirectLightingEstimate::Init();
        ret.pdf_solidAngle = 1;
        ret.dwdA = 1;
        ret.lt = Light::TYPE::SUN;
        ret.lobe = BSDF::LOBE::ALL;

#if SUN_DISK_SAMPLING == 1
        float3 bsdfxCosTheta;
        float pdf;
        float3 wi = Light::SampleSunDirection(-g_frame.SunDir, g_frame.SunCosAngularRadius, 
            normal, surface, bsdfxCosTheta, pdf, rng);
#else
        float3 wi = -g_frame.SunDir;
        surface.SetWi(wi, normal);
        float3 bsdfxCosTheta = BSDF::UnifiedBSDF(surface);
#endif

        if(dot(bsdfxCosTheta, bsdfxCosTheta) == 0)
            return ret;

        if(TestVisibility)
        {
            if (!ReSTIR_RT::Visibility_Ray(pos, wi, normal, g_bvh, surface.HasSpecularTransmission()))
                return ret;
        }

        float3 le = Light::Le_Sun(pos, g_frame);
        ret.ld = bsdfxCosTheta * le;
#if SUPPRESS_SUN_FIREFLIES == 1
        ret.ld = min(ret.ld, le);
#endif
        ret.le = le;
        ret.wi = wi;

        return ret;
    }

    DirectLightingEstimate NEE_Sky(float3 pos, float3 normal, BSDF::ShadingData surface, 
        RaytracingAccelerationStructure g_bvh, uint skyViewDescHeapOffset, inout RNG rng)
    {
        float2 u_wh = rng.Uniform2D();
        float2 u_d = rng.Uniform2D();
        float u_wrs_r = rng.Uniform();
        float u_wrs_d = rng.Uniform();

        // Weighted reservoir sampling to sample Le x BRDF, with BRDF lobes as source distributions
        SkyIncidentRadiance leFunc = SkyIncidentRadiance::Init(skyViewDescHeapOffset);
        BSDF::BSDFSample bsdfSample = BSDF::SampleBSDF(normal, surface, u_wh, u_d, u_wrs_r, u_wrs_d, leFunc);

        DirectLightingEstimate ret = DirectLightingEstimate::Init();
        ret.dwdA = 1;
        ret.lt = Light::TYPE::SKY;
        ret.lobe = bsdfSample.lobe;
        ret.wi = bsdfSample.wi;
        // = Le * BSDF(wi) / pdf
        ret.ld = bsdfSample.bsdfOverPdf;
        ret.pdf_solidAngle = bsdfSample.pdf;
        // ret.le = Light::Le_Sky(ret.wi, skyViewDescHeapOffset);

        if(dot(ret.ld, ret.ld) > 0)
            ret.ld *= ReSTIR_RT::Visibility_Ray(pos, ret.wi, normal, g_bvh, surface.HasSpecularTransmission());

        return ret;
    }

    template<int NumSamples>
    DirectLightingEstimate NEE_Emissive(float3 pos, float3 normal, BSDF::ShadingData surface, uint sampleSetIdx,
        uint numEmissives, ReSTIR_Util::Globals globals, uint emissiveMapsDescHeapOffset, inout RNG rng)
    {
        DirectLightingEstimate ret = DirectLightingEstimate::Init();

        [loop]
        for (int s_l = 0; s_l < NumSamples; s_l++)
        {
#if defined(USE_PRESAMPLED_SETS)
            RT::PresampledEmissiveTriangle tri = Light::SamplePresampledSet(sampleSetIdx, globals.sampleSets, 
                globals.sampleSetSize, rng);

            Light::EmissiveTriSample lightSample;
            lightSample.pos = tri.pos;
            lightSample.normal = Math::DecodeOct32(tri.normal);
            lightSample.bary = Math::DecodeUNorm2(tri.bary);

            float3 le = tri.le;
            const float lightPdf = tri.pdf;
            const uint emissiveIdx = tri.idx;
            const uint lightID = tri.ID;

            if(tri.twoSided && dot(pos - tri.pos, lightSample.normal) < 0)
                lightSample.normal *= -1;
#else
            Light::AliasTableSample entry = Light::AliasTableSample::get(globals.aliasTable, 
                numEmissives, rng);
            RT::EmissiveTriangle tri = globals.emissives[entry.idx];
            Light::EmissiveTriSample lightSample = Light::EmissiveTriSample::get(pos, tri, rng);

            float3 le = Light::Le_EmissiveTriangle(tri, lightSample.bary, emissiveMapsDescHeapOffset);
            const float lightPdf = entry.pdf * lightSample.pdf;
            const uint lightID = tri.ID;
#endif

            const float t = length(lightSample.pos - pos);
            const float3 wi = (lightSample.pos - pos) / t;

            if(dot(lightSample.normal, -wi) > 0)
            {
                const float dwdA = saturate(dot(lightSample.normal, -wi)) / (t * t);

                surface.SetWi(wi, normal);
                float3 ld = le * BSDF::UnifiedBSDF(surface) * dwdA;
                    
                if (Math::Luminance(ld) > 1e-6)
                {
                    ld *= ReSTIR_RT::Visibility_Segment(pos, wi, t, normal, lightID, 
                        globals.bvh, surface.HasSpecularTransmission());
                }

                ret.ld += ld / lightPdf;
                ret.le = le;
                ret.wi = wi;
                ret.pdf_solidAngle = lightPdf / dwdA;
                ret.dwdA = dwdA;
                ret.ID = lightID;
                ret.pos = lightSample.pos;
                ret.normal = lightSample.normal;
                ret.pdf_light = lightPdf;
            }
        }

        ret.lt = Light::TYPE::EMISSIVE;
        ret.lobe = BSDF::LOBE::ALL;
        ret.ld /= NumSamples;

        return ret;
    }
}

#endif