#ifndef NEE_EMISSIVE
#define NEE_EMISSIVE 0
#endif

#include "../../Common/Common.hlsli"
#include "Util.hlsli"

#if !defined (TEMPORAL_TO_CURRENT) && !defined (SPATIAL_TO_CURRENT) && !defined (CURRENT_TO_SPATIAL)
#define CURRENT_TO_TEMPORAL
#endif

#define THREAD_GROUP_SWIZZLING 0

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_ReSTIR_PT_Reuse> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);
StructuredBuffer<RT::MeshInstance> g_frameMeshData : register(t1);
StructuredBuffer<Vertex> g_vertices : register(t2);
StructuredBuffer<uint> g_indices : register(t3);
StructuredBuffer<Material> g_materials : register(t4);

//--------------------------------------------------------------------------------------
// Utility Functions
//--------------------------------------------------------------------------------------

ReSTIR_Util::Globals InitGlobals()
{
    ReSTIR_Util::Globals globals;
    globals.bvh = g_bvh;
    globals.frameMeshData = g_frameMeshData;
    globals.vertices = g_vertices;
    globals.indices = g_indices;
    globals.materials = g_materials;
    globals.maxDiffuseBounces = (uint16_t)(g_local.Packed & 0xf);
    globals.maxGlossyBounces_NonTr = (uint16_t)((g_local.Packed >> 4) & 0xf);
    globals.maxGlossyBounces_Tr = (uint16_t)((g_local.Packed >> 8) & 0xf);
    globals.maxNumBounces = (uint16_t)max(globals.maxDiffuseBounces, 
        max(globals.maxGlossyBounces_NonTr, globals.maxGlossyBounces_Tr));

    return globals;
}

RPT_Util::PathContext ReplayCurrentInTemporalDomain(uint2 prevPosSS, float3 origin, 
    float2 lensSample, float3 prevPos, bool prevMetallic, float prevRoughness, 
    bool prevTransmissive, bool prevTrDepthGt0, RPT_Util::Reconnection rc_curr, 
    ReSTIR_Util::Globals globals)
{
    float eta_t = DEFAULT_ETA_T;
    float eta_i = DEFAULT_ETA_I;

    if(prevTransmissive)
    {
        GBUFFER_IOR g_ior = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::IOR];

        float ior = g_ior[prevPosSS];
        eta_i = GBuffer::DecodeIOR(ior);
    }

    GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_prevNormal[prevPosSS]);

    GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
        GBUFFER_OFFSET::BASE_COLOR];
    const float3 baseColor = g_prevBaseColor[prevPosSS].rgb;

    const float3 wo = normalize(origin - prevPos);
    BSDF::ShadingData surface = BSDF::ShadingData::Init(normal, wo, prevMetallic, prevRoughness, 
        baseColor, eta_i, eta_t, prevTransmissive, prevTrDepthGt0);

    GBUFFER_TRI_DIFF_GEO_A g_triA = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
        GBUFFER_OFFSET::TRI_DIFF_GEO_A];
    GBUFFER_TRI_DIFF_GEO_B g_triB = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
        GBUFFER_OFFSET::TRI_DIFF_GEO_B];
    const uint4 packed_a = g_triA[prevPosSS];
    const uint2 packed_b = g_triB[prevPosSS];

    Math::TriDifferentials triDiffs = Math::TriDifferentials::Unpack(packed_a, packed_b);
    float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

    RT::RayDifferentials rd = RT::RayDifferentials::Init(prevPosSS, renderDim, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevCameraJitter, 
        g_frame.PrevView[0].xyz, g_frame.PrevView[1].xyz, 
        g_frame.PrevView[2].xyz, g_frame.DoF, g_frame.FocusDepth, 
        lensSample, origin);

    float3 dpdx;
    float3 dpdy;
    rd.dpdx_dpdy(prevPos, normal, dpdx, dpdy);
    rd.ComputeUVDifferentials(dpdx, dpdy, triDiffs.dpdu, triDiffs.dpdv);

    const bool regularization = IS_CB_FLAG_SET(CB_IND_FLAGS::PATH_REGULARIZATION);
    SamplerState samp = SamplerDescriptorHeap[g_local.TexFilterDescHeapIdx];

    return Replay_kGt2(prevPos, normal, eta_i, surface, rd, triDiffs, rc_curr, 
        g_local.Alpha_min, regularization, samp, g_frame, globals);
}

