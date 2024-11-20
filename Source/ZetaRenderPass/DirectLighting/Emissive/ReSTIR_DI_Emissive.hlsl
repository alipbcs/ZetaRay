#include "Resampling.hlsli"
#include "../../Common/Common.hlsli"
#include "../../Common/BSDFSampling.hlsli"

#define THREAD_GROUP_SWIZZLING 1

using namespace RtRayQuery;
using namespace RDI_Util;

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

Reservoir RIS_InitialCandidates(uint2 DTid, float3 pos, float3 normal, float roughness,
    BSDF::ShadingData surface, uint sampleSetIdx, int numBsdfCandidates, inout RNG rng, 
    inout Target target)
{
    Reservoir r = Reservoir::Init();
    float target_dwdA = 0.0f;

    // BSDF sampling
    [loop]
    for (int s_b = 0; s_b < numBsdfCandidates; s_b++)
    {
        // TODO sample full BSDF
        BSDF::BSDFSample bsdfSample = BSDF::SampleBSDF_NoDiffuse(normal, surface, rng);
        float3 wi = bsdfSample.wi;
        float3 f = bsdfSample.f;
        float wiPdf = bsdfSample.pdf;

        // check if closest hit is a light source
        BrdfHitInfo hitInfo;
        bool hitEmissive = FindClosestHit(pos, normal, wi, g_bvh, g_frameMeshData, 
            hitInfo, surface.Transmissive());

        RT::EmissiveTriangle emissive;
        float3 le = 0.0;
        float w_i = 0;
        float3 lightNormal = 0.0;
        float dwdA = 0;
        float3 currTarget = 0.0;

        if (hitEmissive)
        {
            emissive = g_emissives[hitInfo.emissiveTriIdx];
            le = Light::Le_EmissiveTriangle(emissive, hitInfo.bary, g_frame.EmissiveMapsDescHeapOffset);

            const float3 vtx1 = Light::DecodeEmissiveTriV1(emissive);
            const float3 vtx2 = Light::DecodeEmissiveTriV2(emissive);
            lightNormal = cross(vtx1 - emissive.Vtx0, vtx2 - emissive.Vtx0);
            float twoArea = length(lightNormal);
            lightNormal = dot(lightNormal, lightNormal) == 0 ? 1.0 : lightNormal / twoArea;
            lightNormal = emissive.IsDoubleSided() && dot(-wi, lightNormal) < 0 ? -lightNormal : lightNormal;

            // skip backfacing lights
            if(dot(-wi, lightNormal) > 0)
            {
                const float lightSourcePdf = g_aliasTable[hitInfo.emissiveTriIdx].CachedP_Orig;
                const float lightPdf = lightSourcePdf * (1.0f / (0.5f * twoArea));

                // solid angle measure to area measure
                dwdA = saturate(dot(lightNormal, -wi)) / (hitInfo.t * hitInfo.t);
                wiPdf *= dwdA;

                // Balance heuristic
                // wiPdf in m_i's numerator and w_i's denominator cancel out
                const float m_i = 1.0f / max(numBsdfCandidates * wiPdf + NUM_LIGHT_CANDIDATES * lightPdf, 1e-6f);

                // target = Le * BSDF(wi, wo) * |ndotwi|
                // source = P(wi)
                currTarget = le * f * dwdA;
                w_i = m_i * Math::Luminance(currTarget);
            }
        }

        // resample
        if (r.Update(w_i, le, hitInfo.emissiveTriIdx, hitInfo.bary, rng))
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
        RT::PresampledEmissiveTriangle tri = Light::SamplePresampledSet(sampleSetIdx, g_sampleSets, 
            g_local.SampleSetSize, rng);

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
        Light::AliasTableSample entry = Light::AliasTableSample::get(g_aliasTable, 
            g_frame.NumEmissiveTriangles, rng);
        RT::EmissiveTriangle tri = g_emissives[entry.idx];
        Light::EmissiveTriSample lightSample = Light::EmissiveTriSample::get(pos, tri, rng);

        float3 le = Light::Le_EmissiveTriangle(tri, lightSample.bary, g_frame.EmissiveMapsDescHeapOffset);
        const float lightPdf = entry.pdf * lightSample.pdf;
        const uint lightID = tri.ID;
        const uint emissiveIdx = entry.idx;
#endif

        float3 currTarget = 0;
        const float t = length(lightSample.pos - pos);
        const float3 wi = (lightSample.pos - pos) / t;
        const float dwdA = saturate(dot(lightSample.normal, -wi)) / (t * t);
        surface.SetWi(wi, normal);

        // skip backfacing lights
        if(dot(lightSample.normal, -wi) > 0)
        {
            currTarget = le * BSDF::Unified(surface).f * dwdA;
                
            if (dot(currTarget, currTarget) > 0)
            {
                currTarget *= Visibility_Segment(pos, wi, t, normal, lightID, 
                    g_bvh, surface.Transmissive());
            }
        }

        // p_d in m_i's numerator and w_i's denominator cancel out
        const float m_i = 1.0f / max(NUM_LIGHT_CANDIDATES * lightPdf + 
            numBsdfCandidates * BSDF::BSDFSamplerPdf_NoDiffuse(normal, surface, wi) * dwdA, 1e-6f);
        const float w_i = m_i * Math::Luminance(currTarget);

        if (r.Update(w_i, le, emissiveIdx, lightSample.bary, rng))
        {
            target.p_hat = currTarget;
            target.rayT = t;
            target.lightID = lightID;
            target.wi = wi;
            target.lightNormal = lightSample.normal;
            target.lightPos = lightSample.pos;
            target.dwdA = dwdA;
        }
    }

    float targetLum = Math::Luminance(target.p_hat);
    r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;

    return r;
}

