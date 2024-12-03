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

Globals InitGlobals(bool transmissive)
{
    Globals globals;
    globals.bvh = g_bvh;
    globals.frameMeshData = g_frameMeshData;
    globals.vertices = g_vertices;
    globals.indices = g_indices;
    globals.materials = g_materials;
    uint maxNonTrBounces = g_local.Packed & 0xf;
    uint maxGlossyTrBounces = (g_local.Packed >> PACKED_INDEX::NUM_GLOSSY_BOUNCES) & 0xf;
    globals.maxNumBounces = transmissive ? (uint16_t)maxGlossyTrBounces :
        (uint16_t)maxNonTrBounces;

    return globals;
}

// Shift base path in temporal domain to offset path in current pixel's domain
OffsetPath ShiftTemporalToCurrent(uint2 DTid, float3 origin, float2 lensSample,
    float3 pos, float3 normal, GBuffer::Flags flags, float roughness, Reconnection rc_prev, 
    Globals globals)
{
    GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::BASE_COLOR];
    const float4 baseColor = flags.subsurface ? g_baseColor[DTid] : 
        float4(g_baseColor[DTid].rgb, 0);

    float eta_next = DEFAULT_ETA_MAT;

    if(flags.transmissive)
    {
        GBUFFER_IOR g_ior = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::IOR];

        float ior = g_ior[DTid];
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
        uint3 packed = g_coat[DTid].xyz;

        GBuffer::Coat coat = GBuffer::UnpackCoat(packed);
        coat_weight = coat.weight;
        coat_color = coat.color;
        coat_roughness = coat.roughness;
        coat_ior = coat.ior;
    }

    const float3 wo = normalize(origin - pos);
    BSDF::ShadingData surface = BSDF::ShadingData::Init(normal, wo, flags.metallic, 
        roughness, baseColor.rgb, ETA_AIR, eta_next, flags.transmissive, flags.trDepthGt0, 
        (half)baseColor.w, coat_weight, coat_color, coat_roughness, coat_ior);

    Math::TriDifferentials triDiffs;
    RT::RayDifferentials rd = RT::RayDifferentials::Init();

    if(rc_prev.k == 2)
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

    bool regularization = IS_CB_FLAG_SET(CB_IND_FLAGS::PATH_REGULARIZATION);

    return RPT_Util::Shift2<NEE_EMISSIVE, true>(DTid, pos, normal, eta_next, 
        surface, rd, triDiffs, rc_prev, g_local.RBufferA_NtC_DescHeapIdx, 
        g_local.RBufferA_NtC_DescHeapIdx + 1, g_local.RBufferA_NtC_DescHeapIdx + 2, 
        g_local.RBufferA_NtC_DescHeapIdx + 3, g_local.Alpha_min, regularization, 
        g_frame, globals);
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_PT_TEMPORAL_GROUP_DIM_X, RESTIR_PT_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING == 1
    uint2 swizzledGid;

    uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint2(RESTIR_PT_TEMPORAL_GROUP_DIM_X, RESTIR_PT_TEMPORAL_GROUP_DIM_Y),
        g_local.DispatchDimX_NumGroupsInTile & 0xffff, 
        RESTIR_PT_TILE_WIDTH, 
        RESTIR_PT_LOG2_TILE_WIDTH, 
        g_local.DispatchDimX_NumGroupsInTile >> 16,
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
        swizzledDTid = RPT_Util::DecodeSorted(swizzledDTid, g_local.ThreadMap_NtC_DescHeapIdx, error);

        if(error)
            return;
    }

    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[
        g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 mr = g_metallicRoughness[swizzledDTid];
    GBuffer::Flags flags = GBuffer::DecodeMetallic(mr.x);

    if (flags.invalid || flags.emissive)
        return;

    RPT_Util::Reservoir r_curr = RPT_Util::Reservoir::Load_NonReconnection<
        RWTexture2D<uint4>, RWTexture2D<float2> >(swizzledDTid, 
        g_local.Reservoir_A_DescHeapIdx, g_local.Reservoir_A_DescHeapIdx + 1);
    r_curr.LoadTarget(swizzledDTid, g_local.TargetDescHeapIdx);

    GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[
        g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float2 motionVec = g_motionVector[swizzledDTid];
    const float2 currUV = (swizzledDTid + 0.5f) / renderDim;
    const float2 prevUV = currUV - motionVec;
    int2 prevPixel = prevUV * renderDim;

    // No temporal history
    if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
    {
        if(!IS_CB_FLAG_SET(CB_IND_FLAGS::SPATIAL_RESAMPLE))
        {
            RPT_Util::WriteOutputColor(swizzledDTid, r_curr.target * r_curr.W, g_local.Packed, 
                g_local.FinalDescHeapIdx, g_frame);
        }

        return;
    }

    // Prepare previous pixel's geometry and material data
    GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    const float prevViewDepth = g_prevDepth[prevPixel];

    // No temporal history
    if(prevViewDepth == FLT_MAX)
    {
        if(!IS_CB_FLAG_SET(CB_IND_FLAGS::SPATIAL_RESAMPLE))
        {
            RPT_Util::WriteOutputColor(swizzledDTid, r_curr.target * r_curr.W, 
                g_local.Packed, g_local.FinalDescHeapIdx, g_frame);
        }

        return;
    }

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

    // Skip if not on the same surface
    if(!RPT_Util::PlaneHeuristic(prevPos, normal, pos, z_view, MAX_PLANE_DIST_REUSE))
    {
        if(!IS_CB_FLAG_SET(CB_IND_FLAGS::SPATIAL_RESAMPLE))
        {
            RPT_Util::WriteOutputColor(swizzledDTid, r_curr.target * r_curr.W, 
                g_local.Packed, g_local.FinalDescHeapIdx, g_frame);
        }

        return;
    }

    GBUFFER_METALLIC_ROUGHNESS g_prevMR = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 prevMR = g_prevMR[prevPixel];
    GBuffer::Flags prevFlags = GBuffer::DecodeMetallic(prevMR.x);

    // Skip if not on the same surface
    if(prevFlags.emissive || 
        abs(prevMR.y - mr.y) > MAX_ROUGHNESS_DIFF_TEMPORAL_REUSE || 
        (prevFlags.transmissive != flags.transmissive))
    {
        if(!IS_CB_FLAG_SET(CB_IND_FLAGS::SPATIAL_RESAMPLE))
        {
            RPT_Util::WriteOutputColor(swizzledDTid, r_curr.target * r_curr.W,
                g_local.Packed, g_local.FinalDescHeapIdx, g_frame);
        }

        return;
    }

    Reservoir r_prev = Reservoir::Load_NonReconnection<Texture2D<uint4>, 
        Texture2D<float2> >(prevPixel, g_local.PrevReservoir_A_DescHeapIdx, 
        g_local.PrevReservoir_A_DescHeapIdx + 1);

    Globals globals = InitGlobals(flags.transmissive);
    const uint16_t M_new = r_curr.M + r_prev.M;

    uint M_max = (g_local.Packed >> PACKED_INDEX::MAX_TEMPORAL_M) & 0xf;

    // Temporal history can't be shifted for reuse
    if(r_prev.rc.Empty())
    {
        // Recompute W as w_sum may have changed after the current to temporal shift
        float targetLum = Math::Luminance(r_curr.target);
        r_curr.W = targetLum > 0 ? r_curr.w_sum / targetLum : 0;
        r_curr.M = M_new;

        r_curr.WriteReservoirData2(swizzledDTid, g_local.Reservoir_A_DescHeapIdx,
            g_local.Reservoir_A_DescHeapIdx + 1, M_max);

        if(!IS_CB_FLAG_SET(CB_IND_FLAGS::SPATIAL_RESAMPLE))
        {
            RPT_Util::WriteOutputColor(swizzledDTid, r_curr.target * r_curr.W,
                g_local.Packed, g_local.FinalDescHeapIdx, g_frame);
        }

        return;
    }

    r_prev.Load_Reconnection<NEE_EMISSIVE, Texture2D<uint4>, Texture2D<uint4>, 
        Texture2D<half>, Texture2D<float2>, Texture2D<uint2> >(prevPixel, 
        g_local.PrevReservoir_A_DescHeapIdx + 2, 
        g_local.PrevReservoir_A_DescHeapIdx + 3,
        g_local.PrevReservoir_A_DescHeapIdx + 4,
        g_local.PrevReservoir_A_DescHeapIdx + 5,
        g_local.PrevReservoir_A_DescHeapIdx + 6);

    if(r_prev.rc.IsCase1() || r_prev.rc.IsCase2())
    {
        const RT::MeshInstance meshData = g_frameMeshData[NonUniformResourceIndex(r_prev.rc.meshIdx)];
        float3 prevTranslation = meshData.Translation - meshData.dTranslation;
        float4 q_prev = Math::DecodeNormalized4(meshData.PrevRotation);
        // due to quantization, it's necessary to renormalize
        q_prev = normalize(q_prev);
        float3 x_local = Math::InverseTransformTRS(r_prev.rc.x_k, prevTranslation, 
            q_prev, meshData.PrevScale);

        float4 q_curr = Math::DecodeNormalized4(meshData.Rotation);
        q_curr = normalize(q_curr);
        r_prev.rc.x_k = Math::TransformTRS(x_local, meshData.Translation, 
            q_curr, meshData.Scale);

        float4 dRot = q_prev - q_curr;
        float3 dScale = meshData.PrevScale - meshData.Scale;
        r_prev.rc.x_k_in_motion = dot(meshData.dTranslation, meshData.dTranslation) > 0;
        r_prev.rc.x_k_in_motion = r_prev.rc.x_k_in_motion || dot(dRot, dRot) > 0;
        r_prev.rc.x_k_in_motion = r_prev.rc.x_k_in_motion || dot(dScale, dScale) > 0;
    }

    // Shift temporal path to current pixel and resample
    OffsetPath shift = ShiftTemporalToCurrent(swizzledDTid, origin, lensSample,
        pos, normal, flags, mr.y, r_prev.rc, globals);

    float targetLum_curr = Math::Luminance(shift.target);
    float jacobian = r_prev.rc.partialJacobian > 0 ? 
        shift.partialJacobian / r_prev.rc.partialJacobian : 0;
    bool changed = false;

    // RIS weight becomes zero when target = 0, in which case only M needs to be
    // updated
    if(targetLum_curr > 1e-6 && jacobian > 1e-5)
    {
        RNG rng = RNG::Init(swizzledDTid.yx, g_frame.FrameNum + 31);

        float targetLum_prev = r_prev.W > 0 ? r_prev.w_sum / r_prev.W : 0;
        // Jacobian term in numerator cancels out with the same term in w_prev
        float numerator = r_prev.M * targetLum_prev;
        float denom = numerator / jacobian + targetLum_curr;
        // Balance heuristic
        float m_prev = denom > 0 ? numerator / denom : 0;
        float w_prev = m_prev * r_prev.W * targetLum_curr;

        if(r_curr.Update(w_prev, shift.target, r_prev.rc, rng))
        {
            r_curr.rc.partialJacobian = shift.partialJacobian;
            changed = true;
        }
    }

    float targetLum = Math::Luminance(r_curr.target);
    r_curr.W = targetLum > 0 ? r_curr.w_sum / targetLum : 0;
    r_curr.M = M_new;

    // If reconnection didn't change, skip writing it
    if(changed)
    {
        r_curr.Write<NEE_EMISSIVE>(swizzledDTid, g_local.Reservoir_A_DescHeapIdx,
            g_local.Reservoir_A_DescHeapIdx + 1, g_local.Reservoir_A_DescHeapIdx + 2, 
            g_local.Reservoir_A_DescHeapIdx + 3, g_local.Reservoir_A_DescHeapIdx + 4, 
            g_local.Reservoir_A_DescHeapIdx + 5, g_local.Reservoir_A_DescHeapIdx + 6,
            M_max);

        if(IS_CB_FLAG_SET(CB_IND_FLAGS::SPATIAL_RESAMPLE))
            r_curr.WriteTarget(swizzledDTid, g_local.TargetDescHeapIdx);
    }
    else
    {
        r_curr.WriteReservoirData(swizzledDTid, g_local.Reservoir_A_DescHeapIdx,
            g_local.Reservoir_A_DescHeapIdx + 1, M_max);
    }

    if(!IS_CB_FLAG_SET(CB_IND_FLAGS::SPATIAL_RESAMPLE))
    {
        float3 li = r_curr.target * r_curr.W;
        RPT_Util::DebugColor(r_curr.rc, g_local.Packed, li);
        RPT_Util::WriteOutputColor(swizzledDTid, li, g_local.Packed, 
            g_local.FinalDescHeapIdx, g_frame, false);
    }
}
