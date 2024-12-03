#include "../IndirectLighting_Common.h"
#include "Util.hlsli"
#include "SampleSet.hlsli"

#define THREAD_GROUP_SWIZZLING 1
#define SPATIAL_SEARCH_RADIUS 15

using namespace RPT_Util;

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
        const float2 sampleUV = k_samples[(offset + i) & (SAMPLE_SET_SIZE - 1)];
        float2 rotated = float2(
            dot(sampleUV, float2(cosTheta, -sinTheta)),
            dot(sampleUV, float2(sinTheta, cosTheta)));
        rotated *= radius;
        const int2 samplePosSS = round(float2(DTid) + rotated);

        if (!Math::IsWithinBounds(samplePosSS, renderDim))
            continue;

        if(samplePosSS.x == DTid.x && samplePosSS.y == DTid.y)
            continue;

        const float2 sampleMR = g_metallicRoughness[samplePosSS];
        GBuffer::Flags sampleFlags = GBuffer::DecodeMetallic(sampleMR.x);

        if(sampleFlags.invalid || sampleFlags.emissive)
            continue;
        if(metallic != sampleFlags.metallic)
            continue;
        if(transmissive != sampleFlags.transmissive)
            continue;
        if(abs(sampleMR.y - roughness) > MAX_ROUGHNESS_DIFF_SPATIAL_REUSE)
            continue;

        const float sampleDepth = g_depth[samplePosSS];
        const float3 samplePos = Math::WorldPosFromScreenSpace(samplePosSS, renderDim,
            sampleDepth, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv, 
            g_frame.CurrCameraJitter);
        const float3 sampleNormal = Math::DecodeUnitVector(g_normal[samplePosSS]);

        if (!RPT_Util::PlaneHeuristic(samplePos, normal, pos, viewDepth, 0.01))
            continue;
        if(dot(sampleNormal, normal) < MIN_NORMAL_SIMILARITY_SPATIAL_REUSE)
            continue;

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
    uint2 swizzledGid;

    const uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint2(RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_X, RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_Y),
        g_local.DispatchDimX_NumGroupsInTile & 0xffff, 
        RESTIR_PT_TILE_WIDTH, 
        RESTIR_PT_LOG2_TILE_WIDTH, 
        g_local.DispatchDimX_NumGroupsInTile >> 16,
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
    const float viewDepth = g_depth[swizzledDTid];
    const float3 pos = Math::WorldPosFromScreenSpace(swizzledDTid,
        float2(g_frame.RenderWidth, g_frame.RenderHeight), viewDepth, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv, 
        g_frame.CurrCameraJitter);

    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_normal[swizzledDTid]);

    // const uint16 passIdx = uint16((g_local.Packed >> 12) & 0x3);
    const uint16 passIdx = 0;
    RNG rng = RNG::Init(RNG::PCG3d(uint3(swizzledDTid, g_frame.FrameNum + passIdx)).xy, 
        g_frame.FrameNum);
    const uint16_t scale = (uint16_t)1;

    const int2 neighbor = FindSpatialNeighbor(swizzledDTid, pos, normal, flags.metallic, 
        mr.y, flags.transmissive, viewDepth, SPATIAL_SEARCH_RADIUS * scale, rng);

    // [-R_max, +R_max]
    int2 mapped = neighbor - (int2)swizzledDTid;
    // [SPATIAL_NEIGHBOR_OFFSET - R_max, SPATIAL_NEIGHBOR_OFFSET + R_max]
    mapped = neighbor.x == UINT16_MAX ? UINT8_MAX : mapped + RPT_Util::SPATIAL_NEIGHBOR_OFFSET;

    // Note: SPATIAL_NEIGHBOR_OFFSET must be >= R_max as output texture is unsigned
    RWTexture2D<uint2> g_out = ResourceDescriptorHeap[g_local.OutputDescHeapIdx];
    g_out[swizzledDTid] = mapped;
}
