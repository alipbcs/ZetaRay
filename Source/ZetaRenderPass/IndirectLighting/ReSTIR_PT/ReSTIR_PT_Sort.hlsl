#include "../../Common/Common.hlsli"
#include "Util.hlsli"

#if !defined (TEMPORAL_TO_CURRENT) && !defined (SPATIAL_TO_CURRENT) && !defined (CURRENT_TO_SPATIAL)
#define CURRENT_TO_TEMPORAL
#endif

#define COMPACTION_ONLY 0

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
    return uint2(Gidx & (2 * RESTIR_PT_SORT_GROUP_DIM_X - 1), 
        Gidx >> (LOG_RESTIR_PT_SORT_GROUP_DIM + 1));
}

void WriteOutput(uint2 Gid, uint2 DTid, uint2 mappedGTid, uint result)
{
    // Transpose the thread group index to handle another edge case for
    // thread groups against the right image boundary
    if((Gid.x == (g_local.DispatchDimX - 1)) && (Gid.y != (g_local.DispatchDimY - 1)))
        mappedGTid = mappedGTid.yx;

    uint2 mappedDTid = Gid * GroupDim * 2 + mappedGTid;

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

    uint2 gtID[4];
    gtID[0] = GTid.xy * 2;
    gtID[1] = GTid.xy * 2 + uint2(1, 0);
    gtID[2] = GTid.xy * 2 + uint2(0, 1);
    gtID[3] = GTid.xy * 2 + uint2(1, 1);

    uint2 dtID[4];
    const uint2 groupStart = Gid.xy * GroupDim * 2;

    [unroll]
    for(int i = 0; i < 4; i++)
        dtID[i] = groupStart + gtID[i];

    bool4 skip;
    int2 neighborPixel[4];
    RPT_Util::SHIFT_ERROR error[4];

    [unroll]
    for(int i = 0; i < 4; i++)
    {
        error[i] = FindNeighbor(dtID[i], neighborPixel[i]);
        skip[i] = error[i] != RPT_Util::SHIFT_ERROR::SUCCESS;
    }

    uint4 result = (uint4)error;
    RPT_Util::Reservoir r_x[4];

    [unroll]
    for(int i = 0; i < 4; i++)
        r_x[i] = RPT_Util::Reservoir::Init();

#if defined (TEMPORAL_TO_CURRENT)
    [unroll]
    for(int i = 0; i < 4; i++)
    {
        if(error[i] == RPT_Util::SHIFT_ERROR::SUCCESS)
        {
            r_x[i] = RPT_Util::Reservoir::Load_MetadataX<Texture2D<uint4> >(neighborPixel[i], 
                g_local.Reservoir_A_DescHeapIdx);
        }
    }

#elif defined(CURRENT_TO_TEMPORAL)
    [unroll]
    for(int i = 0; i < 4; i++)
    {
        if(error[i] == RPT_Util::SHIFT_ERROR::SUCCESS)
        {
            r_x[i] = RPT_Util::Reservoir::Load_MetadataX<RWTexture2D<uint4> >(dtID[i], 
                g_local.Reservoir_A_DescHeapIdx);
        }
    }

#elif defined(CURRENT_TO_SPATIAL)
    [unroll]
    for(int i = 0; i < 4; i++)
    {
        if(error[i] == RPT_Util::SHIFT_ERROR::SUCCESS)
        {
            r_x[i] = RPT_Util::Reservoir::Load_MetadataX<Texture2D<uint4> >(dtID[i], 
                g_local.Reservoir_A_DescHeapIdx);
        }
    }

#elif defined(SPATIAL_TO_CURRENT)
    [unroll]
    for(int i = 0; i < 4; i++)
    {
        if(error[i] == RPT_Util::SHIFT_ERROR::SUCCESS)
        {
            r_x[i] = RPT_Util::Reservoir::Load_MetadataX<Texture2D<uint4> >(neighborPixel[i], 
                g_local.Reservoir_A_DescHeapIdx);
        }
    }

#else
#error Undefined shift type
#endif

    [unroll]
    for(int i = 0; i < 4; i++)
    {
        if(r_x[i].rc.Empty())
        {
            result[i] = result[i] | RPT_Util::SHIFT_ERROR::EMPTY;
            skip[i] = true;
        }
    }

    // Handle an edge case where for thread groups at the right and bottom 
    // image boundaries, valid threads may end up outside the screen and 
    // erroneously skippped in the subsequent passes
    const bool againstEdge = (Gid.x == (g_local.DispatchDimX - 1)) ||
        (Gid.y == (g_local.DispatchDimY - 1));
    bool4 edgeCase = false;

    // Make sure "skip" is true to avoid double counting
    [unroll]
    for(int i = 0; i < 4; i++)
    {
        if(skip[i] && againstEdge && (dtID[i].x < g_frame.RenderWidth) && (dtID[i].y < g_frame.RenderHeight))
        {
            result[i] = RPT_Util::SHIFT_ERROR::SUCCESS;
            skip[i] = false;
            edgeCase[i] = true;
        }
    }

    bool4 kEq2;
#if COMPACTION_ONLY == 1
    bool4 kGe3;
#else
    bool4 kEq3;
    bool4 kEq4;
    bool4 kGe5;
#endif

    [unroll]
    for(int i = 0; i < 4; i++)
    {
        kEq2[i] = !skip[i] && (r_x[i].rc.k == 2);
#if COMPACTION_ONLY == 1
        kGe3[i] = !skip[i] && ((r_x[i].rc.k >= 3) || edgeCase[i]);
#else
        kEq3[i] = !skip[i] && (r_x[i].rc.k == 3);
        kEq4[i] = !skip[i] && (r_x[i].rc.k == 4);
        kGe5[i] = !skip[i] && ((r_x[i].rc.k >= 5) || edgeCase[i]);
#endif
    }

    const uint16_t wavekEq2Count = (uint16_t)WaveActiveSum(dot(1, kEq2));
#if COMPACTION_ONLY == 1
    const uint16_t wavekGe3Count = (uint16_t)WaveActiveSum(dot(1, kGe3));
#else
    const uint16_t wavekEq3Count = (uint16_t)WaveActiveSum(dot(1, kEq3));
    const uint16_t wavekEq4Count = (uint16_t)WaveActiveSum(dot(1, kEq4));
    const uint16_t wavekGe5Count = (uint16_t)WaveActiveSum(dot(1, kGe5));
#endif
    const uint16_t waveSkipCount = (uint16_t)WaveActiveSum(dot(1, skip));
    // const uint16_t4 allCounts = uint16_t4(wavekEq2Count, wavekEq3Count, wavekEq4Count, wavekGe5Count);

    uint4 waveOffsets;
    uint skipOffset;

    if(WaveGetLaneIndex() == 0)
    {
        InterlockedAdd(g_count.x, wavekEq2Count, waveOffsets.x);
#if COMPACTION_ONLY == 1
        InterlockedAdd(g_count.y, wavekGe3Count, waveOffsets.y);
#else
        InterlockedAdd(g_count.y, wavekEq3Count, waveOffsets.y);
        InterlockedAdd(g_count.z, wavekEq4Count, waveOffsets.z);
        InterlockedAdd(g_count.w, wavekGe5Count, waveOffsets.w);
#endif
        InterlockedAdd(g_skip, waveSkipCount, skipOffset);
    }

    GroupMemoryBarrierWithGroupSync();

    waveOffsets = WaveReadLaneAt(waveOffsets, 0);
    skipOffset = WaveReadLaneAt(skipOffset, 0);

    const uint kEq2BaseOffset = waveOffsets.x;
#if COMPACTION_ONLY == 1
    const uint kGe3BaseOffset = g_count.x + waveOffsets.y;
    const uint skipBaseOffset = g_count.x + g_count.y + skipOffset;
#else
    const uint kEq3BaseOffset = g_count.x + waveOffsets.y;
    const uint kEq4BaseOffset = g_count.x + g_count.y + waveOffsets.z;
    const uint kGe5BaseOffset = g_count.x + g_count.y + g_count.z + waveOffsets.w;
    const uint skipBaseOffset = g_count.x + g_count.y + g_count.z + g_count.w + skipOffset;
#endif

    uint lanekEq2Idx = WavePrefixSum(dot(1, kEq2));
#if COMPACTION_ONLY == 1
    uint lanekGe3Idx = WavePrefixSum(dot(1, kGe3));
#else
    uint lanekEq3Idx = WavePrefixSum(dot(1, kEq3));
    uint lanekEq4Idx = WavePrefixSum(dot(1, kEq4));
    uint lanekGe5Idx = WavePrefixSum(dot(1, kGe5));
#endif
    uint laneSkipIdx = WavePrefixSum(dot(1, skip));

    // One-to-one mapping for the very last thread group
    if(Gid.x == (g_local.DispatchDimX - 1) && Gid.y == (g_local.DispatchDimY - 1))
    {
        [unroll]
        for(int i = 0; i < 4; i++)
        {
            WriteOutput(Gid.xy, dtID[i], gtID[i], result[i]);
            return;
        }
    }

    [unroll]
    for(int i = 0; i < 4; i++)
    {
        if(kEq2[i])
        {
            uint prefixSumIn2x2 = 0;
            for(int j = 0; j < i; j++)
                prefixSumIn2x2 += kEq2[j];

            uint2 mappedGTid = GroupIndexToGTid(kEq2BaseOffset + lanekEq2Idx + prefixSumIn2x2);
            WriteOutput(Gid.xy, dtID[i], mappedGTid, result[i]);
        }
#if COMPACTION_ONLY == 1
        else if(kGe3[i])
        {
            uint prefixSumIn2x2 = 0;
            for(int j = 0; j < i; j++)
                prefixSumIn2x2 += kGe3[j];

            uint2 mappedGTid = GroupIndexToGTid(kGe3BaseOffset + lanekGe3Idx + prefixSumIn2x2);
            WriteOutput(Gid.xy, dtID[i], mappedGTid, result[i]);
        }
#else
        else if(kEq3[i])
        {
            uint prefixSumIn2x2 = 0;
            for(int j = 0; j < i; j++)
                prefixSumIn2x2 += kEq3[j];

            uint2 mappedGTid = GroupIndexToGTid(kEq3BaseOffset + lanekEq3Idx + prefixSumIn2x2);
            WriteOutput(Gid.xy, dtID[i], mappedGTid, result[i]);
        }
        else if(kEq4[i])
        {
            uint prefixSumIn2x2 = 0;
            for(int j = 0; j < i; j++)
                prefixSumIn2x2 += kEq4[j];

            uint2 mappedGTid = GroupIndexToGTid(kEq4BaseOffset + lanekEq4Idx + prefixSumIn2x2);
            WriteOutput(Gid.xy, dtID[i], mappedGTid, result[i]);
        }
        else if(kGe5[i])
        {
            uint prefixSumIn2x2 = 0;
            for(int j = 0; j < i; j++)
                prefixSumIn2x2 += kGe5[j];

            uint2 mappedGTid = GroupIndexToGTid(kGe5BaseOffset + lanekGe5Idx + prefixSumIn2x2);
            WriteOutput(Gid.xy, dtID[i], mappedGTid, result[i]);
        }
#endif
        else if(skip[i])
        {
            uint prefixSumIn2x2 = 0;
            for(int j = 0; j < i; j++)
                prefixSumIn2x2 += skip[j];

            uint2 mappedGTid = GroupIndexToGTid(skipBaseOffset + laneSkipIdx + prefixSumIn2x2);
            WriteOutput(Gid.xy, dtID[i], mappedGTid, result[i]);
        }
    }
}
