// Ref: https://alextardif.com/HistogramLuminance.html

#include "AutoExposure_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"

#define EPS 1e-4f

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbAutoExposureHist> g_local : register(b1);
RWByteAddressBuffer g_hist : register(u0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

uint CalculateeBin(uint2 DTid)
{
    // Can't early exit for out-of-screen threads
    if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
        return uint(-1);

    Texture2D<half4> g_input = ResourceDescriptorHeap[g_local.InputDescHeapIdx];
    const float3 color = g_input[DTid].rgb;
    const float lum = Math::Luminance(color);

    if (lum <= EPS)
        return 0;

    float t = saturate((lum - g_local.MinLum) / g_local.LumRange);
    t = pow(t, g_local.LumMapExp);
    uint bin = (uint) (t * (HIST_BIN_COUNT - 2)) + 1;

    return bin;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

groupshared uint g_binSize[HIST_BIN_COUNT];

[numthreads(THREAD_GROUP_SIZE_HIST_X, THREAD_GROUP_SIZE_HIST_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint Gidx : SV_GroupIndex)
{
    g_binSize[Gidx] = 0;

    GroupMemoryBarrierWithGroupSync();

    const uint bin = CalculateeBin(DTid.xy);
    const uint4 waveBinMask = WaveMatch(bin);
    //const uint binSizeInWave = WaveMultiPrefixCountBits(true, waveBinMask);
    const uint binSizeInWave = dot(1, countbits(waveBinMask));

    // Add number of preceding bits (multiples of 32 bits). When mask is zero, firstbitlow 
    // returns -1 and the following logical or doesn't change it (x | -1 = -1).
    const uint4 firstSetLanes = firstbitlow(waveBinMask) | uint4(0x0, 0x20, 0x40, 0x60);
    // min between uint(-1) and anything else returns the latter
    const uint writerLane = min(min(min(firstSetLanes.x, firstSetLanes.y), firstSetLanes.z), firstSetLanes.w);

    // Make sure threads outside the screen are skipped
    if (writerLane == WaveGetLaneIndex() && bin != uint(-1))
        InterlockedAdd(g_binSize[bin], binSizeInWave);

    GroupMemoryBarrierWithGroupSync();

    const uint byteOffsetForBin = Gidx * sizeof(uint);
    g_hist.InterlockedAdd(byteOffsetForBin, g_binSize[Gidx]);
}