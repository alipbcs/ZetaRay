// Ref: https://alextardif.com/HistogramLuminance.html

#include "AutoExposure_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"

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
	// can't early exit for out-of-screen threads
	if (!Math::IsWithinBoundsExc(DTid.xy, uint2(g_frame.RenderWidth, g_frame.RenderHeight)))
		return -1;
	
	Texture2D<half4> g_input = ResourceDescriptorHeap[g_local.InputDescHeapIdx];
	const float3 color = g_input[DTid].rgb;
	const float lum = Math::Color::LuminanceFromLinearRGB(color);
	
	if(lum <= g_local.Eps)
		return 0;
	
	float t = saturate((lum - g_local.MinLum) / g_local.LumRange);
	t = pow(t, g_local.LumMappingExp);
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
	
	// add in the offset corresponding to number of prior uints
	// firstbitlow returns -1 if not bit is set, so logical or with -1 doesn't change it from -1
	const uint4 firstSetLanes = firstbitlow(waveBinMask) | uint4(0x0, 0x20, 0x40, 0x60);
	// min between uint(-1) and anything else returns the latter
	const uint writerLane = min(min(min(firstSetLanes.x, firstSetLanes.y), firstSetLanes.z), firstSetLanes.w);
	
	// make sure threads that are outside the screen are skipped
	if (writerLane == WaveGetLaneIndex() && bin != -1)
		InterlockedAdd(g_binSize[bin], binSizeInWave);
	
	GroupMemoryBarrierWithGroupSync();

	const uint byteOffsetForBin = Gidx * sizeof(uint);
	g_hist.InterlockedAdd(byteOffsetForBin, g_binSize[Gidx]);
}