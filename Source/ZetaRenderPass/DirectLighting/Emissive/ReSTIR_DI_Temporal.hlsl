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
ConstantBuffer<cb_ReSTIR_DI> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);
RaytracingAccelerationStructure g_bvh_prev : register(t1);
StructuredBuffer<RT::EmissiveTriangle> g_emissives : register(t2);
StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable : register(t3);
#ifdef USE_PRESAMPLED_SETS 
StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets : register(t4);
#endif
StructuredBuffer<RT::MeshInstance> g_frameMeshData : register(t5);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

Reservoir RIS_InitialCandidates(uint2 DTid, float3 pos, float3 normal, float roughness,
    BSDF::ShadingData surface, uint sampleSetIdx, int numBsdfSamples, inout RNG rng)
{
    Reservoir r = Reservoir::Init();

    const bool specular = surface.GlossSpecular() && (surface.metallic || surface.specTr) && 
        (!surface.Coated() || surface.CoatSpecular());
    const int numLightSamples = !specular ? NUM_LIGHT_CANDIDATES : 0;

    // BSDF sampling
    [loop]
    for (int s_b = 0; s_b < numBsdfSamples; s_b++)
    {
        BSDF::BSDFSample bsdfSample = BSDF::SampleBSDF_NoDiffuse(normal, surface, rng);
        float3 wi = bsdfSample.wi;
        float pdf_w = bsdfSample.pdf;
#if USE_HALF_VECTOR_COPY_SHIFT == 1
        // i.e. Glossy reflection or coat with lobe roughness below threshold
        const bool useHalfVecShift = BSDF::LobeAlpha(surface, bsdfSample.lobe) <= g_local.Alpha_min;
#else
        const bool useHalfVecShift = false;
#endif

        // check if closest hit is a light source
        BSDFHitInfo hitInfo = FindClosestHit(pos, normal, wi, g_bvh, g_frameMeshData, 
            surface.Transmissive());

        float w_b = 0;
        float3 le = 0;
        float3 lightNormal = 0;
        float3 target = 0;
        uint emissiveID = UINT32_MAX;
        bool doubleSided = false;

        if (hitInfo.hit)
        {
            RT::EmissiveTriangle emissive = g_emissives[hitInfo.emissiveTriIdx];
            le = Light::Le_EmissiveTriangle(emissive, hitInfo.bary, 
                g_frame.EmissiveMapsDescHeapOffset);

            const float3 vtx1 = Light::DecodeEmissiveTriV1(emissive);
            const float3 vtx2 = Light::DecodeEmissiveTriV2(emissive);
            lightNormal = cross(vtx1 - emissive.Vtx0, vtx2 - emissive.Vtx0);
            float twoArea = length(lightNormal);
            lightNormal = dot(lightNormal, lightNormal) == 0 ? 0 : lightNormal / twoArea;
            lightNormal = emissive.IsDoubleSided() && dot(-wi, lightNormal) < 0 ? -lightNormal : lightNormal;
            doubleSided = emissive.IsDoubleSided();
            emissiveID = emissive.ID;

            // Light is backfacing
            if(dot(-wi, lightNormal) > 0)
            {
                const float lightSourcePdf = g_aliasTable[hitInfo.emissiveTriIdx].CachedP_Orig;
                const float pdf_light = lightSourcePdf * (1.0f / (0.5f * twoArea));

                // solid angle measure to area measure
                const float dwdA = saturate(dot(lightNormal, -wi)) / (hitInfo.t * hitInfo.t);
                pdf_w *= dwdA;

                // Balance Heuristic
                const bool sampleIsSpecular = 
                    (surface.GlossSpecular() && bsdfSample.lobe == BSDF::LOBE::GLOSSY_R) || 
                    (surface.CoatSpecular() && bsdfSample.lobe == BSDF::LOBE::COAT);
                float denom = numBsdfSamples * pdf_w + !sampleIsSpecular * numLightSamples * pdf_light;
                // pdf_a in m_i's numerator and w_i's denominator cancel out
                const float m_i = 1.0f / denom; 

                // target = Le * BSDF(wi, wo) * |ndotwi|
                // source = P(wi)
                target = le * bsdfSample.f * dwdA;
                w_b = m_i * Math::Luminance(target);
            }
        }

        if (r.Update(w_b, useHalfVecShift, wi, surface.wo, normal, bsdfSample.lobe, le, 
            hitInfo.emissiveTriIdx, hitInfo.bary, rng))
        {
            r.target = target;
            r.lightID = emissiveID;
            r.lightPos = hitInfo.lightPos;
            r.lightNormal = lightNormal;
            r.doubleSided = doubleSided;
        }
    }

    // light sampling
    [loop]
    for (int s_l = 0; s_l < numLightSamples; s_l++)
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
        const float pdf_light = tri.pdf;
        const uint emissiveIdx = tri.idx;
        const uint lightID = tri.ID;
        const bool doubleSided = tri.twoSided;

        if(tri.twoSided && dot(pos - tri.pos, lightSample.normal) < 0)
            lightSample.normal = -lightSample.normal;
#else
        Light::AliasTableSample entry = Light::AliasTableSample::get(g_aliasTable, 
            g_frame.NumEmissiveTriangles, rng);
        RT::EmissiveTriangle tri = g_emissives[entry.idx];
        Light::EmissiveTriSample lightSample = Light::EmissiveTriSample::get(pos, tri, rng);

        float3 le = Light::Le_EmissiveTriangle(tri, lightSample.bary, g_frame.EmissiveMapsDescHeapOffset);
        const float pdf_light = entry.pdf * lightSample.pdf;
        const uint emissiveIdx = entry.idx;
        const uint lightID = tri.ID;
        const bool doubleSided = tri.IsDoubleSided();
#endif

        float3 target = 0;
        float3 wi = lightSample.pos - pos;
        const bool isZero = dot(wi, wi) == 0;
        const float t = isZero ? 0 : length(wi);
        wi = isZero ? wi : wi / t;
        const float dwdA = isZero ? 0 : saturate(dot(lightSample.normal, -wi)) / (t * t);
        surface.SetWi(wi, normal);

        // skip backfacing lights
        if(dot(lightSample.normal, -wi) > 0)
        {
            target = le * BSDF::Unified(surface).f * dwdA;
            if (dot(target, target) > 0)
            {
                target *= Visibility_Segment(pos, wi, t, normal, lightID, g_bvh, 
                    surface.Transmissive());
            }
        }

        // pdf_light in m_i's numerator and w_i's denominator cancel out
        const float denom = numLightSamples * pdf_light + 
            numBsdfSamples * BSDF::BSDFSamplerPdf_NoDiffuse(normal, surface, wi) * dwdA;
        const float m_l = denom > 0 ? 1.0f / denom : 0;
        const float w_l = m_l * Math::Luminance(target);

        if (r.Update(w_l, le, emissiveIdx, lightSample.bary, rng))
        {
            r.target = target;
            r.lightID = lightID;
            r.lightNormal = lightSample.normal;
            r.lightPos = lightSample.pos;
            r.doubleSided = doubleSided;
        }
    }

    float targetLum = Math::Luminance(r.target);
    r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;

    return r;
}

