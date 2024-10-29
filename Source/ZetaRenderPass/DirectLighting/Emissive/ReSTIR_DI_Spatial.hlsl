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
StructuredBuffer<RT::EmissiveTriangle> g_emissives : register(t2);

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

    Reservoir r = Reservoir::Load(swizzledDTid, g_local.CurrReservoir_A_DescHeapIdx, 
        g_local.CurrReservoir_A_DescHeapIdx + 1);
    bool disoccluded = false;

    if(r.lightIdx != UINT32_MAX)
    {
        RT::EmissiveTriangle tri = g_emissives[r.lightIdx];
        r.lightID = tri.ID;

        const float3 vtx1 = Light::DecodeEmissiveTriV1(tri);
        const float3 vtx2 = Light::DecodeEmissiveTriV2(tri);
        r.lightPos = (1.0f - r.bary.x - r.bary.y) * tri.Vtx0 + r.bary.x * vtx1 + r.bary.y * vtx2;

        r.lightNormal = cross(vtx1 - tri.Vtx0, vtx2 - tri.Vtx0);
        r.lightNormal = dot(r.lightNormal, r.lightNormal) == 0 ? r.lightNormal : 
            normalize(r.lightNormal);
        r.doubleSided = tri.IsDoubleSided();

        r.LoadTarget(swizzledDTid, g_local.TargetDescHeapIdx);
        disoccluded = r.target.x < 0 || r.target.y < 0 || r.target.z < 0;
        r.target = abs(r.target);
    }

    if(IS_CB_FLAG_SET(CB_RDI_FLAGS::EXTRA_DISOCCLUSION_SAMPLING))
    {
        // Skip thin geometry such as grass or fences
        disoccluded = disoccluded && (WaveActiveSum(disoccluded) > 3);
    }

    // Since spatial samples are expensive, take extra samples stochastically
    // per thread group for better coherency
    int numSamples = !IS_CB_FLAG_SET(CB_RDI_FLAGS::STOCHASTIC_SPATIAL) || (rng_group.Uniform() < PROB_EXTRA_SPATIAL_SAMPLES) ? 
        MIN_NUM_SPATIAL_SAMPLES + NUM_EXTRA_SPATIAL_SAMPLES : 
        MIN_NUM_SPATIAL_SAMPLES;
    numSamples = !disoccluded ? numSamples : MAX_NUM_SPATIAL_SAMPLES;

    RDI_Util::SpatialResample(swizzledDTid, numSamples, SPATIAL_SEARCH_RADIUS, pos, normal, z_view, 
        mr.y, surface, g_local.CurrReservoir_A_DescHeapIdx, g_local.CurrReservoir_A_DescHeapIdx + 1, 
        g_frame, g_emissives, g_bvh, r, rng_thread);

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