RPT_Util::PathContext ReplayCurrentInSpatialDomain(uint2 samplePosSS, RPT_Util::Reconnection rc_curr,
    ReSTIR_Util::Globals globals)
{
    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    GBUFFER_METALLIC_ROUGHNESS g_mr = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::BASE_COLOR];

    const float depth_n = g_depth[samplePosSS];

    float2 lensSample_n = 0;
    float3 origin_n = g_frame.CameraPos;
    if(g_frame.DoF)
    {
        RNG rngDoF = RNG::Init(RNG::PCG3d(samplePosSS.xyx).zy, g_frame.FrameNum);
        lensSample_n = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
        lensSample_n *= g_frame.LensRadius;
    }

    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float3 pos_n = Math::WorldPosFromScreenSpace2(samplePosSS, renderDim, depth_n, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrCameraJitter, 
        g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, g_frame.CurrView[2].xyz, 
        g_frame.DoF, lensSample_n, g_frame.FocusDepth, origin_n);

    const float2 mr_n = g_mr[samplePosSS];
    GBuffer::Flags flags_n = GBuffer::DecodeMetallic(mr_n.x);

    float3 normal_n = Math::DecodeUnitVector(g_normal[samplePosSS]);
    float3 baseColor_n = g_baseColor[samplePosSS].rgb;
    float eta_i = DEFAULT_ETA_I;

    if(flags_n.transmissive)
    {
        GBUFFER_IOR g_ior = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::IOR];

        float ior = g_ior[samplePosSS];
        eta_i = GBuffer::DecodeIOR(ior);
    }

    const float3 wo_n = normalize(origin_n - pos_n);
    BSDF::ShadingData surface_n = BSDF::ShadingData::Init(normal_n, wo_n, flags_n.metallic, 
        mr_n.y, baseColor_n, eta_i, DEFAULT_ETA_T, flags_n.transmissive, flags_n.trDepthGt0);

    GBUFFER_TRI_DIFF_GEO_A g_triA = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::TRI_DIFF_GEO_A];
    GBUFFER_TRI_DIFF_GEO_B g_triB = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::TRI_DIFF_GEO_B];
    const uint4 packed_a = g_triA[samplePosSS];
    const uint2 packed_b = g_triB[samplePosSS];

    Math::TriDifferentials triDiffs = Math::TriDifferentials::Unpack(packed_a, packed_b);

    RT::RayDifferentials rd = RT::RayDifferentials::Init(samplePosSS, renderDim, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrCameraJitter, 
        g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, 
        g_frame.CurrView[2].xyz, g_frame.DoF, g_frame.FocusDepth, 
        lensSample_n, origin_n);

    bool regularization = IS_CB_FLAG_SET(CB_IND_FLAGS::PATH_REGULARIZATION);
    SamplerState samp = SamplerDescriptorHeap[g_local.TexFilterDescHeapIdx];

    return Replay_kGt2(pos_n, normal_n, eta_i, surface_n, rd, triDiffs, rc_curr, 
        g_local.Alpha_min, regularization, samp, g_frame, globals);
}

