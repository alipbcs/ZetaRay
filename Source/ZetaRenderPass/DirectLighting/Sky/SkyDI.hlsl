#include "Resampling.hlsli"

#define THREAD_GROUP_SWIZZLING 1

using namespace SkyDI_Util;

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_SkyDI> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

Reservoir RIS_InitialCandidates(uint2 DTid, float3 pos, float3 normal, 
    BSDF::ShadingData surface, inout RNG rng)
{
    Reservoir r = Reservoir::Init();

    // Sample sun
#if 1
    {
        Light::SunSample sunSample = Light::SunSample::get(-g_frame.SunDir, 
            g_frame.SunCosAngularRadius, normal, surface, rng);
        float3 wi_s = sunSample.wi;

        float3 target = 0;
        const bool trace = 
            // Sun below the horizon
            (wi_s.y > 0) &&
            // Sun hits the backside of non-transmissive surface
            ((dot(wi_s, normal) > 0) || surface.Transmissive()) &&
            // Make sure BSDF samples are within the valid range
            (dot(wi_s, -g_frame.SunDir) >= g_frame.SunCosAngularRadius);

        if(trace && (dot(sunSample.f, sunSample.f) > 0))
        {
            bool visible = RtRayQuery::Visibility_Ray(pos, wi_s, normal, g_bvh, surface.Transmissive());
            if(visible)
                target = Light::Le_Sun(pos, g_frame) * sunSample.f;
        }

        const float targetLum = Math::Luminance(target);
#if 0
        const float w_s = targetLum / sunSample.pdf;
#else
        const float w_s = targetLum / 1;
#endif
        r.Update(w_s, wi_s, Light::TYPE::SUN, BSDF::LOBE::ALL, false, /*unused*/ 1, 
            target, rng);
    }
#endif

    // MIS for sky -- take a light sample and a BSDF sample

    // Since sky is low frequency, use cosine-weighted hemisphere sampling as 
    // a proxy for light sampling
    const bool specular = surface.GlossSpecular() && (surface.metallic || surface.specTr) && 
        (!surface.Coated() || surface.CoatSpecular());

    if(!specular)
    {
        const float2 u = rng.Uniform2D();
        float pdf_e;
        float3 wi_e = BSDF::SampleDiffuse(normal, u, pdf_e);
        const float3 le = Light::Le_Sky(wi_e, g_frame.EnvMapDescHeapOffset);
        surface.SetWi(wi_e, normal);

        float3 target = le * BSDF::Unified(surface).f;
        if(dot(target, target) > 0)
            target *= RtRayQuery::Visibility_Ray(pos, wi_e, normal, g_bvh, surface.Transmissive());

        // Balance Heuristic
        const float targetLum = Math::Luminance(target);
        const float pdf_b = BSDF::BSDFSamplerPdf(normal, surface, wi_e, rng);
        const float w_e = RT::BalanceHeuristic(pdf_e, pdf_b, targetLum);

        r.Update(w_e, wi_e, Light::TYPE::SKY, BSDF::LOBE::ALL, false, /*unused*/ 1, 
            target, rng);
    }

    // Sample BSDF
    {
        SkyIncidentRadiance leFunc = SkyIncidentRadiance::Init(g_frame.EnvMapDescHeapOffset);
        BSDF::BSDFSample bsdfSample = BSDF::SampleBSDF(normal, surface, leFunc, rng);

        float3 wi_b = bsdfSample.wi;
        float pdf_b = bsdfSample.pdf;

        float3 target = bsdfSample.f;
        if(dot(target, target) > 0)
            target *= RtRayQuery::Visibility_Ray(pos, wi_b, normal, g_bvh, surface.Transmissive());

        // Balance Heuristic
        const float targetLum = Math::Luminance(target);
        float ndotwi = saturate(dot(wi_b, normal));
        const float pdf_e = ndotwi * ONE_OVER_PI;
        const float w_b = RT::BalanceHeuristic(pdf_b, pdf_e, targetLum);
        // i.e. Glossy reflection or coat with lobe roughness below threshold
        const bool useHalfVecShift = BSDF::LobeAlpha(surface, bsdfSample.lobe) <= g_local.Alpha_min;

        r.Update(w_b, wi_b, surface.wo, normal, Light::TYPE::SKY, bsdfSample.lobe, 
            useHalfVecShift, target, rng);
    }

    float targetLum = Math::Luminance(r.target);
    r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;

    return r;
}

