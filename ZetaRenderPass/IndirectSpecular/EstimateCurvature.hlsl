#include "Reservoir_Specular.hlsli"
#include "../Common/Common.hlsli"
#include "../Common/FrameConstants.h"
#include "../../ZetaCore/Core/Material.h"
#include "../Common/GBuffers.hlsli"
#include "../Common/BRDF.hlsli"
#include "../Common/Sampling.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/RT.hlsli"
#include "../Common/VolumetricLighting.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbCurvature> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Globals
//--------------------------------------------------------------------------------------

groupshared half3 g_normalDepth[RGI_SPEC_TEMPORAL_GROUP_DIM_Y][RGI_SPEC_TEMPORAL_GROUP_DIM_X];
groupshared float3 g_pos[RGI_SPEC_TEMPORAL_GROUP_DIM_Y][RGI_SPEC_TEMPORAL_GROUP_DIM_X];

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

// Ref: T. Akenine-Moller, J. Nilsson, M. Andersson, C. Barre-Brisebois, R. Toth 
// and T. Karras, "Texture Level of Detail Strategies for Real-Time Ray Tracing," in 
// Ray Tracing Gems 1, 2019.
float EstimateLocalCurvature(float3 normal, float3 pos, float linearDepth, int2 GTid)
{
	const float depthX = (GTid.x & 0x1) == 0 ? g_normalDepth[GTid.y][GTid.x + 1].z : g_normalDepth[GTid.y][GTid.x - 1].z;
	const float depthY = (GTid.y & 0x1) == 0 ? g_normalDepth[GTid.y + 1][GTid.x].z : g_normalDepth[GTid.y - 1][GTid.x].z;
	
	const float maxDepthDiscontinuity = 0.005f;
	const bool invalidX = abs(linearDepth - depthX) >= maxDepthDiscontinuity * linearDepth;
	const bool invalidY = abs(linearDepth - depthY) >= maxDepthDiscontinuity * linearDepth;
	
	const float3 normalX = (GTid.x & 0x1) == 0 ? 
		Math::Encoding::DecodeUnitNormal(g_normalDepth[GTid.y][GTid.x + 1].xy) : 
		Math::Encoding::DecodeUnitNormal(g_normalDepth[GTid.y][GTid.x - 1].xy);
	const float3 normalY = (GTid.y & 0x1) == 0 ? 
		Math::Encoding::DecodeUnitNormal(g_normalDepth[GTid.y + 1][GTid.x].xy) : 
		Math::Encoding::DecodeUnitNormal(g_normalDepth[GTid.y - 1][GTid.x].xy);
	const float3 normalddx = (GTid.x & 0x1) == 0 ? normalX - normal : normal - normalX;
	const float3 normalddy = (GTid.y & 0x1) == 0 ? normalY - normal : normal - normalY;

	const float3 posX = (GTid.x & 0x1) == 0 ? g_pos[GTid.y][GTid.x + 1] : g_pos[GTid.y][GTid.x - 1];
	const float3 posY = (GTid.y & 0x1) == 0 ? g_pos[GTid.y + 1][GTid.x] : g_pos[GTid.y - 1][GTid.x];
	const float3 posddx = (GTid.x & 0x1) == 0 ? posX - pos : pos - posX;
	const float3 posddy = (GTid.y & 0x1) == 0 ? posY - pos : pos - posY;
	
	const float phi = sqrt((invalidX ? 0 : dot(normalddx, normalddx)) + (invalidY ? 0 : dot(normalddy, normalddy)));
	const float s = sign((invalidX ? 0 : dot(posddx, normalddx)) + (invalidY ? 0 : dot(posddy, normalddy)));
	const float k = 2.0f * phi * s;
	
	return k;
}

float2 FindNearestPrevUV(int2 GTid, float2 prevUV, float linearDepth)
{
	g_pos[GTid.y][GTid.x] = float3(prevUV, linearDepth);
	
	GroupMemoryBarrierWithGroupSync();

	float3 left = g_pos[GTid.y][max(GTid.x - 1, 0)];
	float3 right = g_pos[GTid.y][min(GTid.x + 1, RGI_SPEC_TEMPORAL_GROUP_DIM_X - 1)];
	float3 up = g_pos[max(GTid.y - 1, 0)][GTid.x];
	float3 down = g_pos[min(GTid.y + 1, RGI_SPEC_TEMPORAL_GROUP_DIM_Y - 1)][GTid.x];
	
	float3 closest = left;
	closest = right.z < closest.z ? right : closest;
	closest = up.z < closest.z ? up : closest;
	closest = down.z < closest.z ? down : closest;
	
	return closest.xy;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(ESTIMATE_CURVATURE_GROUP_DIM_X, ESTIMATE_CURVATURE_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
	if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
		return;

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];

	if(depth == 0)
		return;
	
	// reconstruct position from depth buffer
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 posW = Math::Transform::WorldPosFromScreenSpace(DTid.xy,
		renderDim,
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	// shading normal
	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const half2 encodedNormal = g_normal[DTid.xy];
	const float3 normal = Math::Encoding::DecodeUnitNormal(encodedNormal);
	
	g_normalDepth[GTid.y][GTid.x] = half3(encodedNormal, linearDepth);
	g_pos[GTid.y][GTid.x] = posW;

	GroupMemoryBarrierWithGroupSync();
	
	const float k = EstimateLocalCurvature(normal, posW, linearDepth, GTid.xy);
	
	RWTexture2D<float> g_curvature = ResourceDescriptorHeap[g_local.OutputUAVDescHeapIdx];
	g_curvature[DTid.xy] = k;
}