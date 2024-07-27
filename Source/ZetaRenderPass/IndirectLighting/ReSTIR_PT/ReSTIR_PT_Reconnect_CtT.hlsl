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

// Shift base path in current pixel's domain to offset path in temporal domain
RPT_Util::OffsetPath ShiftCurrentToTemporal(uint2 DTid, uint2 prevPosSS, float3 origin,
    float2 lensSample, float3 prevPos, bool prevMetallic, float prevRoughness, 
    bool prevTransmissive, RPT_Util::Reconnection rc_curr, ReSTIR_Util::Globals globals)
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
        baseColor, eta_i, eta_t, prevTransmissive);

    Math::TriDifferentials triDiffs;
    RT::RayDifferentials rd;

    if(rc_curr.k == 2)
    {
        GBUFFER_TRI_DIFF_GEO_A g_triA = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
            GBUFFER_OFFSET::TRI_DIFF_GEO_A];
        GBUFFER_TRI_DIFF_GEO_B g_triB = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
            GBUFFER_OFFSET::TRI_DIFF_GEO_B];
        const uint4 packed_a = g_triA[prevPosSS];
        const uint2 packed_b = g_triB[prevPosSS];

        triDiffs = Math::TriDifferentials::Unpack(packed_a, packed_b);
        float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

        rd = RT::RayDifferentials::Init(prevPosSS, renderDim, g_frame.TanHalfFOV, 
            g_frame.AspectRatio, g_frame.PrevCameraJitter, 
            g_frame.PrevView[0].xyz, g_frame.PrevView[1].xyz, 
            g_frame.PrevView[2].xyz, g_frame.DoF, g_frame.FocusDepth, 
            lensSample, origin);
    }

    const uint16_t maxDiffuseBounces = (uint16_t)(g_local.Packed & 0xf);
    bool regularization = IS_CB_FLAG_SET(CB_IND_FLAGS::PATH_REGULARIZATION);
    SamplerState samp = SamplerDescriptorHeap[g_local.TexFilterDescHeapIdx];

    return RPT_Util::Shift2<NEE_EMISSIVE>(DTid, prevPos, normal, eta_i, surface, 
        rd, triDiffs, rc_curr, g_local.RBufferA_CtN_DescHeapIdx, 
        g_local.RBufferA_CtN_DescHeapIdx + 1, g_local.RBufferA_CtN_DescHeapIdx + 2, 
        g_local.Alpha_min, regularization, samp, g_frame, globals);
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_PT_TEMPORAL_GROUP_DIM_X, RESTIR_PT_TEMPORAL_GROUP_DIM_Y, 1)]
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
    
    if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
        return;

    if(IS_CB_FLAG_SET(CB_IND_FLAGS::SORT_TEMPORAL))
    {
        bool error;
        swizzledDTid = RPT_Util::DecodeSorted(swizzledDTid, g_local.ThreadMap_CtN_DescHeapIdx, error);

        if(error)
            return;
    }

    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 mr = g_metallicRoughness[swizzledDTid];
    bool metallic;
    bool transmissive;
    bool emissive;
    bool invalid;
    GBuffer::DecodeMetallic(mr.x, metallic, transmissive, emissive, invalid);

    if (invalid || emissive)
        return;

    GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::MOTION_VECTOR];
    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float2 motionVec = g_motionVector[swizzledDTid];
    const float2 currUV = (swizzledDTid + 0.5f) / renderDim;
    const float2 prevUV = currUV - motionVec;
    int2 prevPixel = prevUV * renderDim;

    // No temporal history
    if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
        return;

    // Prepare previous pixel's geometry and material data
    GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    const float prevViewDepth = g_prevDepth[prevPixel];

    // No temporal history
    if(prevViewDepth == FLT_MAX)
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

    const float3 pos = Math::WorldPosFromScreenSpace2(swizzledDTid, renderDim, z_view, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrCameraJitter, 
        g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, g_frame.CurrView[2].xyz, 
        g_frame.DoF, lensSample, g_frame.FocusDepth, origin);

    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_normal[swizzledDTid]);

    float2 lensSample_t = 0;
    float3 origin_t = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, 
        g_frame.PrevViewInv._m23);
    if(g_frame.DoF)
    {
        RNG rngDoF = RNG::Init(RNG::PCG3d(prevPixel.xyx).zy, g_frame.FrameNum - 1);
        lensSample_t = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
        lensSample_t *= g_frame.LensRadius;
    }

    const float3 prevPos = Math::WorldPosFromScreenSpace2(prevPixel, renderDim, prevViewDepth, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevCameraJitter, 
        g_frame.PrevView[0].xyz, g_frame.PrevView[1].xyz, g_frame.PrevView[2].xyz, 
        g_frame.DoF, lensSample_t, g_frame.FocusDepth, origin_t);

    // No temporal history
    if(!RPT_Util::PlaneHeuristic(prevPos, normal, pos, z_view, 0.01))
        return;

    GBUFFER_METALLIC_ROUGHNESS g_prevMR = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 prevMR = g_prevMR[prevPixel];

    bool prevMetallic;
    bool prevTransmissive;
    bool prevEmissive;
    GBuffer::DecodeMetallic(prevMR.x, prevMetallic, prevTransmissive, prevEmissive);

    // No temporal history
    if(prevEmissive || (abs(prevMR.y - mr.y) > 0.3))
        return;

    RPT_Util::Reservoir r_curr = RPT_Util::Reservoir::Load_NonReconnection<RWTexture2D<uint2>, 
        RWTexture2D<float2> >(swizzledDTid, g_local.Reservoir_A_DescHeapIdx, 
        g_local.Reservoir_A_DescHeapIdx + 1);
    RPT_Util::Reservoir r_prev = RPT_Util::Reservoir::Load_Metadata<Texture2D<uint2> >(
        prevPixel, g_local.PrevReservoir_A_DescHeapIdx);

    // Shift current reservoir's path to previous pixel and resample
    if(r_curr.w_sum != 0 && r_prev.M > 0 && !r_curr.rc.Empty())
    {
        r_curr.Load_Reconnection<NEE_EMISSIVE, RWTexture2D<uint4>, RWTexture2D<uint4>, 
            RWTexture2D<half>, RWTexture2D<float2>, RWTexture2D<uint> >(swizzledDTid, 
            g_local.Reservoir_A_DescHeapIdx + 2, 
            g_local.Reservoir_A_DescHeapIdx + 3,
            g_local.Reservoir_A_DescHeapIdx + 4,
            g_local.Reservoir_A_DescHeapIdx + 5,
            g_local.Reservoir_A_DescHeapIdx + 6);

        ReSTIR_Util::Globals globals = InitGlobals();
        RPT_Util::OffsetPath shift = ShiftCurrentToTemporal(swizzledDTid, prevPixel, origin_t, 
            lensSample_t, prevPos, prevMetallic, prevMR.y, prevTransmissive, r_curr.rc, globals);
        float target_prev = Math::Luminance(shift.target);

        if(target_prev > 0)
        {
            float targetLum_curr = r_curr.W > 0 ? r_curr.w_sum / r_curr.W : 0;
            float jacobian = shift.partialJacobian / r_curr.rc.partialJacobian;
            float m_curr = targetLum_curr / (targetLum_curr + r_prev.M * target_prev * jacobian);
            r_curr.w_sum *= m_curr;
        
            r_curr.WriteWSum(swizzledDTid, g_local.Reservoir_A_DescHeapIdx + 1);
        }
    }
}
