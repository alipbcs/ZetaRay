#include "../IndirectLighting_Common.h"
#include "Util.hlsli"

#define THREAD_GROUP_SWIZZLING 1
#define SPATIAL_SEARCH_RADIUS 15

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_ReSTIR_PT_SpatialSearch> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Utility Functions
//--------------------------------------------------------------------------------------

int2 FindSpatialNeighbor(uint2 DTid, float3 pos, float3 normal, bool metallic, float roughness,
    bool transmissive, float viewDepth, float radius, inout RNG rng)
{
    static const half2 k_samples[16] =
    {
        half2(-0.899423, 0.365076),
        half2(-0.744442, -0.124006),
        half2(-0.229714, 0.245876),
        half2(-0.545186, 0.741148),
        half2(-0.156274, -0.336366),
        half2(0.468400, 0.348798),
        half2(0.035776, 0.606928),
        half2(-0.208966, 0.904852),
        half2(-0.491070, -0.484810),
        half2(0.162490, -0.081156),
        half2(0.232062, -0.851382),
        half2(0.641310, -0.162124),
        half2(0.320798, 0.922460),
        half2(0.959086, 0.263642),
        half2(0.531136, -0.519002),
        half2(-0.223014, -0.774740)
    };

    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];

    // rotate sample sequence per pixel
    const float u0 = rng.Uniform();
    const uint offset = rng.UniformUint();
    const float theta = u0 * TWO_PI;
    const float sinTheta = sin(theta);
    const float cosTheta = cos(theta);
    const int2 renderDim = int2((int)g_frame.RenderWidth, (int)g_frame.RenderHeight);

    [loop]
    for (uint i = 0; i < 3; i++)
    {
        float2 sampleUV = k_samples[(offset + i) & 15];
        float2 rotated = float2(
            dot(sampleUV, float2(cosTheta, -sinTheta)),
            dot(sampleUV, float2(sinTheta, cosTheta)));
        rotated *= radius;
        const int2 samplePosSS = round(float2(DTid) + rotated);

        if (!Math::IsWithinBounds(samplePosSS, renderDim))
            continue;

        if(samplePosSS.x == DTid.x && samplePosSS.y == DTid.y)
            continue;

        const float sampleDepth = g_depth[samplePosSS];
        if (sampleDepth == FLT_MAX)
            continue;

        const float3 samplePos = Math::WorldPosFromScreenSpace(samplePosSS, renderDim,
            sampleDepth, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv, 
            g_frame.CurrCameraJitter);

        const float2 sampleMR = g_metallicRoughness[samplePosSS];
        
        bool sampleMetallic;
        bool sampleTransmissive;
        bool sampleEmissive;
        GBuffer::DecodeMetallic(sampleMR.x, sampleMetallic, sampleTransmissive, sampleEmissive);

        const float3 sampleNormal = Math::DecodeUnitVector(g_normal[samplePosSS]);

        if (!RPT_Util::PlaneHeuristic(samplePos, normal, pos, viewDepth, 0.1))
            continue;

        if(sampleEmissive)
            continue;

        if(abs(sampleMR.y - roughness) > 0.15)
            continue;

        if(transmissive != sampleTransmissive)
            continue;

        if(dot(sampleNormal, normal) < 0.5)
            continue;

        if(IS_CB_FLAG_SET(CB_IND_FLAGS::REJECT_OUTLIERS))
        {
            RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.Final];
            float derivative = g_final[samplePosSS].w;
            if(abs(derivative) > MAX_W_SUM_DERIV_SPATIAL_REUSE)
                continue;
        }

        return samplePosSS;
    }

    return UINT16_MAX;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_X, RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING == 1
    uint16_t2 swizzledGid;

    const uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint16_t2(RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_X, RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_Y),
        uint16_t(g_local.DispatchDimX_NumGroupsInTile & 0xffff), 
        RESTIR_PT_TILE_WIDTH, 
        RESTIR_PT_LOG2_TILE_WIDTH, 
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
    bool isMetallic;
    bool isTransmissive;
    bool isEmissive;
    bool invalid;
    GBuffer::DecodeMetallic(mr.x, isMetallic, isTransmissive, isEmissive, invalid);

    if (invalid || isEmissive)
        return;

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    const float viewDepth = g_depth[swizzledDTid];

    const float3 pos = Math::WorldPosFromScreenSpace(swizzledDTid,
        float2(g_frame.RenderWidth, g_frame.RenderHeight), viewDepth, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv, 
        g_frame.CurrCameraJitter);

    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_normal[swizzledDTid]);

    RNG rng = RNG::Init(swizzledDTid.yx, g_frame.FrameNum + 61);
    const uint16_t passIdx = (uint16_t)((g_local.Packed >> 12) & 0x3);
    const uint16_t scale = passIdx + (uint16_t)1;

    int2 neighbor = FindSpatialNeighbor(swizzledDTid, pos, normal, isMetallic, mr.y, 
        isTransmissive, viewDepth, SPATIAL_SEARCH_RADIUS * scale, rng);

    RWTexture2D<uint2> g_out = ResourceDescriptorHeap[g_local.OutputDescHeapIdx];
    // [-R_max, +R_max]
    int2 mapped = neighbor - (int2)swizzledDTid;
    // [0, 2 * R_max]
    mapped = neighbor.x == UINT16_MAX ? UINT8_MAX : mapped + RPT_Util::SPATIAL_NEIGHBOR_OFFSET;
    g_out[swizzledDTid] = mapped;
}