Reservoir EstimateDirectLighting(uint2 DTid, float3 pos, float3 normal, float z_view, 
    float roughness, uint sampleSetIdx, BSDF::ShadingData surface, RNG rng_group,
    RNG rng_thread)
{
    // Light sampling is less effective for glossy surfaces or when light source is close to surface
    const int numBsdfSamples = EXTRA_BSDF_SAMPLE_HIGHLY_GLOSSY && !surface.GlossSpecular() && roughness < 0.3 ? 
        2 : 
        1;

    Reservoir r = RIS_InitialCandidates(DTid, pos, normal, roughness, surface, sampleSetIdx,
        numBsdfSamples, rng_thread);

    if (IS_CB_FLAG_SET(CB_RDI_FLAGS::TEMPORAL_RESAMPLE)) 
    {
        GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::MOTION_VECTOR];
        float2 motionVec = g_motionVector[DTid];
        const float2 currUV = (DTid + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);
        float2 prevUV = currUV - motionVec;

        TemporalCandidate temporalCandidate = FindTemporalCandidate(DTid, pos, 
            normal, z_view, roughness, surface, prevUV, g_frame, rng_thread);

        if (temporalCandidate.valid)
        {
            TemporalResample1(pos, normal, surface, g_local.Alpha_min, 
                temporalCandidate, g_local.PrevReservoir_A_DescHeapIdx, 
                g_local.PrevReservoir_A_DescHeapIdx + 1, g_frame, 
                g_bvh, g_bvh_prev, g_emissives, g_frameMeshData,
                r, rng_thread);
        }

        if(IS_CB_FLAG_SET(CB_RDI_FLAGS::SPATIAL_RESAMPLE))
        {
            bool disoccluded = !temporalCandidate.valid && (dot(motionVec, motionVec) > 0);
            r.target = disoccluded ? -r.target : r.target;
            r.WriteTarget(DTid, g_local.TargetDescHeapIdx);
        }
    }

    if (IS_CB_FLAG_SET(CB_RDI_FLAGS::TEMPORAL_RESAMPLE) ||
        IS_CB_FLAG_SET(CB_RDI_FLAGS::RESET_TEMPORAL_TEXTURES)) 
    {
        // Note: Results are improved when spatial doesn't feed into next frame's 
        // temporal reservoirs. May need to revisit in the future.
        r.Write(DTid, g_local.CurrReservoir_A_DescHeapIdx,
            g_local.CurrReservoir_A_DescHeapIdx + 1, (uint16)g_local.M_max);
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

    RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];

    if (flags.invalid)
    {
        if(g_frame.Accumulate && g_frame.CameraStatic)
        {
            float3 prev = g_final[swizzledDTid].rgb;
            g_final[swizzledDTid].xyz = prev * (g_frame.NumFramesCameraStatic > 1) + 
                Light::Le_SkyWithSunDisk(swizzledDTid, g_frame);
        }
        else
            g_final[swizzledDTid].xyz = 0;

        return;
    }

    if(flags.emissive && !IS_CB_FLAG_SET(CB_RDI_FLAGS::SPATIAL_RESAMPLE))
    {
        GBUFFER_EMISSIVE_COLOR g_emissiveColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::EMISSIVE_COLOR];
        float3 le = g_emissiveColor[swizzledDTid].rgb;

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

    Reservoir r = EstimateDirectLighting(swizzledDTid, pos, normal, z_view, mr.y, 
        sampleSetIdx, surface, rng_group, rng_thread);

    if(!IS_CB_FLAG_SET(CB_RDI_FLAGS::SPATIAL_RESAMPLE) || !IS_CB_FLAG_SET(CB_RDI_FLAGS::TEMPORAL_RESAMPLE))
    {
        float3 li = r.target * r.W;
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