Reservoir EstimateDirectLighting(uint2 DTid, float3 pos, float3 normal, float z_view, 
    float roughness, BSDF::ShadingData surface, inout RNG rng)
{
    Reservoir r = RIS_InitialCandidates(DTid, pos, normal, surface, rng);

    if (IS_CB_FLAG_SET(CB_SKY_DI_FLAGS::TEMPORAL_RESAMPLE)) 
    {
        TemporalCandidate candidate = FindTemporalCandidate(DTid, pos, normal, z_view, 
            roughness, surface, g_frame);

        if(candidate.valid)
        {
            TemporalResample(candidate, pos, normal, surface, 
                g_local.PrevReservoir_A_DescHeapIdx, 
                g_local.PrevReservoir_A_DescHeapIdx + 1,
                g_local.PrevReservoir_A_DescHeapIdx + 2, 
                g_local.Alpha_min, g_bvh, g_frame, r, rng);
        }

        const uint M_max = r.lightType == Light::TYPE::SKY ? g_local.M_max & 0xffff : 
            g_local.M_max >> 16;
        r.Write(DTid, g_local.CurrReservoir_A_DescHeapIdx,
            g_local.CurrReservoir_A_DescHeapIdx + 1, 
            g_local.CurrReservoir_A_DescHeapIdx + 2,
            M_max);

        if(IS_CB_FLAG_SET(CB_SKY_DI_FLAGS::SPATIAL_RESAMPLE))
        {
            SpatialResample(DTid, pos, normal, 
                z_view, roughness, surface, 
                g_local.PrevReservoir_A_DescHeapIdx, 
                g_local.PrevReservoir_A_DescHeapIdx + 1, 
                g_local.PrevReservoir_A_DescHeapIdx + 2, 
                g_local.Alpha_min, g_bvh, g_frame, r, rng);
        }
    }

    return r;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(SKY_DI_GROUP_DIM_X, SKY_DI_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
    uint16_t2 swizzledGid;

    uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint16_t2(SKY_DI_GROUP_DIM_X, SKY_DI_GROUP_DIM_Y),
        g_local.DispatchDimX, 
        SKY_DI_TILE_WIDTH, 
        SKY_DI_LOG2_TILE_WIDTH, 
        g_local.NumGroupsInTile,
        swizzledGid);
#else
    const uint2 swizzledDTid = DTid.xy;
    const uint2 swizzledGid = Gid.xy;
#endif

    if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
        return;

    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 mr = g_metallicRoughness[swizzledDTid];
    GBuffer::Flags flags = GBuffer::DecodeMetallic(mr.x);

    if (flags.invalid || flags.emissive)
        return;

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    const float z_view = g_depth[swizzledDTid];
    
    float2 lensSample = 0;
    float3 origin = g_frame.CameraPos;
    if(g_frame.DoF)
    {
        RNG rngDoF = RNG::Init(RNG::PCG3d(swizzledDTid.xyx).zy, g_frame.FrameNum);
        lensSample = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
        lensSample *= g_frame.LensRadius;
    }

    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float3 pos = Math::WorldPosFromScreenSpace2(swizzledDTid, renderDim, z_view, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrCameraJitter, 
        g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, g_frame.CurrView[2].xyz, 
        g_frame.DoF, lensSample, g_frame.FocusDepth, origin);

    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_normal[swizzledDTid]);

    GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::BASE_COLOR];
    const float4 baseColor = flags.subsurface ? g_baseColor[swizzledDTid] :
        float4(g_baseColor[swizzledDTid].rgb, 0);

    float eta_curr = ETA_AIR;
    float eta_next = DEFAULT_ETA_MAT;

    if(flags.transmissive)
    {
        GBUFFER_IOR g_ior = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::IOR];

        float ior = g_ior[swizzledDTid];
        eta_next = GBuffer::DecodeIOR(ior);
    }

    float coat_weight = 0;
    float3 coat_color = 0.0f;
    float coat_roughness = 0;
    float coat_ior = DEFAULT_ETA_COAT;

    if(flags.coated)
    {
        GBUFFER_COAT g_coat = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::COAT];
        uint3 packed = g_coat[swizzledDTid].xyz;

        GBuffer::Coat coat = GBuffer::UnpackCoat(packed);
        coat_weight = coat.weight;
        coat_color = coat.color;
        coat_roughness = coat.roughness;
        coat_ior = coat.ior;
    }

    const float3 wo = normalize(origin - pos);
    BSDF::ShadingData surface = BSDF::ShadingData::Init(normal, wo, flags.metallic, mr.y, 
        baseColor.xyz, eta_curr, eta_next, flags.transmissive, flags.trDepthGt0, (half)baseColor.w,
        coat_weight, coat_color, coat_roughness, coat_ior);

    RNG rng = RNG::Init(RNG::PCG3d(swizzledDTid.yxx).yz, g_frame.FrameNum);

    SkyDI_Util::Reservoir r = EstimateDirectLighting(swizzledDTid, pos, normal, z_view, 
        mr.y, surface, rng);

    /*
    if(IS_CB_FLAG_SET(CB_SKY_DI_FLAGS::DENOISE))
    {
        // split into diffuse & specular, so they can be denoised separately
        surface.SetWi_Refl(r.wx, normal);
        float3 fr = surface.Fresnel();

        // demodulate base color
        float3 f_d = (1.0f - fr) * (1.0f - flags.metallic) * surface.ndotwi * ONE_OVER_PI;
        
        // demodulate Fresnel for metallic surfaces to preserve texture detail
        float alphaSq = max(1e-5f, surface.alpha * surface.alpha);
        float3 f_s = 0;

        if(flags.metallic)
        {
            if(surface.GlossSpecular())
            {
                f_s = (surface.ndotwh >= MIN_N_DOT_H_SPECULAR);
            }
            else
            {
                float alphaSq = max(1e-5f, surface.alpha * surface.alpha);
                float NDF = BSDF::GGX(surface.ndotwh, alphaSq);
                float G2Div_ndotwi_ndotwo = BSDF::SmithHeightCorrelatedG2_Opt<1>(alphaSq, surface.ndotwi, surface.ndotwo);
                f_s = NDF * G2Div_ndotwi_ndotwo * surface.ndotwi;
            }
        }
        else
            f_s = BSDF::EvalGloss(surface, fr);

        const float3 le = r.lightType == Light::TYPE::SKY ? 
            Light::Le_Sky(r.wx, g_frame.EnvMapDescHeapOffset) :
            Light::Le_Sun(pos, g_frame);

        float3 Li_d = le * f_d * r.W;
        float3 Li_s = le * f_s * r.W;
        float3 wh = normalize(surface.wo + r.wx);
        float whdotwo = saturate(dot(wh, surface.wo));
        float tmp = 1.0f - whdotwo;
        float tmpSq = tmp * tmp;
        uint tmpU = asuint(tmpSq * tmpSq * tmp);
        half2 encoded = half2(asfloat16(uint16_t(tmpU & 0xffff)), asfloat16(uint16_t(tmpU >> 16)));

        RWTexture2D<float4> g_colorA = ResourceDescriptorHeap[g_local.ColorAUavDescHeapIdx];
        RWTexture2D<half4> g_colorB = ResourceDescriptorHeap[g_local.ColorBUavDescHeapIdx];

        g_colorA[swizzledDTid] = float4(Li_s, Li_d.r);
        g_colorB[swizzledDTid] = half4(Li_d.gb, encoded);
    }
    else
    */
    {
        float3 ld = r.target * r.W;
        ld = any(isnan(ld)) ? 0 : ld;
        RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];

        if(g_frame.Accumulate && g_frame.CameraStatic && g_frame.NumFramesCameraStatic > 1)
        {
            float3 prev = g_final[swizzledDTid].rgb;
            g_final[swizzledDTid].rgb = prev + ld;
        }
        else
            g_final[swizzledDTid].rgb = ld;
    }
}