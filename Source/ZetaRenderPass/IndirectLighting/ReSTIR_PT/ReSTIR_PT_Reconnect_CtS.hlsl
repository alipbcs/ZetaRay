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

// Shift base path in current pixel's domain to offset path in spatial pixel's domain
RPT_Util::OffsetPath ShiftCurrentToSpatial(uint2 DTid, uint2 samplePosSS, RPT_Util::Reconnection rc_curr,
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
    GBUFFER_TRANSMISSION g_tr = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::TRANSMISSION];

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
        
    bool metallic_n;
    bool transmissive_n;
    GBuffer::DecodeMetallicTr(mr_n.x, metallic_n, transmissive_n);

    float3 normal_n = Math::DecodeUnitVector(g_normal[samplePosSS]);
    float3 baseColor_n = g_baseColor[samplePosSS].rgb;
    float eta_i = DEFAULT_ETA_I;
    float specTr_n = DEFAULT_SPECULAR_TRANSMISSION;

    if(transmissive_n)
    {
        float2 tr_ior = g_tr[samplePosSS];
        specTr_n = tr_ior.x;
        eta_i = GBuffer::DecodeIOR(tr_ior.y);
    }

    const float3 wo_n = normalize(origin_n - pos_n);
    BSDF::ShadingData surface_n = BSDF::ShadingData::Init(normal_n, wo_n, metallic_n, 
        mr_n.y, baseColor_n, eta_i, DEFAULT_ETA_T, specTr_n);

    Math::TriDifferentials triDiffs;
    RT::RayDifferentials rd;

    if(rc_curr.k == 2)
    {
        GBUFFER_TRI_DIFF_GEO_A g_triA = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::TRI_DIFF_GEO_A];
        GBUFFER_TRI_DIFF_GEO_B g_triB = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::TRI_DIFF_GEO_B];
        const uint4 packed_a = g_triA[samplePosSS];
        const uint2 packed_b = g_triB[samplePosSS];

        triDiffs = Math::TriDifferentials::Unpack(packed_a, packed_b);
        float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

        rd = RT::RayDifferentials::Init(samplePosSS, renderDim, g_frame.TanHalfFOV, 
            g_frame.AspectRatio, g_frame.CurrCameraJitter, 
            g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, 
            g_frame.CurrView[2].xyz, g_frame.DoF, g_frame.FocusDepth, 
            lensSample_n, origin_n);
    }

    const uint16_t maxDiffuseBounces = (uint16_t)(g_local.Packed & 0xf);
    bool regularization = IS_CB_FLAG_SET(CB_IND_FLAGS::PATH_REGULARIZATION);
    SamplerState samp = SamplerDescriptorHeap[g_local.TexFilterDescHeapIdx];

    return RPT_Util::Shift2<NEE_EMISSIVE>(DTid, pos_n, normal_n, eta_i, surface_n, 
        rd, triDiffs, rc_curr, g_local.RBufferA_CtN_DescHeapIdx, 
        g_local.RBufferA_CtN_DescHeapIdx + 1, g_local.RBufferA_CtN_DescHeapIdx + 2, 
        g_local.Alpha_min, regularization, samp, g_frame, globals);
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
        swizzledDTid = RPT_Util::DecodeSorted(swizzledDTid, g_local.ThreadMap_CtN_DescHeapIdx, error);

        if(error)
            return;
    }

    Texture2D<uint2> g_neighbor = ResourceDescriptorHeap[g_local.SpatialNeighborHeapIdx];
    int2 samplePos = (int2)g_neighbor[swizzledDTid];

    if(samplePos.x == UINT8_MAX)
        return;

    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 mr = g_metallicRoughness[swizzledDTid];
    bool isMetallic;
    bool isTransmissive;
    bool isEmissive;
    bool invalid;
    GBuffer::DecodeMetallic(mr.x, isMetallic, isTransmissive, isEmissive, invalid);

    if (invalid || isEmissive)
        return;

    samplePos -= RPT_Util::SPATIAL_NEIGHBOR_OFFSET;
    samplePos += swizzledDTid;

    RPT_Util::Reservoir r_curr = RPT_Util::Reservoir::Load_NonReconnection<Texture2D<uint2>, 
        Texture2D<float2> >(swizzledDTid, g_local.Reservoir_A_DescHeapIdx, 
        g_local.Reservoir_A_DescHeapIdx + 1);
    RPT_Util::Reservoir r_spatial = RPT_Util::Reservoir::Load_Metadata<Texture2D<uint2> >(
        samplePos, g_local.Reservoir_A_DescHeapIdx);

    // Shift current reservoir's path to spatial pixel and resample
    if((r_curr.w_sum != 0) && !r_curr.rc.Empty())
    {
        r_curr.Load_Reconnection<NEE_EMISSIVE, Texture2D<uint4>, Texture2D<uint4>, 
            Texture2D<half>, Texture2D<float2>, Texture2D<uint> >(swizzledDTid, 
            g_local.Reservoir_A_DescHeapIdx + 2, 
            g_local.Reservoir_A_DescHeapIdx + 3,
            g_local.Reservoir_A_DescHeapIdx + 4,
            g_local.Reservoir_A_DescHeapIdx + 5,
            g_local.Reservoir_A_DescHeapIdx + 6);

        ReSTIR_Util::Globals globals = InitGlobals();
        RPT_Util::OffsetPath shift = ShiftCurrentToSpatial(swizzledDTid, samplePos, r_curr.rc, globals);
        float target_spatial = Math::Luminance(shift.target);

        if(target_spatial > 0)
        {
            float targetLum_curr = r_curr.W > 0 ? r_curr.w_sum / r_curr.W : 0;
            float jacobian = r_curr.rc.partialJacobian > 0 ? 
                shift.partialJacobian / r_curr.rc.partialJacobian : 0;
            float numerator = r_curr.M * targetLum_curr;
            float denom = numerator + r_spatial.M * target_spatial * jacobian;
            float m_curr = denom > 0 ? numerator / denom : 0;
            r_curr.w_sum *= m_curr;
        }

        r_curr.WriteWSum(swizzledDTid, g_local.PrevReservoir_A_DescHeapIdx + 1);
    }
}
