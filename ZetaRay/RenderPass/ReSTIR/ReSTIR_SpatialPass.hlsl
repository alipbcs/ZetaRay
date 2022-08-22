#include "../Common/RT.hlsli"
#include "../Common/LightSourceFuncs.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "ReSTIR_Common.h"
#include "ReSTIR_Funcs.hlsli"

//--------------------------------------------------------------------------------------
// Resources
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0, space0);
ConstantBuffer<cbReSTIR> g_local : register(b1, space0);
RaytracingAccelerationStructure g_sceneBVH : register(t0, space0);
StructuredBuffer<EnvMapPatch> g_envMapPatches : register(t1, space0);
StructuredBuffer<Reservoir> g_currReservoir : register(t2, space0);
StructuredBuffer<AliasTableEntry> g_envMapAliasTable : register(t3, space0);
StructuredBuffer<float2> g_halton : register(t4, space0);
//RWStructuredBuffer<Reservoir> g_nextReservoir : register(u0, space0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

bool EvaluateVisibility(in float3 posW, in float3 wi, in float3 geometricNormal)
{
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
			 RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
			 RAY_FLAG_CULL_NON_OPAQUE> rayQuery;
	
	float3 adjustedRayOrigin = OffsetRayRTG(posW, geometricNormal);
	
	RayDesc ray;
	ray.Origin = adjustedRayOrigin;
	ray.TMin = 0.0f;
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

float3 Shade(in Reservoir currReservoir, in float3 posW, in SurfaceInteraction surface)
{
	float2 phiTheta = currReservoir.EnvMapUV * float2(TWO_PI, PI);
	const float3 lightSurfaceSample = SphericalToCartesian(g_frame.WorldRadius, cos(phiTheta.y), phiTheta.x);
	const float3 wi = normalize(lightSurfaceSample - posW);
	
//	if (!EvaluateVisibility(posW, wi, surface.geometricNormal))
//		return float3(0.0f, 0.0f, 0.0f);
	
	return currReservoir.TargetFunction * rcp(currReservoir.W);
}

void SpatialPass(in uint2 DTid, in int2 textureDim, in float3 posW, in float linearDepth, in float2 linearDepthGrad, 
	inout Reservoir currReservoir, inout SurfaceInteraction surface, inout uint rngState)
{
	Texture2D<half3> g_envMap = ResourceDescriptorHeap[g_frame.EnvMapDescHeapOffset];
	const uint offset = RandUintRange(rngState, 0, g_local.HaltonSeqLength);

	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::BASE_COLOR];
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_METALLIC_ROUGHNESS g_prevMetallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];

	int2 neighborScreenPos;
	const uint numSpatialReuseRounds = currReservoir.DidTemporalReuse ? g_local.NumSpatialSamples : g_local.NumSpatialSamplesWhenTemporalReuseFailed;
	uint numSpatialReuseRoundsSoFar = 0;
	const float origCurrReservoirTarget = currReservoir.TargetFunction;
	const float origCurrReservoirW = currReservoir.W;
	const float2 origCurrReservoirUV = currReservoir.EnvMapUV;
	float M = 0.0f;
	float m_c = 1.0f;
	
	// loop to find the first compatible neighbor
	for (uint i = 0; i < g_local.HaltonSeqLength - 1; i++)
	{
		// avoid infinite-loop
		if (i == g_local.HaltonSeqLength)
			break;
		
		// wrap offset around to bring it to {0, 8} range
		uint haltonSeqOffset = offset + i;
		haltonSeqOffset = haltonSeqOffset < g_local.HaltonSeqLength ? haltonSeqOffset : 0;
		
		int2 neighborOffset = int2(g_halton[haltonSeqOffset] * g_local.SpatialNeighborSearchRadius);
		neighborScreenPos = int2(DTid.xy) + neighborOffset;
		
		// neighbor went out of bounds
		if (!IsInRange(neighborScreenPos, textureDim))
			continue;
						
		// load the gbuffer data for this neighbor
		const half2 encodedNeighborNormal = g_normal[neighborScreenPos].xy;
		const float3 neighborNormal = DecodeUnitNormalFromHalf2(encodedNeighborNormal);

		float neighborLinearDepth = g_depth[neighborScreenPos];
		neighborLinearDepth = ComputeLinearDepth(neighborLinearDepth, g_frame.CameraNear, g_frame.CameraFar);

		// check if neighbor is compatible
		bool isNeighborCompatible = NormalBiasHeuristic(neighborNormal, surface.shadingNormal, g_local.NormalAngleThreshold) &&
									DepthBiasHeuristic(neighborLinearDepth, linearDepth, linearDepthGrad, g_local.DepthToleranceScale);

		if (!isNeighborCompatible)
			continue;
		
		// skip neighbor if it was occluded
		const uint neighborIdx1D = neighborScreenPos.y * textureDim.x + neighborScreenPos.x;
		Reservoir neighborReservoir = g_currReservoir[neighborIdx1D];
		
		if(neighborReservoir.W == 0.0f)
			continue;
		
		float3 neighborPosW = WorldPosFromScreenSpacePos(neighborScreenPos, textureDim, neighborLinearDepth, 
			g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv, g_frame.CurrCameraJitter);
		
		float3 neighborBseColor = g_baseColor[neighborScreenPos].rgb;
		half2 neighborMR = g_prevMetallicRoughness[neighborScreenPos];
		float3 neighborWo = normalize(g_frame.CameraPos - neighborPosW);
		
		SurfaceInteraction neighborSurface = ComputePartialSurfaceInteraction(neighborNormal,
			neighborMR.y,
			neighborMR.x,
			neighborWo,
			neighborBseColor);
		
		// combine with Pairwise-MIS
		// Ref: https://digitalcommons.dartmouth.edu/dissertations/77/
		DoPairwiseMIS(g_envMap, posW, neighborPosW, g_frame.WorldRadius, neighborReservoir, currReservoir,
			neighborSurface, surface, m_c, M, rngState);
		
		numSpatialReuseRoundsSoFar++;
		
		if (numSpatialReuseRoundsSoFar == numSpatialReuseRounds)
			break;
	}

	if (numSpatialReuseRoundsSoFar > 0)
		EndPairwiseMIS(M, origCurrReservoirUV, origCurrReservoirTarget, origCurrReservoirW, numSpatialReuseRoundsSoFar, 
			currReservoir, m_c, rngState);
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(ReSTIR_THREAD_GROUP_SIZE_X, ReSTIR_THREAD_GROUP_SIZE_Y, ReSTIR_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
	const uint2 textureDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);

	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint groupIDFlattened = Gid.y * g_local.DispatchDimX.x + Gid.x;
	const uint tileID = groupIDFlattened / g_local.NumGroupsInTile;
	const uint groupIDinTileFlattened = groupIDFlattened & (g_local.NumGroupsInTile - 1);
	const uint2 groupIDinTile = uint2(groupIDinTileFlattened & (g_local.TileWidth - 1), groupIDinTileFlattened >> g_local.Log2TileWidth);
	const uint swizzledGidFlattened = groupIDinTile.y * g_local.DispatchDimX + tileID * g_local.TileWidth + groupIDinTile.x;
	
	const uint2 swizzledGid = uint2(swizzledGidFlattened % g_local.DispatchDimX, swizzledGidFlattened / g_local.DispatchDimX);
	const uint2 GroupDim = uint2(ReSTIR_THREAD_GROUP_SIZE_X, ReSTIR_THREAD_GROUP_SIZE_Y);
	const uint2 swizzledDTid = swizzledGid * GroupDim + GTid.xy;

	if (swizzledDTid.x >= textureDim.x || swizzledDTid.y >= textureDim.y)
		return;

	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	float4 baseColor = g_baseColor[swizzledDTid];

	// not on the surface of the scene
	const float EPS = 0.0001f;
	if (baseColor.w < MIN_ALPHA_CUTOFF + EPS)
		return;

	// seed the RNG. Use a different seed for this pixel to avoid correlations with the temporal pass
	uint rngState = dot(swizzledDTid.xy, uint2(1, textureDim.x)) ^ PcgHash(g_frame.FrameNum);
	rngState = PcgHash(rngState);

	// reconstruct position from depth buffer
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float linearDepth = g_depth[swizzledDTid];
	linearDepth = ComputeLinearDepth(linearDepth, g_frame.CameraNear, g_frame.CameraFar);
	const float2 posTS = (swizzledDTid.xy + 0.5f) / textureDim;
	const float3 posW = WorldPosFromTexturePos(posTS, linearDepth, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv);
	const float3 wo = normalize(g_frame.CameraPos - posW);

	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const half2 packedNormals = g_normal.Load(int3(swizzledDTid, 0));
	const float3 shadingNormal = DecodeUnitNormalFromHalf2(packedNormals.xy);
	//const float3 geometricNormal = DecodeUnitNormalFromHalf2(packedNormals.zw);

	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	const half2 metalnessRoughness = g_metallicRoughness.Load(int3(swizzledDTid, 0));

	SurfaceInteraction surface = ComputePartialSurfaceInteraction(shadingNormal,
		metalnessRoughness.y,
		metalnessRoughness.x,
		wo,
		baseColor.xyz);

	Texture2D<float2> g_linearDepthPartialDerivatives = ResourceDescriptorHeap[g_local.LinearDepthGradDescHeapIdx];
	const float2 linearDepthGrad = g_linearDepthPartialDerivatives[swizzledDTid.xy];
	
	const uint currPixelIdx1D = swizzledDTid.y * textureDim.x + swizzledDTid.x;
	Reservoir r = g_currReservoir[currPixelIdx1D];
	SpatialPass(swizzledDTid, textureDim, posW, linearDepth, linearDepthGrad, r, surface, rngState);
	
	RWTexture2D<half4> g_outputColor = ResourceDescriptorHeap[g_local.OutputDescHeapIdx];
	float3 color = Shade(r, posW, surface);
	
//	g_nextReservoir[currPixelIdx1D] = r;
	g_outputColor[swizzledDTid.xy] = half4(color, 1.0h);
}