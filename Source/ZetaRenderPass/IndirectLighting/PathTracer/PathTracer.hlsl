#ifndef NEE_EMISSIVE
#define NEE_EMISSIVE 0
#endif

#include "Params.hlsli"
#include "../ReSTIR_GI/PathTracing.hlsli"
#include "../../Common/Common.hlsli"

#define THREAD_GROUP_SWIZZLING 1

using namespace RtRayQuery;

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_ReSTIR_GI> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);
StructuredBuffer<RT::MeshInstance> g_frameMeshData : register(t1);
StructuredBuffer<Vertex> g_vertices : register(t2);
StructuredBuffer<uint> g_indices : register(t3);
StructuredBuffer<Material> g_materials : register(t4);
#if NEE_EMISSIVE == 1
StructuredBuffer<RT::EmissiveTriangle> g_emissives : register(t5);
StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets : register(t6);
StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable : register(t7);
#endif

//--------------------------------------------------------------------------------------
// Utility Functions
//--------------------------------------------------------------------------------------

ReSTIR_Util::Globals InitGlobals(bool transmissive)
{
    ReSTIR_Util::Globals globals;
    globals.bvh = g_bvh;
    globals.frameMeshData = g_frameMeshData;
    globals.vertices = g_vertices;
    globals.indices = g_indices;
    globals.materials = g_materials;
    globals.maxNumBounces = transmissive ? (uint16_t)g_local.MaxGlossyTrBounces :
        (uint16_t)g_local.MaxNonTrBounces;
    globals.russianRoulette = IS_CB_FLAG_SET(CB_IND_FLAGS::RUSSIAN_ROULETTE);

#if NEE_EMISSIVE == 1
    globals.emissives = g_emissives;
    globals.sampleSets = g_sampleSets;
    globals.aliasTable = g_aliasTable;
    globals.sampleSetSize = (uint16_t)(g_local.SampleSetSize_NumSampleSets & 0xffff);
#endif

    return globals;
}

float3 EstimateIndirectLighting(uint2 DTid, float3 origin, float2 lensSample, float3 pos, 
    float3 normal, float ior, BSDF::ShadingData surface, ReSTIR_Util::Globals globals, 
    inout RNG rngThread, inout RNG rngGroup)
{
    // Use the same sample set for all the threads in this group
    const uint sampleSetIdx = rngGroup.UniformUintBounded_Faster(g_local.SampleSetSize_NumSampleSets >> 16);

    BSDF::BSDFSample bsdfSample = BSDF::SampleBSDF(normal, surface, rngThread);
    if(bsdfSample.pdf == 0)
        return 0;

    Hit hitInfo = Hit::FindClosest<true, true>(pos, normal, bsdfSample.wi, globals.bvh, 
        globals.frameMeshData, globals.vertices, globals.indices, 
        surface.Transmissive());

    if(!hitInfo.hit)
        return 0;

    GBUFFER_TRI_DIFF_GEO_A g_triA = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::TRI_DIFF_GEO_A];
    GBUFFER_TRI_DIFF_GEO_B g_triB = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::TRI_DIFF_GEO_B];
    const uint4 packed_a = g_triA[DTid];
    const uint2 packed_b = g_triB[DTid];

    Math::TriDifferentials triDiffs = Math::TriDifferentials::Unpack(packed_a, packed_b);
    float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

    RT::RayDifferentials rd = RT::RayDifferentials::Init(DTid, renderDim, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrCameraJitter, 
        g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, 
        g_frame.CurrView[2].xyz, g_frame.DoF, g_frame.FocusDepth, 
        lensSample, origin);

    float3 dpdx;
    float3 dpdy;
    rd.dpdx_dpdy(pos, normal, dpdx, dpdy);
    rd.ComputeUVDifferentials(dpdx, dpdy, triDiffs.dpdu, triDiffs.dpdv);

    rd.UpdateRays(pos, normal, bsdfSample.wi, surface.wo, triDiffs, 
        dpdx, dpdy, dot(bsdfSample.wi, normal) < 0, surface.eta);

    SamplerState samp = SamplerDescriptorHeap[g_local.TexFilterDescHeapIdx];
    float3 li = ReSTIR_RT::PathTrace(pos, normal, ior, sampleSetIdx, bsdfSample, 
        hitInfo, rd, g_frame, globals, samp, rngThread, rngGroup);

    if(dot(li, li) > 0)
    {
        surface.SetWi(bsdfSample.wi, normal);
        li *= bsdfSample.bsdfOverPdf;
    }

    return li;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_GI_TEMPORAL_GROUP_DIM_X, RESTIR_GI_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING == 1
    uint16_t2 swizzledGid;

    uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint16_t2(RESTIR_GI_TEMPORAL_GROUP_DIM_X, RESTIR_GI_TEMPORAL_GROUP_DIM_Y),
        uint16_t(g_local.DispatchDimX_NumGroupsInTile & 0xffff), 
        RESTIR_GI_TEMPORAL_TILE_WIDTH, 
        RESTIR_GI_TEMPORAL_LOG2_TILE_WIDTH, 
        uint16_t(g_local.DispatchDimX_NumGroupsInTile >> 16),
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
    const float3 normal = Math::DecodeUnitVector(g_normal[swizzledDTid.xy]);

    GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::BASE_COLOR];
    const float3 baseColor = g_baseColor[swizzledDTid].rgb;

    float eta_curr = ETA_AIR;
    float eta_next = DEFAULT_ETA_MAT;

    if(flags.transmissive)
    {
        GBUFFER_IOR g_ior = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::IOR];

        float ior = g_ior[swizzledDTid];
        eta_next = GBuffer::DecodeIOR(ior);
    }

    const float3 wo = normalize(origin - pos);
    BSDF::ShadingData surface = BSDF::ShadingData::Init(normal, wo, flags.metallic, mr.y, baseColor, 
        eta_curr, eta_next, flags.transmissive, flags.trDepthGt0);

    // Per-group RNG
    RNG rngGroup = RNG::Init(swizzledGid ^ 61, g_frame.FrameNum);
    // Per-thread RNG
    RNG rngThread = RNG::Init(uint2(swizzledDTid.x ^ 511, swizzledDTid.y ^ 31), g_frame.FrameNum);

    ReSTIR_Util::Globals globals = InitGlobals(flags.transmissive);

    float3 li = EstimateIndirectLighting(swizzledDTid, origin, lensSample, pos, 
        normal, eta_next, surface, globals, rngThread, rngGroup);
    li = any(isnan(li)) ? 0 : li;

    RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];

    if(g_frame.Accumulate && g_frame.CameraStatic)
    {
        float3 prev = g_final[swizzledDTid].rgb;
        g_final[swizzledDTid].rgb = prev + li;
    }
    else
        g_final[swizzledDTid].rgb = li;
}
