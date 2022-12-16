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
#define NUM_WAVES (THREAD_GROUP_SIZE_X_SECOND / NUM_LANES_IN_WAVE) 
groupshared float g_sum[NUM_WAVES];

[WaveSize(NUM_LANES_IN_WAVE)]
[numthreads(THREAD_GROUP_SIZE_X_SECOND, THREAD_GROUP_SIZE_Y_SECOND, THREAD_GROUP_SIZE_Z_SECOND)]
void main(uint3 DTid : SV_DispatchThreadID, uint Gidx : SV_GroupIndex, uint3 Gid : SV_GroupID)
{
	const uint2 textureDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	uint loadAddr = DTid.x * g_local.NumToProcessPerThreadSecondPass;

	float sum = 0.0f;

	for (uint i = 0; i < g_local.NumToProcessPerThreadSecondPass; i++)
		sum += loadAddr + i < g_local.NumGroupsInFirstPass ? g_intermediate[loadAddr + i] : 0.0f;

	const uint wave = Gidx >> 5;
	float waveSum = WaveActiveSum(sum);

	if (WaveIsFirstLane())
		g_sum[wave] = waveSum;

	GroupMemoryBarrierWithGroupSync();

	// reduce the last 32 elements
	if (wave == 0)
	{
		float summed = g_sum[WaveGetLaneIndex()];
		float sumAll = WaveActiveSum(summed);

		if (Gidx == 0)
		{
			g_out[0] = exp(sumAll / (textureDim.x * textureDim.y));
		}
	}
}