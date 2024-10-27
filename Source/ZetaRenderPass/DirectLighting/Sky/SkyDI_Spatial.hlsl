#include "Resampling.hlsli"

#define THREAD_GROUP_SWIZZLING 1

using namespace SkyDI_Util;

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_SkyDI> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(SKY_DI_GROUP_DIM_X, SKY_DI_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
    uint16_t2 swizzledGid;

    uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint16_t2(SKY_DI_GROUP_DIM_X, SKY_DI_GROUP_DIM_Y),
        g_local.DispatchDimX, 
        SKY_DI_TILE_WIDTH, 
        SKY_DI_LOG2_TILE_WIDTH, 
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

    RNG rng = RNG::Init(RNG::PCG3d(swizzledDTid.yxx).yz, g_frame.FrameNum);

    Reservoir r = Reservoir::Load(swizzledDTid, 
        g_local.CurrReservoir_A_DescHeapIdx, g_local.CurrReservoir_A_DescHeapIdx + 1,
        g_local.CurrReservoir_A_DescHeapIdx + 2);
    r.LoadTarget(swizzledDTid, g_local.TargetDescHeapIdx);

    SpatialResample(swizzledDTid, pos, normal, z_view, mr.y, surface, 
        g_local.CurrReservoir_A_DescHeapIdx, 
        g_local.CurrReservoir_A_DescHeapIdx + 1, 
        g_local.CurrReservoir_A_DescHeapIdx + 2, 
        g_local.Alpha_min, g_bvh, g_frame, r, rng);

    {
        float3 ld = r.target * r.W;
        ld = any(isnan(ld)) ? 0 : ld;
        RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];

        if(g_frame.Accumulate && g_frame.CameraStatic && g_frame.NumFramesCameraStatic > 1)
        {
            float3 prev = g_final[swizzledDTid].rgb;
            g_final[swizzledDTid].rgb = prev + ld;
        }
        else
            g_final[swizzledDTid].rgb = ld;
    }
}