RPT_Util::PathContext ReplayInCurrent(uint2 DTid, float3 origin, float2 lensSample, float3 pos, 
    float3 normal, bool metallic, float roughness, bool transmissive, bool trDepthGt0, 
    RPT_Util::Reconnection rc, ReSTIR_Util::Globals globals)
{
    GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::BASE_COLOR];
    const float3 baseColor = g_baseColor[DTid].rgb;

    float eta_t = DEFAULT_ETA_T;
    float eta_i = DEFAULT_ETA_I;

    if(transmissive)
    {
        GBUFFER_IOR g_ior = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::IOR];

        float ior = g_ior[DTid];
        eta_i = GBuffer::DecodeIOR(ior);
    }

    const float3 wo = normalize(origin - pos);
    BSDF::ShadingData surface = BSDF::ShadingData::Init(normal, wo, metallic, roughness, 
        baseColor, eta_i, eta_t, transmissive, trDepthGt0);

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

    const bool regularization = IS_CB_FLAG_SET(CB_IND_FLAGS::PATH_REGULARIZATION);
    SamplerState samp = SamplerDescriptorHeap[g_local.TexFilterDescHeapIdx];

    return Replay_kGt2(pos, normal, eta_i, surface, rd, triDiffs, rc, g_local.Alpha_min, 
        regularization, samp, g_frame, globals);
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_PT_REPLAY_GROUP_DIM_X, RESTIR_PT_REPLAY_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING == 1
    uint16_t2 swizzledGid;

    uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint16_t2(RESTIR_PT_TEMPORAL_GROUP_DIM_X, RESTIR_PT_TEMPORAL_GROUP_DIM_Y),
        uint16_t(g_local.DispatchDimX_NumGroupsInTile & 0xffff), 
        RESTIR_PT_TILE_WIDTH, 
        RESTIR_PT_LOG2_TILE_WIDTH, 
        uint16_t(g_local.DispatchDimX_NumGroupsInTile >> 16),
        swizzledGid);
#else
    uint2 swizzledDTid = DTid.xy;
    const uint2 swizzledGid = Gid.xy;
#endif

#if defined (TEMPORAL_TO_CURRENT) || defined (CURRENT_TO_TEMPORAL)
    if(IS_CB_FLAG_SET(CB_IND_FLAGS::SORT_TEMPORAL))
#else
    if(IS_CB_FLAG_SET(CB_IND_FLAGS::SORT_SPATIAL))
#endif
    {
#if defined (TEMPORAL_TO_CURRENT) || defined (SPATIAL_TO_CURRENT)
        uint descHeapIdx = g_local.ThreadMap_NtC_DescHeapIdx;
#elif defined (CURRENT_TO_TEMPORAL) || defined (CURRENT_TO_SPATIAL)
        uint descHeapIdx = g_local.ThreadMap_CtN_DescHeapIdx;
#else
#error Unknown shift type
#endif

        bool error;
        swizzledDTid = RPT_Util::DecodeSorted(swizzledDTid, descHeapIdx, error);

        if(error)
            return;
    }

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

#if defined (TEMPORAL_TO_CURRENT) || defined(CURRENT_TO_TEMPORAL)
    // Check if there is valid history data for temporal reuse
    GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::MOTION_VECTOR];
    const float2 motionVec = g_motionVector[swizzledDTid];
    const float2 currUV = (swizzledDTid + 0.5f) / renderDim;
    const float2 prevUV = currUV - motionVec;
    int2 prevPixel = prevUV * renderDim;

    // No temporal history
    if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
        return;

    GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    const float z_view_n = g_prevDepth[prevPixel];

    // No temporal history
    if(z_view_n == FLT_MAX)
        return;

    float2 lensSample_n = 0;
    float3 origin_n = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, 
        g_frame.PrevViewInv._m23);
    if(g_frame.DoF)
    {
        RNG rngDoF = RNG::Init(RNG::PCG3d(prevPixel.xyx).zy, g_frame.FrameNum - 1);
        lensSample_n = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
        lensSample_n *= g_frame.LensRadius;
    }

    const float3 pos_n = Math::WorldPosFromScreenSpace2(prevPixel, renderDim, z_view_n, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevCameraJitter, 
        g_frame.PrevView[0].xyz, g_frame.PrevView[1].xyz, g_frame.PrevView[2].xyz, 
        g_frame.DoF, lensSample_n, g_frame.FocusDepth, origin_n);

    // No temporal history
    if(!RPT_Util::PlaneHeuristic(pos_n, normal, pos, z_view, 0.01))
        return;

    GBUFFER_METALLIC_ROUGHNESS g_prevMR = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 mr_n = g_prevMR[prevPixel];
    GBuffer::Flags flags_n = GBuffer::DecodeMetallic(mr_n.x);

    // No temporal history
    if(flags_n.emissive || abs(mr_n.y - mr.y) > 0.3)
        return;
