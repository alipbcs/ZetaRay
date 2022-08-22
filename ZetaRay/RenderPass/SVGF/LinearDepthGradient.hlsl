//	Ref: "D3D12RaytracingRealTimeDenoisedAmbientOcclusion" sample From Microsoft:
//	https://github.com/microsoft/DirectX-Graphics-Samples

#include "../Common/Common.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "LinearDepthGradient_Common.h"

#define USE_SHARED_MEM 0

//--------------------------------------------------------------------------------------
// Resources
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0, space0);
ConstantBuffer<cbLinearDepthGrad> g_local : register(b1, space0);

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

#if USE_SHARED_MEM == 1
groupshared float g_shared[LINEAR_DEPTH_GRAD_THREAD_GROUP_SIZE_X + 2][LINEAR_DEPTH_GRAD_THREAD_GROUP_SIZE_Y + 2];
#endif

// Compute forward and backward differences to estimate gradient of linear depth w.r.t. screen-space pos.
// Main usage is to estimate the tangent plane at a given surface point (x0, y0, f(x0, y0)) where 
// f(x, y) returns the linear depth.
//		z1 ~ f(x0, y0) + dfdx(x0, y0) * (x1 - x0) + dfdy(x0, y0) * (y1 - y0)
[numthreads(LINEAR_DEPTH_GRAD_THREAD_GROUP_SIZE_X, LINEAR_DEPTH_GRAD_THREAD_GROUP_SIZE_Y, LINEAR_DEPTH_GRAD_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID)
{
	const uint2 gbuffDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	if (!IsInRange(DTid.xy, gbuffDim))
		return;
	
	//				(i, j - 1)
	//	(i - 1, j)		(i, j)		(i + 1, j)
	//				(i, j + 1)
	
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float z = ComputeLinearDepthReverseZ(g_depth[DTid.xy], g_frame.CameraNear);
	
#if USE_SHARED_MEM == 1
	
	// cache all the depth values that are needed by this group
	g_shared[GTid.y + 1][GTid.x + 1] = z;
	
	// column to the left of this group
	if (GTid.x == 0)
	{
		const int2 addr = int2(max((int)DTid.x - 1, 0), DTid.y);
		const float zLeft = ComputeLinearDepthReverseZ(g_depth[addr], g_frame.CameraNear);
		g_shared[GTid.y + 1][0] = zLeft;
	}
	// column to the right of this group
	else if (GTid.x == LINEAR_DEPTH_GRAD_THREAD_GROUP_SIZE_X - 1)
	{
		const int2 addr = int2(min(DTid.x + 1, gbuffDim.x - 1), DTid.y);
		const float zRight = ComputeLinearDepthReverseZ(g_depth[addr], g_frame.CameraNear);
		g_shared[GTid.y + 1][LINEAR_DEPTH_GRAD_THREAD_GROUP_SIZE_X + 1] = zRight;
	}

	// row above this group
	if (GTid.y == 0)
	{
		const int2 addr = int2(DTid.x, max((int) DTid.y - 1, 0));
		const float zTop = ComputeLinearDepthReverseZ(g_depth[addr], g_frame.CameraNear);
		g_shared[0][GTid.x + 1] = zTop;
	}
	// row below this group
	else if (GTid.y == LINEAR_DEPTH_GRAD_THREAD_GROUP_SIZE_Y - 1)
	{
		const int2 addr = int2(DTid.x, min(DTid.y + 1, gbuffDim.y - 1));
		const float zBottom = ComputeLinearDepthReverseZ(g_depth[addr], g_frame.CameraNear);
		g_shared[LINEAR_DEPTH_GRAD_THREAD_GROUP_SIZE_Y + 1][GTid.x + 1] = zBottom;
	}
	
	GroupMemoryBarrierWithGroupSync();
		
	const float zL = g_shared[GTid.y + 1][GTid.x];
	const float zR = g_shared[GTid.y + 1][GTid.x + 2];
	const float zB = g_shared[GTid.y + 2][GTid.x + 1];
	const float zT = g_shared[GTid.y][GTid.x + 1];
#else
	const float zL = ComputeLinearDepthReverseZ(g_depth[int2(max((int) DTid.x - 1, 0), DTid.y)], g_frame.CameraNear);
	const float zR = ComputeLinearDepthReverseZ(g_depth[int2(min(DTid.x + 1, gbuffDim.x - 1), DTid.y)], g_frame.CameraNear);
	const float zB = ComputeLinearDepthReverseZ(g_depth[int2(DTid.x, min(DTid.y + 1, gbuffDim.y - 1))], g_frame.CameraNear);
	const float zT = ComputeLinearDepthReverseZ(g_depth[int2(DTid.x, max((int) DTid.y - 1, 0))], g_frame.CameraNear);
#endif
	
	// min(abs(forawrd difference), abs(backward difference))
	const float dzdx = abs(zR - z) < abs(z - zL) ? (zR - z) : (z - zL);
	const float dzdy = abs(zB - z) < abs(z - zT) ? (zB - z) : (z - zT);

	// TODO fix comment
	// clamp the result to [-1, 1]: prevents going over surface boundaries on thin geometry,
	// which can lead to pixels on different surfaces getting blended together during temporal 
	// accumulation or spatial filter
	RWTexture2D<float2> g_output = ResourceDescriptorHeap[g_local.OutputDescHeapIdx];
	//g_output[DTid.xy] = clamp(float2(dzdx, dzdy), -1.0f.xx, 1.0f.xx);
	//g_output[DTid.xy] = float2(abs(zB - z), abs(zT - z));
	g_output[DTid.xy] = float2(dzdx, dzdy);
}