Reservoir EstimateDirectLighting(uint2 DTid, float3 pos, float3 normal, float z_view, 
    float roughness, uint sampleSetIdx, BSDF::ShadingData surface, inout Target target, 
    RNG rng_group, RNG rng_thread)
{
    // Light sampling is less effective for glossy surfaces or when light source is close to surface
    const int numBsdfCandidates = EXTRA_BSDF_SAMPLE_HIGHLY_GLOSSY && (roughness > 0.06 && roughness < 0.3) ? 
        2 : 
        1;

    Reservoir r = RIS_InitialCandidates(DTid, pos, normal, roughness, surface, sampleSetIdx,
        numBsdfCandidates, rng_thread, target);

    float2 mv = 0;
    bool temporalValid = false;
    TemporalCandidate temporalCandidate;

    if (g_local.TemporalResampling)
    {
        float2 prevUV;

        if(!surface.GlossSpecular() || target.Empty())
        {
            GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
                GBUFFER_OFFSET::MOTION_VECTOR];
            const float2 motionVec = g_motionVector[DTid];
            const float2 currUV = (DTid + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);
            prevUV = currUV - motionVec;
            mv = motionVec;
        }
        else
            prevUV = RDI_Util::VirtualMotionReproject(pos, surface.wo, target.rayT, g_frame.PrevViewProj);

        TemporalCandidate temporalCandidate = FindTemporalCandidate(DTid, pos, 
            normal, z_view, roughness, surface, prevUV, g_frame, rng_thread);
        temporalValid = temporalCandidate.valid;

        if (temporalCandidate.valid)
        {
            temporalCandidate.lightIdx = PartialReadReservoir_ReuseLightIdx(temporalCandidate.posSS, 
                g_local.PrevReservoir_B_DescHeapIdx);

            TemporalResample1(pos, normal, roughness, z_view, surface,
                g_local.PrevReservoir_A_DescHeapIdx, g_local.PrevReservoir_B_DescHeapIdx, 
                temporalCandidate, g_frame, g_emissives, g_bvh, r, rng_thread, target);
        }

        // Note: Results are improved when spatial doesn't feed into next frame's 
        // temporal reservoirs. May need to revisit in the future.
        WriteReservoir(DTid, r, g_local.CurrReservoir_A_DescHeapIdx,
            g_local.CurrReservoir_B_DescHeapIdx, g_local.M_max);
    }

    if(g_local.SpatialResampling)
    {
        bool disoccluded = false;
        if(g_local.ExtraSamplesDisocclusion)
        {
            disoccluded = !temporalValid && (dot(mv, mv) > 0);
            // Skip thin geometry such as grass or fences
            disoccluded = disoccluded && (WaveActiveSum(disoccluded) > 3);
        }

        // Since spatial samples are expensive, take extra samples stochastically
        // per thread group for better coherency
        int numSamples = !g_local.StochasticSpatial || (rng_group.Uniform() < PROB_EXTRA_SPATIAL_SAMPLES) ? 
            MIN_NUM_SPATIAL_SAMPLES + NUM_EXTRA_SPATIAL_SAMPLES : 
            MIN_NUM_SPATIAL_SAMPLES;
        numSamples = !disoccluded ? numSamples : MAX_NUM_SPATIAL_SAMPLES;

        RDI_Util::SpatialResample(DTid, numSamples, SPATIAL_SEARCH_RADIUS, pos, normal, z_view, 
            roughness, surface, g_local.PrevReservoir_A_DescHeapIdx, g_local.PrevReservoir_B_DescHeapIdx, 
            g_frame, g_emissives, g_bvh, r, target, rng_thread);
    }

    return r;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_DI_TEMPORAL_GROUP_DIM_X, RESTIR_DI_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
    uint16_t2 swizzledGid;

    const uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint16_t2(RESTIR_DI_TEMPORAL_GROUP_DIM_X, RESTIR_DI_TEMPORAL_GROUP_DIM_Y),
        g_local.DispatchDimX, 
        RESTIR_DI_TILE_WIDTH, 
        RESTIR_DI_LOG2_TILE_WIDTH, 
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

    if (flags.invalid)
        return;

    if(flags.emissive)
    {
        GBUFFER_EMISSIVE_COLOR g_emissiveColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::EMISSIVE_COLOR];
        float3 le = g_emissiveColor[swizzledDTid].rgb;

        RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];

        if(g_frame.Accumulate && g_frame.CameraStatic)
        {
            float3 prev = g_final[swizzledDTid].rgb;
            g_final[swizzledDTid].rgb = prev + le;
        }
        else
            g_final[swizzledDTid].rgb = le;

        return;
    }

    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_normal[swizzledDTid.xy]);

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

    // Group-uniform index so that every thread in this group uses the same set
    RNG rng_group = RNG::Init(Gid.xy, g_frame.FrameNum);
    const uint sampleSetIdx = rng_group.UniformUintBounded_Faster(g_local.NumSampleSets);

    RNG rng_thread = RNG::Init(swizzledDTid, g_frame.FrameNum);
    Target target = Target::Init();

    Reservoir r = EstimateDirectLighting(swizzledDTid, pos, normal, z_view, mr.y, 
        sampleSetIdx, surface, target, rng_group, rng_thread);

    {
        float3 li = target.p_hat * r.W;
        li = any(isnan(li)) ? 0 : li;
        RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];

        if(g_frame.Accumulate && g_frame.CameraStatic && g_frame.NumFramesCameraStatic > 1)
        {
            float3 prev = g_final[swizzledDTid].rgb;
            g_final[swizzledDTid].rgb = prev + li;
        }
        else
            g_final[swizzledDTid].rgb = li;
    }
}