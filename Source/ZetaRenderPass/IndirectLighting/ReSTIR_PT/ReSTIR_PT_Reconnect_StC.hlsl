#ifndef NEE_EMISSIVE
#define NEE_EMISSIVE 0
#endif

#include "../../Common/Common.hlsli"
#include "Util.hlsli"

#define THREAD_GROUP_SWIZZLING 1

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

// Shift base path in spatial domain to offset path in current pixel's domain
RPT_Util::OffsetPath ShiftSpatialToCurrent(uint2 DTid, float3 origin, float2 lensSample, 
    float3 pos, float3 normal, bool metallic, float roughness, bool transmissive, 
    bool trDepthGt0, RPT_Util::Reconnection rc_spatial, ReSTIR_Util::Globals globals)
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

    Math::TriDifferentials triDiffs;
    RT::RayDifferentials rd;

    if(rc_spatial.k == 2)
    {
        GBUFFER_TRI_DIFF_GEO_A g_triA = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::TRI_DIFF_GEO_A];
        GBUFFER_TRI_DIFF_GEO_B g_triB = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::TRI_DIFF_GEO_B];
        const uint4 packed_a = g_triA[DTid];
        const uint2 packed_b = g_triB[DTid];

        triDiffs = Math::TriDifferentials::Unpack(packed_a, packed_b);
        float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

        rd = RT::RayDifferentials::Init(DTid, renderDim, g_frame.TanHalfFOV, 
            g_frame.AspectRatio, g_frame.CurrCameraJitter, 
            g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, 
            g_frame.CurrView[2].xyz, g_frame.DoF, g_frame.FocusDepth, 
            lensSample, origin);
    }

    const uint16_t maxDiffuseBounces = (uint16_t)(g_local.Packed & 0xf);
    bool regularization = IS_CB_FLAG_SET(CB_IND_FLAGS::PATH_REGULARIZATION);
    SamplerState samp = SamplerDescriptorHeap[g_local.TexFilterDescHeapIdx];

    return RPT_Util::Shift2<NEE_EMISSIVE>(DTid, pos, normal, eta_i, surface, rd, 
        triDiffs, rc_spatial, g_local.RBufferA_NtC_DescHeapIdx, 
        g_local.RBufferA_NtC_DescHeapIdx + 1, g_local.RBufferA_NtC_DescHeapIdx + 2, 
        g_local.Alpha_min, regularization, samp, g_frame, globals);
}

// Copies reservoir data along with the reconnection found after temporal resue (if any)
// to next frame's reservoir
void CopyToNextFrame(uint2 DTid, RPT_Util::Reservoir r_curr)
{
    if(!r_curr.rc.Empty())
    {
        r_curr.Load_Reconnection<NEE_EMISSIVE, Texture2D<uint4>, Texture2D<uint4>, 
            Texture2D<half>, Texture2D<float2>, Texture2D<uint> >(DTid, 
            g_local.Reservoir_A_DescHeapIdx + 2, 
            g_local.Reservoir_A_DescHeapIdx + 3,
            g_local.Reservoir_A_DescHeapIdx + 4,
            g_local.Reservoir_A_DescHeapIdx + 5,
            g_local.Reservoir_A_DescHeapIdx + 6);

        r_curr.Write<NEE_EMISSIVE>(DTid, g_local.PrevReservoir_A_DescHeapIdx,
            g_local.PrevReservoir_A_DescHeapIdx + 1, g_local.PrevReservoir_A_DescHeapIdx + 2, 
            g_local.PrevReservoir_A_DescHeapIdx + 3, g_local.PrevReservoir_A_DescHeapIdx + 4, 
            g_local.PrevReservoir_A_DescHeapIdx + 5, g_local.PrevReservoir_A_DescHeapIdx + 6,
            g_local.MaxSpatialM);
    }
    else
    {
        r_curr.WriteReservoirData(DTid, g_local.PrevReservoir_A_DescHeapIdx,
            g_local.PrevReservoir_A_DescHeapIdx + 1, g_local.MaxSpatialM);
    }
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_PT_SPATIAL_GROUP_DIM_X, RESTIR_PT_SPATIAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING == 1
    uint16_t2 swizzledGid;

    uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint16_t2(RESTIR_PT_SPATIAL_GROUP_DIM_X, RESTIR_PT_SPATIAL_GROUP_DIM_Y),
        uint16_t(g_local.DispatchDimX_NumGroupsInTile & 0xffff), 
        RESTIR_PT_TILE_WIDTH, 
        RESTIR_PT_LOG2_TILE_WIDTH, 
        uint16_t(g_local.DispatchDimX_NumGroupsInTile >> 16),
        swizzledGid);
