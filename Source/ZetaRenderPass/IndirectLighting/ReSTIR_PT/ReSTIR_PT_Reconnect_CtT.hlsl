#ifndef NEE_EMISSIVE
#define NEE_EMISSIVE 0
#endif

#include "../../Common/Common.hlsli"
#include "Util.hlsli"

#define THREAD_GROUP_SWIZZLING 1

using namespace ReSTIR_Util;
using namespace RPT_Util;

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

ReSTIR_Util::Globals InitGlobals(bool transmissive)
{
    ReSTIR_Util::Globals globals;
    globals.bvh = g_bvh;
    globals.frameMeshData = g_frameMeshData;
    globals.vertices = g_vertices;
    globals.indices = g_indices;
    globals.materials = g_materials;
    uint maxNonTrBounces = g_local.Packed & 0xf;
    uint maxGlossyTrBounces = (g_local.Packed >> 4) & 0xf;
    globals.maxNumBounces = transmissive ? (uint16_t)maxGlossyTrBounces :
        (uint16_t)maxNonTrBounces;

    return globals;
}

// Shift base path in current pixel's domain to offset path in temporal domain
OffsetPath ShiftCurrentToTemporal(uint2 DTid, uint2 prevPosSS, float3 origin,
    float2 lensSample, float3 prevPos, GBuffer::Flags prevFlags, float prevRoughness,
    Reconnection rc_curr, Globals globals)
{
    float eta_curr = ETA_AIR;
    float eta_next = DEFAULT_ETA_MAT;

    if(prevFlags.transmissive)
    {
        GBUFFER_IOR g_ior = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::IOR];

        float ior = g_ior[prevPosSS];
        eta_next = GBuffer::DecodeIOR(ior);
    }

    GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_prevNormal[prevPosSS]);

    GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
        GBUFFER_OFFSET::BASE_COLOR];
    const float4 baseColor = prevFlags.subsurface ? g_prevBaseColor[prevPosSS] :
        float4(g_prevBaseColor[prevPosSS].rgb, 0);

    float coat_weight = 0;
    float3 coat_color = 0.0f;
    float coat_roughness = 0;
    float coat_ior = DEFAULT_ETA_COAT;

    if(prevFlags.coated)
    {
        GBUFFER_COAT g_coat = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::COAT];
        uint3 packed = g_coat[DTid.xy].xyz;

        GBuffer::Coat coat = GBuffer::UnpackCoat(packed);
        coat_weight = coat.weight;
        coat_color = coat.color;
        coat_roughness = coat.roughness;
        coat_ior = coat.ior;
    }

    const float3 wo = normalize(origin - prevPos);
    BSDF::ShadingData surface = BSDF::ShadingData::Init(normal, wo, prevFlags.metallic, 
        prevRoughness, baseColor.rgb, eta_curr, eta_next, prevFlags.transmissive, 
        prevFlags.trDepthGt0, (half)baseColor.w, coat_weight, coat_color, 
        coat_roughness, coat_ior);

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

    bool regularization = IS_CB_FLAG_SET(CB_IND_FLAGS::PATH_REGULARIZATION);

    return RPT_Util::Shift2<NEE_EMISSIVE>(DTid, prevPos, normal, eta_next, surface, 
        rd, triDiffs, rc_curr, g_local.RBufferA_CtN_DescHeapIdx, 
        g_local.RBufferA_CtN_DescHeapIdx + 1, g_local.RBufferA_CtN_DescHeapIdx + 2, 
        g_local.RBufferA_CtN_DescHeapIdx + 3, g_local.Alpha_min, regularization, 
        g_frame, globals);
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
    GBuffer::Flags flags = GBuffer::DecodeMetallic(mr.x);

    if (flags.invalid || flags.emissive)
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
    GBuffer::Flags prevFlags = GBuffer::DecodeMetallic(prevMR.x);

    // No temporal history
    if(prevFlags.emissive || 
        (abs(prevMR.y - mr.y) > MAX_ROUGHNESS_DIFF_TEMPORAL_REUSE) || 
        (prevFlags.transmissive != flags.transmissive))
        return;

    Reservoir r_curr = Reservoir::Load_NonReconnection<RWTexture2D<uint4>, 
        RWTexture2D<float2> >(swizzledDTid, g_local.Reservoir_A_DescHeapIdx, 
        g_local.Reservoir_A_DescHeapIdx + 1);
    Reservoir r_prev = Reservoir::Load_Metadata<Texture2D<uint4> >(
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

        Globals globals = InitGlobals(flags.transmissive);
        OffsetPath shift = ShiftCurrentToTemporal(swizzledDTid, prevPixel, origin_t, 
            lensSample_t, prevPos, prevFlags, prevMR.y, r_curr.rc, globals);
        float target_prev = Math::Luminance(shift.target);

        // When target_prev = 0, ris weight works out to be existing w_sum, so
        // write to memory can be skipped
        if(target_prev > 0)
        {
            float targetLum_curr = r_curr.W > 0 ? r_curr.w_sum / r_curr.W : 0;
            float jacobian = r_curr.rc.partialJacobian > 0 ? 
                shift.partialJacobian / r_curr.rc.partialJacobian : 0;
            float m_curr = targetLum_curr / (targetLum_curr + r_prev.M * target_prev * jacobian);
            r_curr.w_sum *= m_curr;

            r_curr.WriteWSum(swizzledDTid, g_local.Reservoir_A_DescHeapIdx + 1);
        }
    }
}