#endif

    ReSTIR_Util::Globals globals = InitGlobals();

    // Replay temporal reservoir's path
#if defined (TEMPORAL_TO_CURRENT)
    RPT_Util::Reservoir r_prev = RPT_Util::Reservoir::Load_Metadata<Texture2D<uint2> >(
        prevPixel, g_local.PrevReservoir_A_DescHeapIdx);

    // If reconnection vertex exists and replay is needed
    if(!r_prev.rc.Empty() && (r_prev.rc.k > 2))
    {
        r_prev.Load_Reconnection<NEE_EMISSIVE, Texture2D<uint4>, Texture2D<uint4>, 
            Texture2D<half>, Texture2D<float2>, Texture2D<uint> >(prevPixel, 
            g_local.PrevReservoir_A_DescHeapIdx + 2, 
            g_local.PrevReservoir_A_DescHeapIdx + 3,
            g_local.PrevReservoir_A_DescHeapIdx + 4,
            g_local.PrevReservoir_A_DescHeapIdx + 5,
            g_local.PrevReservoir_A_DescHeapIdx + 6);

        RPT_Util::PathContext ctx_ntc = ReplayInCurrent(swizzledDTid, origin, lensSample, 
            pos, normal, flags.metallic, mr.y, flags.transmissive, flags.trDepthGt0, 
            r_prev.rc, globals);

        ctx_ntc.Write(swizzledDTid, g_local.RBufferA_NtC_DescHeapIdx, 
            g_local.RBufferA_NtC_DescHeapIdx + 1,
            g_local.RBufferA_NtC_DescHeapIdx + 2, 
            r_prev.rc.IsCase3());
    }

    // Replay spatial reservoir's path
#elif defined(SPATIAL_TO_CURRENT)
    Texture2D<uint2> g_neighbor = ResourceDescriptorHeap[g_local.SpatialNeighborHeapIdx];
    int2 samplePos = (int2)g_neighbor[swizzledDTid];

    if(samplePos.x == UINT8_MAX)
        return;

    samplePos -= RPT_Util::SPATIAL_NEIGHBOR_OFFSET;
    samplePos += swizzledDTid;

    RPT_Util::Reservoir r_spatial = RPT_Util::Reservoir::Load_Metadata<Texture2D<uint2> >(
        samplePos, g_local.Reservoir_A_DescHeapIdx);

    if(!r_spatial.rc.Empty() && (r_spatial.rc.k > 2))
    {
        r_spatial.Load_Reconnection<NEE_EMISSIVE, Texture2D<uint4>, Texture2D<uint4>, 
            Texture2D<half>, Texture2D<float2>, Texture2D<uint> >(samplePos, 
            g_local.Reservoir_A_DescHeapIdx + 2, 
            g_local.Reservoir_A_DescHeapIdx + 3,
            g_local.Reservoir_A_DescHeapIdx + 4,
            g_local.Reservoir_A_DescHeapIdx + 5,
            g_local.Reservoir_A_DescHeapIdx + 6);

        RPT_Util::PathContext ctx_ntc = ReplayInCurrent(swizzledDTid, origin, lensSample, 
            pos, normal, flags.metallic, mr.y, flags.transmissive, flags.trDepthGt0, 
            r_spatial.rc, globals);

        ctx_ntc.Write(swizzledDTid, g_local.RBufferA_NtC_DescHeapIdx, 
            g_local.RBufferA_NtC_DescHeapIdx + 1,
            g_local.RBufferA_NtC_DescHeapIdx + 2, 
            r_spatial.rc.IsCase3());
    }

    // Replay current reservoir's path in temporal
