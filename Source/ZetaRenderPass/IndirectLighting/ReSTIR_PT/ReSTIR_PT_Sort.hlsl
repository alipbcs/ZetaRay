#include "../../Common/Common.hlsli"
#include "Util.hlsli"

#define LOG_THREAD_GROUP_DIM_X 5

#if !defined (TEMPORAL_TO_CURRENT) && !defined (SPATIAL_TO_CURRENT) && !defined (CURRENT_TO_SPATIAL)
#define CURRENT_TO_TEMPORAL
#endif

static const uint16_t2 GroupDim = uint16_t2(RESTIR_PT_SORT_GROUP_DIM_X, RESTIR_PT_SORT_GROUP_DIM_Y);

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_ReSTIR_PT_Sort> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Utility Functions
//--------------------------------------------------------------------------------------

RPT_Util::SHIFT_ERROR FindNeighbor(uint2 DTid, out int2 neighborPixel)
{
    if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
        return RPT_Util::SHIFT_ERROR::INVALID_PIXEL;

    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float m = g_metallicRoughness[DTid].x;
    GBuffer::Flags flags = GBuffer::DecodeMetallic(m);

    if (flags.invalid || flags.emissive)
        return RPT_Util::SHIFT_ERROR::INVALID_PIXEL;

#if defined (TEMPORAL_TO_CURRENT)
    GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::MOTION_VECTOR];
    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float2 motionVec = g_motionVector[DTid];
    const float2 currUV = (DTid + 0.5f) / renderDim;
    const float2 prevUV = currUV - motionVec;
    neighborPixel = prevUV * renderDim;

    if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
        return RPT_Util::SHIFT_ERROR::NOT_FOUND;

#elif defined(SPATIAL_TO_CURRENT)
    Texture2D<uint2> g_neighbor = ResourceDescriptorHeap[g_local.SpatialNeighborHeapIdx];
    neighborPixel = (int2)g_neighbor[DTid];

    if(neighborPixel.x == UINT8_MAX) 
        return RPT_Util::SHIFT_ERROR::NOT_FOUND;

    neighborPixel -= RPT_Util::SPATIAL_NEIGHBOR_OFFSET;
    neighborPixel += DTid;

#endif

    return RPT_Util::SHIFT_ERROR::SUCCESS;
}

uint2 GroupIndexToGTid(uint Gidx)
{
    return uint2(Gidx & (RESTIR_PT_SORT_GROUP_DIM_X - 1), Gidx >> LOG_THREAD_GROUP_DIM_X);
}

