#include "SunShadow_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/RT.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/Sampling.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbSunShadow> g_local : register(b1);
RaytracingAccelerationStructure g_sceneBVH : register(t0);
ByteAddressBuffer g_owenScrambledSobolSeq : register(t1);
ByteAddressBuffer g_scramblingTile : register(t2);
ByteAddressBuffer g_rankingTile : register(t3);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

bool EvaluateVisibility(float3 pos, float3 wi, float3 normal, float linearDepth)
{
	if(wi.y < 0)
		return false;
	
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
			 RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
			 RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
			 RAY_FLAG_CULL_NON_OPAQUE> rayQuery;
	
	float tMin;
	const float3 adjustedOrigin = RT::OffsetRay(pos, normal, linearDepth, tMin);
	
	RayDesc ray;
	ray.Origin = adjustedOrigin;
	ray.TMin = tMin;
	ray.TMax = FLT_MAX;
	ray.Direction = wi;
	
	// initialize
	rayQuery.TraceRayInline(g_sceneBVH, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);
	
	// traversal
	rayQuery.Proceed();

	// light source is occluded
	if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		return false;
		
	return true;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[WaveSize(32)]
[numthreads(SUN_SHADOW_THREAD_GROUP_SIZE_X, SUN_SHADOW_THREAD_GROUP_SIZE_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID)
{
	if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
		return;

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];
	
	if(depth == 0)
		return;
	
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);	
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	float3 posW = Math::Transform::WorldPosFromScreenSpace(DTid.xy,
		renderDim,
		linearDepth, 
		g_frame.TanHalfFOV, 
		g_frame.AspectRatio, 
		g_frame.CurrViewInv,
		g_frame.CurrProjectionJitter);

	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitVector(g_normal[DTid.xy]);

	float3 wi = -g_frame.SunDir;
	
	// sample the cone subtended by sun	
	if (g_local.SoftShadows)
	{
		const uint sampleIdx = g_frame.FrameNum & 31;
		const float u0 = Sampling::BlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			DTid.x, DTid.y, sampleIdx, 2);
		const float u1 = Sampling::BlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			DTid.x, DTid.y, sampleIdx, 3);
	
		float pdf = 1.0f;
		float3 wi = Sampling::UniformSampleCone(float2(u0, u1), g_frame.SunCosAngularRadius, pdf);
		
		float3 T;
		float3 B;
		Math::Transform::revisedONB(normal, T, B);
		wi = wi.x * T + wi.y * B + wi.z * normal;
	}
	
	const bool isUnoccluded = EvaluateVisibility(posW, wi, normal, linearDepth);
	const uint laneMask = (isUnoccluded << WaveGetLaneIndex());
	const uint ret = WaveActiveBitOr(laneMask);
	
	if (WaveIsFirstLane())
	{
		RWTexture2D<uint> g_shadowMask = ResourceDescriptorHeap[g_local.OutShadowMaskDescHeapIdx];
		g_shadowMask[Gid.xy] = ret;
	}
	
//	return float4(L_i, 1.0f);
//	return float4(0.42f, 0.0f, 0.0f, 1.0f);
//	return float4(tr, 1.0f);
//	return float4(shadowFactor, shadowFactor, shadowFactor, 1.0f);
}
