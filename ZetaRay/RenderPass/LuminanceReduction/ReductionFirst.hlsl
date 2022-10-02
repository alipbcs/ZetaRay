#include "Reduction_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/FrameConstants.h"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbReduction> g_local : register(b1);
StructuredBuffer<float> g_intermediate : register(t0);
RWStructuredBuffer<float> g_out : register(u0);

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

#define NUM_LANES_IN_WAVE 32 
groupshared float g_sum[NUM_LANES_IN_WAVE];

[WaveSize(NUM_LANES_IN_WAVE)]
[numthreads(THREAD_GROUP_SIZE_X_FIRST, THREAD_GROUP_SIZE_Y_FIRST, THREAD_GROUP_SIZE_Z_FIRST)]
void main(uint3 DTid : SV_DispatchThreadID, uint Gidx : SV_GroupIndex, uint3 Gid : SV_GroupID)
{
	Texture2D<float4> g_input = ResourceDescriptorHeap[g_local.InputDescHeapIdx];
	const uint2 textureDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);

	float logLum = 0.0f;
	const bool isInBounds = DTid.x < textureDim.x && DTid.y < textureDim.y;
//	const uint numInBound = WaveActiveCountBits(isInBounds);

	if (isInBounds)
	{
		const float3 val = g_input[DTid.xy].rgb;
		const float lum = max(Common::LuminanceFromLinearRGB(val), 1e-5f);
		logLum = log(lum);
	}

	float sum = WaveActiveSum(logLum);
	uint wave = Gidx >> 5;

	if (WaveIsFirstLane())
	{
		g_sum[wave] = sum;
	}

	GroupMemoryBarrierWithGroupSync();

	float blockAvg = 0.0f;

	if(wave == 0)
	{
		blockAvg = g_sum[WaveGetLaneIndex()];
		blockAvg = WaveActiveSum(blockAvg);
	}

	if (Gidx == 0)
	{
//		blockAvg /= numInBound;
		g_out[Gid.y * g_local.DispatchDimXFirstPass + Gid.x] = blockAvg / (THREAD_GROUP_SIZE_X_FIRST * THREAD_GROUP_SIZE_Y_FIRST);
	}
}