void WriteOutput(uint2 Gid, uint2 DTid, uint2 mappedGTid, uint result)
{
    // Transpose the thread group index to handle another edge case for
    // thread groups against the right image boundary
    if((Gid.x == (g_local.DispatchDimX - 1)) && (Gid.y != (g_local.DispatchDimY - 1)))
        mappedGTid = mappedGTid.yx;

    uint2 mappedDTid = Gid * GroupDim + mappedGTid;

#if defined (TEMPORAL_TO_CURRENT)
    // Reconnect_TtC still needs to run even if motion vector is invalid and spatial is disabled
    uint flags = IS_CB_FLAG_SET(CB_IND_FLAGS::SPATIAL_RESAMPLE) ?
        RPT_Util::SHIFT_ERROR::INVALID_PIXEL | RPT_Util::SHIFT_ERROR::NOT_FOUND :
        RPT_Util::SHIFT_ERROR::INVALID_PIXEL;
    uint error = result & flags;
#elif defined (SPATIAL_TO_CURRENT)
    uint error = result & RPT_Util::SHIFT_ERROR::INVALID_PIXEL;
#elif defined (CURRENT_TO_TEMPORAL) || defined (CURRENT_TO_SPATIAL)
    uint error = result & (RPT_Util::SHIFT_ERROR::INVALID_PIXEL | RPT_Util::SHIFT_ERROR::EMPTY);
#endif

    if (mappedDTid.x < g_frame.RenderWidth && mappedDTid.y < g_frame.RenderHeight)
        RPT_Util::EncodeSorted(DTid, mappedDTid, g_local.MapDescHeapIdx, error);
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

// (k = 2, k = 3, k = 4, k >= 5)
groupshared uint4 g_count;
// No reconnection 
groupshared uint g_skip;

[numthreads(RESTIR_PT_SORT_GROUP_DIM_X, RESTIR_PT_SORT_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
    if(Gidx == 0)
    {
        g_count = 0.xxxx;
        g_skip = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    int2 neighborPixel;
    RPT_Util::SHIFT_ERROR error = FindNeighbor(DTid.xy, neighborPixel);
    bool skip = error != RPT_Util::SHIFT_ERROR::SUCCESS;
    uint result = (uint)error;
    RPT_Util::Reservoir r_x = RPT_Util::Reservoir::Init();

#if defined (TEMPORAL_TO_CURRENT)
    if(error == RPT_Util::SHIFT_ERROR::SUCCESS)
    {
        r_x = RPT_Util::Reservoir::Load_Metadata<Texture2D<uint2> >(neighborPixel, 
            g_local.Reservoir_A_DescHeapIdx);
    }

#elif defined(CURRENT_TO_TEMPORAL)
    if(error == RPT_Util::SHIFT_ERROR::SUCCESS)
    {
        r_x = RPT_Util::Reservoir::Load_Metadata<RWTexture2D<uint2> >(DTid.xy, 
            g_local.Reservoir_A_DescHeapIdx);
    }

#elif defined(CURRENT_TO_SPATIAL)
    if(error == RPT_Util::SHIFT_ERROR::SUCCESS)
    {
        r_x = RPT_Util::Reservoir::Load_Metadata<Texture2D<uint2> >(DTid.xy, 
            g_local.Reservoir_A_DescHeapIdx);
    }

#elif defined(SPATIAL_TO_CURRENT)
    if(error == RPT_Util::SHIFT_ERROR::SUCCESS)
    {
        r_x = RPT_Util::Reservoir::Load_Metadata<Texture2D<uint2> >(neighborPixel, 
            g_local.Reservoir_A_DescHeapIdx);
    }

#else
#error Undefined shift type
#endif

    if(r_x.rc.Empty())
    {
        result = result | RPT_Util::SHIFT_ERROR::EMPTY;
        skip = true;
    }

    // Handle an edge case where for thread groups at the right and bottom 
    // image boundaries, valid threads may end up outside the screen and 
    // erroneously skippped in the subsequent passes
    const bool againstEdge = (Gid.x == (g_local.DispatchDimX - 1)) ||
        (Gid.y == (g_local.DispatchDimY - 1));
    bool edgeCase = false;

    // Make sure "skip" is true to avoid double counting
    if(skip && againstEdge && (DTid.x < g_frame.RenderWidth) && (DTid.y < g_frame.RenderHeight))
    {
        result = RPT_Util::SHIFT_ERROR::SUCCESS;
        skip = false;
        edgeCase = true;
    }

    const bool kEq2 = !skip && (r_x.rc.k == 2);
    const bool kEq3 = !skip && (r_x.rc.k == 3);
    const bool kEq4 = !skip && (r_x.rc.k == 4);
    const bool kGe5 = !skip && ((r_x.rc.k >= 5) || edgeCase);

    const uint16_t wavekEq2Count = (uint16_t)WaveActiveCountBits(kEq2);
    const uint16_t wavekEq3Count = (uint16_t)WaveActiveCountBits(kEq3);
    const uint16_t wavekEq4Count = (uint16_t)WaveActiveCountBits(kEq4);
    const uint16_t wavekGe5Count = (uint16_t)WaveActiveCountBits(kGe5);
    const uint16_t waveSkipCount = (uint16_t)WaveActiveCountBits(skip);
    const uint16_t4 allCounts = uint16_t4(wavekEq2Count, wavekEq3Count, wavekEq4Count, wavekGe5Count);

    const uint16_t laneIdx = (uint16_t)WaveGetLaneIndex();
    uint4 waveOffsets;
    uint skipOffset;

    if(laneIdx == 0)
    {
        InterlockedAdd(g_count.x, wavekEq2Count, waveOffsets.x);
        InterlockedAdd(g_count.y, wavekEq3Count, waveOffsets.y);
        InterlockedAdd(g_count.z, wavekEq4Count, waveOffsets.z);
        InterlockedAdd(g_count.w, wavekGe5Count, waveOffsets.w);
        InterlockedAdd(g_skip, waveSkipCount, skipOffset);
    }

    GroupMemoryBarrierWithGroupSync();

    waveOffsets = WaveReadLaneAt(waveOffsets, 0);
    skipOffset = WaveReadLaneAt(skipOffset, 0);

    const uint kEq2BaseOffset = waveOffsets.x;
    const uint kEq3BaseOffset = g_count.x + waveOffsets.y;
    const uint kEq4BaseOffset = g_count.x + g_count.y + waveOffsets.z;
    const uint kGe5BaseOffset = g_count.x + g_count.y + g_count.z + waveOffsets.w;
    const uint skipBaseOffset = g_count.x + g_count.y + g_count.z + g_count.w + skipOffset;

    uint lanekEq2Idx = WavePrefixCountBits(kEq2);
    uint lanekEq3Idx = WavePrefixCountBits(kEq3);
    uint lanekEq4Idx = WavePrefixCountBits(kEq4);
    uint lanekGe5Idx = WavePrefixCountBits(kGe5);
    uint laneSkipIdx = WavePrefixCountBits(skip);

    // One-to-one mapping for the very last thread group
    if(Gid.x == (g_local.DispatchDimX - 1) && Gid.y == (g_local.DispatchDimY - 1))
    {
        WriteOutput(Gid.xy, DTid.xy, GTid.xy, result);
        return;
    }

    if(kEq2)
    {
        uint2 mappedGTid = GroupIndexToGTid(kEq2BaseOffset + lanekEq2Idx);
        WriteOutput(Gid.xy, DTid.xy, mappedGTid, result);
    }
    else if(kEq3)
    {
        uint2 mappedGTid = GroupIndexToGTid(kEq3BaseOffset + lanekEq3Idx);
        WriteOutput(Gid.xy, DTid.xy, mappedGTid, result);
    }
    else if(kEq4)
    {
        uint2 mappedGTid = GroupIndexToGTid(kEq4BaseOffset + lanekEq4Idx);
        WriteOutput(Gid.xy, DTid.xy, mappedGTid, result);
    }
    else if(kGe5)
    {
        uint2 mappedGTid = GroupIndexToGTid(kGe5BaseOffset + lanekGe5Idx);
        WriteOutput(Gid.xy, DTid.xy, mappedGTid, result);
    }
    else if(skip)
    {
        uint2 mappedGTid = GroupIndexToGTid(skipBaseOffset + laneSkipIdx);
        WriteOutput(Gid.xy, DTid.xy, mappedGTid, result);
    }
}
