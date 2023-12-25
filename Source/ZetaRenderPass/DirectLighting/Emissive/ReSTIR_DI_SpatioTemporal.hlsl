#include "ReSTIR_DI.hlsli"
#include "ReSTIR_DI_Resampling.hlsli"
#include "../../Common/Common.hlsli"
#include "../../Common/BRDF.hlsli"
#include "../../Common/LightSource.hlsli"

#define THREAD_GROUP_SWIZZLING 1

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_ReSTIR_DI_SpatioTemporal> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);
StructuredBuffer<RT::EmissiveTriangle> g_emissives : register(t1);
StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable : register(t2);
#ifdef USE_PRESAMPLED_SETS 
StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets : register(t3);
#endif
StructuredBuffer<RT::MeshInstance> g_frameMeshData : register(t4);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

RDI_Util::Reservoir RIS_InitialCandidates(uint2 DTid, float3 posW, float3 normal, float roughness,
    BRDF::ShadingData surface, uint sampleSetIdx, int numBrdfCandidates, inout RNG rng, 
    inout RDI_Util::Target target)
{
    RDI_Util::Reservoir r = RDI_Util::Reservoir::Init();
    target.needsShadowRay = false;
    float target_dwdA = 0.0f;

    // brdf sampling
    for (int s_b = 0; s_b < numBrdfCandidates; s_b++)
    {
        const float3 wi = BRDF::SampleSpecularMicrofacet(surface, normal, rng.Uniform2D());

        // check if closest hit is a light source
        RDI_Util::BrdfHitInfo hitInfo;
        bool hitEmissive = RDI_Util::FindClosestHit(posW, normal, wi, g_bvh, g_frameMeshData, hitInfo);

        RT::EmissiveTriangle emissive;
        float3 L_e = 0.0.xxx;
        float w_i = 0;
        float3 lightNormal = 0.0.xxx;
        float dwdA = 0;
        float3 currTarget = 0.0.xxx;

        if (hitEmissive)
        {
            emissive = g_emissives[hitInfo.emissiveTriIdx];
            L_e = Light::Le_EmissiveTriangle(emissive, hitInfo.bary, g_frame.EmissiveMapsDescHeapOffset);

            const float3 vtx1 = Light::DecodeEmissiveTriV1(emissive);
            const float3 vtx2 = Light::DecodeEmissiveTriV2(emissive);
            lightNormal = cross(vtx1 - emissive.Vtx0, vtx2 - emissive.Vtx0);
            float twoArea = length(lightNormal);
            twoArea = max(twoArea, 1e-6);
            lightNormal = all(lightNormal == 0) ? 1.0.xxx : lightNormal / twoArea;
            lightNormal = emissive.IsDoubleSided() && dot(-wi, lightNormal) < 0 ? lightNormal * -1.0f : lightNormal;

            // skip backfacing lights
            if(dot(-wi, lightNormal) > 0)
            {
                const float lightSourcePdf = g_aliasTable[hitInfo.emissiveTriIdx].CachedP_Orig;
                const float lightPdf = lightSourcePdf * (1.0f / (0.5f * twoArea));

                surface.SetWi(wi, normal);
                float wiPdf = BRDF::GGXVNDFReflectionPdf(surface);

                // solid angle measure to area measure
                dwdA = saturate(dot(lightNormal, -wi)) / max(hitInfo.t * hitInfo.t, 1e-6f);
                wiPdf *= dwdA;

                // balance heuristic
                // wiPdf in m_i's numerator and w_i's denominator cancel out
                const float m_i = 1.0f / max(numBrdfCandidates * wiPdf + NUM_LIGHT_CANDIDATES * lightPdf, 1e-6f);

                // target = Le * BRDF(wi, wo) * |ndotwi|
                // source = P(wi)
                currTarget = L_e * BRDF::CombinedBRDF(surface) * dwdA;
                w_i = m_i * Math::Luminance(currTarget);
            }
        }

        // resample    
        if (r.Update(w_i, L_e, hitInfo.emissiveTriIdx, hitInfo.bary, rng))
        {
            target.p_hat = currTarget;
            target.rayT = hitInfo.t;
            target.lightID = emissive.ID;
            target.wi = wi;
            target.lightPos = hitInfo.lightPos;    
            target.lightNormal = lightNormal;    
            target.dwdA = dwdA;
        }
    }

    // light sampling
    [loop]
    for (int s_l = 0; s_l < NUM_LIGHT_CANDIDATES; s_l++)
    {
        // sample a light source relative to its power
#ifdef USE_PRESAMPLED_SETS
        RT::PresampledEmissiveTriangle tri = Light::UnformSampleSampleSet(sampleSetIdx, g_sampleSets, 
            g_local.SampleSetSize, rng);

        Light::EmissiveTriAreaSample lightSample;
        lightSample.pos = tri.pos;
        lightSample.normal = Math::DecodeOct16(tri.normal);
        lightSample.bary = Math::DecodeUNorm2(tri.bary);

        float3 Le = tri.le;
        const float lightPdf = tri.pdf;
        const uint emissiveIdx = tri.idx;
        const uint lightID = tri.ID;

        if(tri.twoSided && dot(posW - tri.pos, lightSample.normal) < 0)
            lightSample.normal *= -1;
#else
        float lightSourcePdf;
        const uint emissiveIdx = Light::SampleAliasTable(g_aliasTable, g_frame.NumEmissiveTriangles, rng, lightSourcePdf);
        RT::EmissiveTriangle emissive = g_emissives[emissiveIdx];

        const Light::EmissiveTriAreaSample lightSample = Light::SampleEmissiveTriangleSurface(posW, emissive, rng);

        float3 Le = Light::Le_EmissiveTriangle(emissive, lightSample.bary, g_frame.EmissiveMapsDescHeapOffset);
        const float lightPdf = lightSourcePdf * lightSample.pdf;
        uint lightID = emissive.ID;
#endif

        float3 currTarget = 0;

        const float t = length(lightSample.pos - posW);
        const float3 wi = (lightSample.pos - posW) / t;
        const float dwdA = saturate(dot(lightSample.normal, -wi)) / (t * t);

        // skip backfacing lights
        if(dot(lightSample.normal, -wi) > 0)
        {
            surface.SetWi(wi, normal);
            currTarget = Le * BRDF::CombinedBRDF(surface) * dwdA;
                
#if TARGET_WITH_VISIBILITY == 1
            if (Math::Luminance(currTarget) > 1e-6)
                currTarget *= RDI_Util::VisibilityApproximate(g_bvh, posW, wi, t, normal, lightID);
#endif
        }

        // p_d in m_i's numerator and w_i's denominator cancel out
        const float m_i = 1.0f / max(NUM_LIGHT_CANDIDATES * lightPdf + numBrdfCandidates * BRDF::GGXVNDFReflectionPdf(surface) * dwdA, 1e-6f);
        const float w_i = m_i * Math::Luminance(currTarget);

        if (r.Update(w_i, Le, emissiveIdx, lightSample.bary, rng))
        {
            target.p_hat = currTarget;
            target.rayT = t;
            target.lightID = lightID;
            target.wi = wi;
            target.lightNormal = lightSample.normal;
            target.lightPos = lightSample.pos;
            target.dwdA = dwdA;
            target.needsShadowRay = 1 - TARGET_WITH_VISIBILITY;
        }
    }

    float targetLum = Math::Luminance(target.p_hat);
    r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;

#if TARGET_WITH_VISIBILITY == 0
    if (target.needsShadowRay)
    {
        if (!RDI_Util::VisibilityApproximate(g_bvh, posW, target.wi, target.rayT, normal, target.lightID))
        {
            target.visible = false; 
            r.W = 0;
        }
    }
#endif

    target.needsShadowRay = false;

    return r;
}

