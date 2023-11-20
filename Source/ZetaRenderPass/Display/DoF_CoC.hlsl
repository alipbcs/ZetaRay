#include "Display_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/Common.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/GBuffers.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbDoF_Gather> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper Functions
//--------------------------------------------------------------------------------------

float CoC(float linearDepth)
{
	float f = g_local.FocalLength / 1000.0f; // convert from mm to meters
	float numerator = f * f * abs(linearDepth - g_local.FocusDepth);
	float denom = g_local.FStop * linearDepth * (g_local.FocusDepth - f);
	return abs(numerator / denom) * 1000;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(DOF_CoC_THREAD_GROUP_DIM_X, DOF_CoC_THREAD_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex)
{
	if (DTid.x >= g_frame.DisplayWidth || DTid.y >= g_frame.DisplayHeight)
		return;

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];
	const float coc = CoC(depth);
	
	RWTexture2D<float> g_coc = ResourceDescriptorHeap[g_local.CoCUavDescHeapIdx];
	g_coc[DTid.xy] = coc;
}