#else
    uint2 swizzledDTid = DTid.xy;
    const uint2 swizzledGid = Gid.xy;
#endif

    if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
        return;

    if(IS_CB_FLAG_SET(CB_IND_FLAGS::SORT_SPATIAL))
    {
        bool error;
        swizzledDTid = RPT_Util::DecodeSorted(swizzledDTid, g_local.ThreadMap_NtC_DescHeapIdx, error);

        if(error)
            return;
    }

    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 mr = g_metallicRoughness[swizzledDTid];
    GBuffer::Flags flags = GBuffer::DecodeMetallic(mr.x);

    if (flags.invalid || flags.emissive)
        return;

    RPT_Util::Reservoir r_curr = RPT_Util::Reservoir::Load_NonReconnection<
        Texture2D<uint2>, Texture2D<float2> >(swizzledDTid, 
        g_local.Reservoir_A_DescHeapIdx, g_local.Reservoir_A_DescHeapIdx + 1);
    r_curr.LoadTarget(swizzledDTid, g_local.TargetDescHeapIdx);

    Texture2D<uint2> g_neighbor = ResourceDescriptorHeap[g_local.SpatialNeighborHeapIdx];
    int2 samplePos = (int2)g_neighbor[swizzledDTid];

    float waveSum = WaveActiveSum(r_curr.w_sum);
    float waveAvgExclusive = (waveSum - r_curr.w_sum) / WaveGetLaneCount();
    waveSum = WaveActiveSum(r_curr.w_sum * (samplePos.x == UINT8_MAX));

    // Couldn't find reusable spatial neighbor
    if(samplePos.x == UINT8_MAX)
    {
        if(IS_CB_FLAG_SET(CB_IND_FLAGS::BOILING_SUPPRESSION))
            RPT_Util::SuppressOutlierReservoirs(waveAvgExclusive, r_curr);

        RPT_Util::WriteOutputColor(swizzledDTid, r_curr.target * r_curr.W, 
            g_local.Packed, g_local.Final, g_frame);
        CopyToNextFrame(swizzledDTid, r_curr);

        return;
    }

    samplePos -= RPT_Util::SPATIAL_NEIGHBOR_OFFSET;
    samplePos += swizzledDTid;

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    const float z_view = g_depth[swizzledDTid];

    // Use the same lens sample that was used for this pixel's camera ray
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

    RPT_Util::Reservoir r_spatial = RPT_Util::Reservoir::Load_NonReconnection<Texture2D<uint2>, 
        Texture2D<float2> >(samplePos, g_local.Reservoir_A_DescHeapIdx, 
        g_local.Reservoir_A_DescHeapIdx + 1);

    RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.Final];
    const float prevDeriv = g_final[swizzledDTid].w;
    const float prevWSum = r_curr.w_sum;

    if((r_curr.w_sum != 0) && (r_spatial.M > 0) && !r_curr.rc.Empty())
        r_curr.LoadWSum(swizzledDTid, g_local.PrevReservoir_A_DescHeapIdx + 1);

    ReSTIR_Util::Globals globals = InitGlobals();
    const uint16_t M_new = r_curr.M + r_spatial.M;

    waveSum += WaveActiveSum(r_curr.w_sum * r_spatial.rc.Empty());

    // Spatial neighbor can't be shifted for reuse
    if(r_spatial.rc.Empty())
    {
        if(IS_CB_FLAG_SET(CB_IND_FLAGS::BOILING_SUPPRESSION))
            RPT_Util::SuppressOutlierReservoirs(waveAvgExclusive, r_curr);

        // Recompute W as w_sum may have changed after the current to spatial shift
        float targetLum = Math::Luminance(r_curr.target);
        r_curr.W = targetLum > 0 ? r_curr.w_sum / targetLum : 0;
        r_curr.M = M_new;

        CopyToNextFrame(swizzledDTid, r_curr);
        RPT_Util::WriteOutputColor(swizzledDTid, r_curr.target * r_curr.W,
            g_local.Packed, g_local.Final, g_frame);

        return;
    }

    r_spatial.Load_Reconnection<NEE_EMISSIVE, Texture2D<uint4>, Texture2D<uint4>, 
        Texture2D<half>, Texture2D<float2>, Texture2D<uint> >(samplePos, 
        g_local.Reservoir_A_DescHeapIdx + 2, 
        g_local.Reservoir_A_DescHeapIdx + 3,
        g_local.Reservoir_A_DescHeapIdx + 4,
        g_local.Reservoir_A_DescHeapIdx + 5,
        g_local.Reservoir_A_DescHeapIdx + 6);

    // Shift spatial neighbor's path to current pixel and resample
    RPT_Util::OffsetPath shift = ShiftSpatialToCurrent(swizzledDTid, origin, lensSample,
        pos, normal, flags.metallic, mr.y, flags.transmissive, flags.trDepthGt0, 
        r_spatial.rc, globals);

    float targetLum_curr = Math::Luminance(shift.target);
    float targetLum_spatial = r_spatial.w_sum / r_spatial.W;
    float jacobian = shift.partialJacobian / r_spatial.rc.partialJacobian;

    bool changed = false;
    
    // RIS weight becomes zero when target = 0
    if(targetLum_curr > 1e-5)
    {
        RNG rng = RNG::Init(RNG::PCG3d(swizzledDTid.xyy).xz, g_frame.FrameNum + 511);

        // Jacobian term in numerator cancels out with the same term in w_spatial
        float numerator = r_spatial.M * targetLum_spatial;
        float denom = numerator / jacobian + r_curr.M * targetLum_curr;
        // Balance heuristic
        float m_spatial = denom > 0 ? numerator / denom : 0;
        float w_spatial = m_spatial * r_spatial.W * targetLum_curr;

        if(r_curr.Update(w_spatial, shift.target, r_spatial.rc, rng))
        {
            r_curr.rc.partialJacobian = shift.partialJacobian;
            changed = true;
        }
    }

    float targetLum = Math::Luminance(r_curr.target);
    r_curr.W = targetLum > 0 ? r_curr.w_sum / targetLum : 0;
    r_curr.M = M_new;

    const float alpha = 0.45;
    float runningDeriv = alpha * (r_curr.w_sum - prevWSum) + (1 - alpha) * prevDeriv;

    if(IS_CB_FLAG_SET(CB_IND_FLAGS::BOILING_SUPPRESSION))
    {
        waveSum += WaveActiveSum(r_curr.w_sum);
        waveAvgExclusive = (waveSum - r_curr.w_sum) / WaveGetLaneCount();
        RPT_Util::SuppressOutlierReservoirs(waveAvgExclusive, r_curr);
    }

    // If reconnection didn't change, skip writing it
    if(changed)
    {
        r_curr.Write<NEE_EMISSIVE>(swizzledDTid, g_local.PrevReservoir_A_DescHeapIdx,
            g_local.PrevReservoir_A_DescHeapIdx + 1, g_local.PrevReservoir_A_DescHeapIdx + 2, 
            g_local.PrevReservoir_A_DescHeapIdx + 3, g_local.PrevReservoir_A_DescHeapIdx + 4, 
            g_local.PrevReservoir_A_DescHeapIdx + 5, g_local.PrevReservoir_A_DescHeapIdx + 6, 
            g_local.MaxSpatialM);

        // if(scale != numPasses)
        //     r_curr.WriteTarget(swizzledDTid, g_local.TargetDescHeapIdx);
    }
    else
        CopyToNextFrame(swizzledDTid, r_curr);

    float3 li = r_curr.target * r_curr.W;
    RPT_Util::DebugColor(r_curr.rc, g_local.Packed, li);
    RPT_Util::WriteOutputColor2(swizzledDTid, float4(li, runningDeriv), 
        g_local.Packed, g_local.Final, g_frame, false);
}