#elif defined(CURRENT_TO_TEMPORAL)
    RPT_Util::Reservoir r_curr = RPT_Util::Reservoir::Load_Metadata<RWTexture2D<uint2> >(
        swizzledDTid, g_local.Reservoir_A_DescHeapIdx);

    if(!r_curr.rc.Empty() && (r_curr.rc.k > 2))
    {
        r_curr.Load_Reconnection<NEE_EMISSIVE, RWTexture2D<uint4>, RWTexture2D<uint4>, 
            RWTexture2D<half>, RWTexture2D<float2>, RWTexture2D<uint> >(swizzledDTid, 
            g_local.Reservoir_A_DescHeapIdx + 2, 
            g_local.Reservoir_A_DescHeapIdx + 3,
            g_local.Reservoir_A_DescHeapIdx + 4,
            g_local.Reservoir_A_DescHeapIdx + 5,
            g_local.Reservoir_A_DescHeapIdx + 6);

        RPT_Util::PathContext ctx_ctn = ReplayCurrentInTemporalDomain(prevPixel, origin_n, 
            lensSample_n, pos_n, flags_n.metallic, mr_n.y, flags_n.transmissive, flags_n.trDepthGt0, 
            r_curr.rc, globals);

        ctx_ctn.Write(swizzledDTid, g_local.RBufferA_CtN_DescHeapIdx, 
            g_local.RBufferA_CtN_DescHeapIdx + 1,
            g_local.RBufferA_CtN_DescHeapIdx + 2, 
            r_curr.rc.IsCase3());
    }

    // Replay current reservoir's path in spatial neighbor
#elif defined(CURRENT_TO_SPATIAL)
    RPT_Util::Reservoir r_curr = RPT_Util::Reservoir::Load_Metadata<Texture2D<uint2> >(
        swizzledDTid, g_local.Reservoir_A_DescHeapIdx);

    if(!r_curr.rc.Empty() && (r_curr.rc.k > 2))
    {
        r_curr.Load_Reconnection<NEE_EMISSIVE, Texture2D<uint4>, Texture2D<uint4>, 
            Texture2D<half>, Texture2D<float2>, Texture2D<uint> >(swizzledDTid, 
            g_local.Reservoir_A_DescHeapIdx + 2, 
            g_local.Reservoir_A_DescHeapIdx + 3,
            g_local.Reservoir_A_DescHeapIdx + 4,
            g_local.Reservoir_A_DescHeapIdx + 5,
            g_local.Reservoir_A_DescHeapIdx + 6);

        Texture2D<uint2> g_neighbor = ResourceDescriptorHeap[g_local.SpatialNeighborHeapIdx];
        int2 samplePos = (int2)g_neighbor[swizzledDTid];

        if(samplePos.x == UINT8_MAX)
            return;

        samplePos -= RPT_Util::SPATIAL_NEIGHBOR_OFFSET;
        samplePos += swizzledDTid;

        RPT_Util::PathContext ctx_ctn = ReplayCurrentInSpatialDomain(samplePos, r_curr.rc, 
            globals);
        ctx_ctn.Write(swizzledDTid, g_local.RBufferA_CtN_DescHeapIdx, 
            g_local.RBufferA_CtN_DescHeapIdx + 1,
            g_local.RBufferA_CtN_DescHeapIdx + 2, 
            r_curr.rc.IsCase3());
    }
#endif
}