RDI_Util::Reservoir EstimateDirectLighting(uint2 DTid, float3 posW, float3 normal, float linearDepth, float roughness, 
    float3 baseColor, uint sampleSetIdx, BRDF::ShadingData surface, float2 prevUV, inout RDI_Util::Target target, 
    inout RNG rng)
{
    // light sampling is less effective for glossy surfaces or when light source is close to surface
    const uint numBrdfCandidates = roughness > 0.06 && roughness < g_local.MaxRoughnessExtraBrdfSampling ? MAX_NUM_BRDF_SAMPLES : 1;

    // initial candidates
    RDI_Util::Reservoir r = RIS_InitialCandidates(DTid, posW, normal, roughness, surface, sampleSetIdx,
        numBrdfCandidates, rng, target);

    // temporal resampling
    if (g_local.TemporalResampling)
    {
        RDI_Util::TemporalCandidate temporalCandidate = RDI_Util::FindTemporalCandidates(DTid, posW, normal, 
            linearDepth, roughness, prevUV, g_frame, rng);

        if (temporalCandidate.valid)
        {
            temporalCandidate.lightIdx = RDI_Util::PartialReadReservoir_ReuseLightIdx(temporalCandidate.posSS, 
                g_local.PrevReservoir_B_DescHeapIdx);

            RDI_Util::TemporalResample1(posW, normal, roughness, linearDepth, surface,
                g_local.PrevReservoir_A_DescHeapIdx, g_local.PrevReservoir_B_DescHeapIdx, temporalCandidate,
                g_frame, g_emissives, g_bvh, r, rng, target);
        }
    }

    RDI_Util::WriteReservoir(DTid, r, g_local.CurrReservoir_A_DescHeapIdx,
        g_local.CurrReservoir_B_DescHeapIdx, g_local.M_max);

    // spatial resampling
    if(g_local.SpatialResampling)
    {
#if TARGET_WITH_VISIBILITY == 1
        // spatial resampling is really expensive -- use a heuristic to decide when extra samples 
        // have a noticeable impact
        bool extraSample = normal.y < -0.1 && 
            roughness > 0.075 && 
            Math::Luminance(baseColor) > 0.5f;

        int numSamples = MIN_NUM_SPATIAL_SAMPLES + extraSample;
#else
        int numSamples = MIN_NUM_SPATIAL_SAMPLES + 1;
#endif    
        
        RDI_Util::SpatialResample(DTid, numSamples, SPATIAL_SEARCH_RADIUS, posW, normal, linearDepth, 
            roughness, surface, g_local.PrevReservoir_A_DescHeapIdx, g_local.PrevReservoir_B_DescHeapIdx, 
            g_frame, g_emissives, g_bvh, r, target, rng);
    }

#if TARGET_WITH_VISIBILITY == 0
    if (target.needsShadowRay)
        target.visible = RDI_Util::VisibilityApproximate(g_bvh, posW, target.wi, target.rayT, normal, target.lightID);
#endif

    return r;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_DI_TEMPORAL_GROUP_DIM_X, RESTIR_DI_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
    uint16_t2 swizzledGid;

    // swizzle thread groups for better L2-cache behavior
    // Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
    uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint16_t2(RESTIR_DI_TEMPORAL_GROUP_DIM_X, RESTIR_DI_TEMPORAL_GROUP_DIM_Y),
        g_local.DispatchDimX, 
        RESTIR_DI_TEMPORAL_TILE_WIDTH, 
        RESTIR_DI_TEMPORAL_LOG2_TILE_WIDTH, 
        g_local.NumGroupsInTile,
        swizzledGid);
#else
    const uint2 swizzledDTid = DTid.xy;
    const uint2 swizzledGid = Gid.xy;
#endif

    if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
        return;

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
    const float linearDepth = g_depth[swizzledDTid];
    
    if (linearDepth == FLT_MAX)
        return;

    // reconstruct position from depth buffer
    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float3 posW = Math::WorldPosFromScreenSpace(swizzledDTid,
        renderDim,
        linearDepth,
        g_frame.TanHalfFOV,
        g_frame.AspectRatio,
        g_frame.CurrViewInv,
        g_frame.CurrCameraJitter);

    // shading normal
    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_normal[swizzledDTid.xy]);

    // roughness and metallic mask
    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 mr = g_metallicRoughness[swizzledDTid];
    bool isMetallic;
    bool isEmissive;
    GBuffer::DecodeMetallicEmissive(mr.x, isMetallic, isEmissive);

    if (isEmissive)
        return;

    GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::BASE_COLOR];
    const float3 baseColor = g_baseColor[swizzledDTid].rgb;

    const float3 wo = normalize(g_frame.CameraPos - posW);
    BRDF::ShadingData surface = BRDF::ShadingData::Init(normal, wo, isMetallic, mr.y, baseColor);

    // get a unique per-group index
    RNG rng = RNG::Init(Gid.xy, g_frame.FrameNum);
    const uint sampleSetIdx = rng.UniformUintBounded_Faster(g_local.NumSampleSets);

    // reverse reproject current pixel
    GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::MOTION_VECTOR];
    const float2 motionVec = g_motionVector[swizzledDTid.xy];
    const float2 currUV = (swizzledDTid + 0.5f) / renderDim;
    const float2 prevUV = currUV - motionVec;

    rng = RNG::Init(swizzledDTid, g_frame.FrameNum);
    RDI_Util::Target target = RDI_Util::Target::Init();

    RDI_Util::Reservoir r = EstimateDirectLighting(swizzledDTid, posW, normal, linearDepth, mr.y, baseColor, 
        sampleSetIdx, surface, prevUV, target, rng);

    if(g_local.Denoise)
    {
        // split into diffuse & specular, so they can be denoised seperately
        surface.SetWi(target.wi, normal);

        // demodulate base color
        float3 f_d = (1.0f - surface.F) * (1.0f - isMetallic) * surface.ndotwi * ONE_OVER_PI;
        float3 f_s = 0;

        // demodulate Fresnel for metallic surfaces to preserve texture detail
        float alphaSq = max(1e-5f, surface.alpha * surface.alpha);
        if(isMetallic)
        {
            if(surface.deltaNDF)
            {
                // Divide by ndotwi is so that integrating brdf over hemisphere would give F (Frensel).
                f_s = (surface.ndotwh >= MIN_N_DOT_H_PERFECT_SPECULAR) / surface.ndotwi;
            }
            else
            {
                float alphaSq = max(1e-5f, surface.alpha * surface.alpha);
                float NDF = BRDF::GGX(surface.ndotwh, alphaSq);
                float G2Div_ndotwi_ndotwo = BRDF::SmithHeightCorrelatedG2ForGGX(alphaSq, surface.ndotwi, surface.ndotwo);
                f_s = NDF * G2Div_ndotwi_ndotwo * surface.ndotwi;
            }
        }
        else
            f_s = BRDF::SpecularBRDFGGXSmith(surface);

        float3 Li_d = r.Le * target.dwdA * f_d * target.visible * r.W;
        float3 Li_s = r.Le * target.dwdA * f_s * target.visible * r.W;
        float3 wh = normalize(surface.wo + target.wi);
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
    {
        float3 Li = target.p_hat * target.visible * r.W;
        RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];

        if(g_frame.Accumulate && g_frame.CameraStatic)
        {
            float3 prev = g_final[swizzledDTid].rgb;
            g_final[swizzledDTid].rgb = prev + Li;
        }
        else
            g_final[swizzledDTid].rgb = Li;
    }
}