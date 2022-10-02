#include "IndirectDiffuse_Common.h"
#include "../Common/FrameConstants.h"
#include "../Common/LightSourceFuncs.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/BRDF.hlsli"
#include "../Common/Sampling.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/RT.hlsli"
#include "../Common/VolumetricLighting.hlsli"
#include "../Common/SH.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0, space0);
ConstantBuffer<cbIndirectDiffuse> g_local : register(b1, space0);
RaytracingAccelerationStructure g_sceneBVH : register(t0, space0);
StructuredBuffer<Material> g_materials : register(t1, space0);
StructuredBuffer<uint> g_owenScrambledSobolSeq : register(t3, space0);
StructuredBuffer<uint> g_scramblingTile : register(t4, space0);
StructuredBuffer<uint> g_rankingTile : register(t5, space0);
StructuredBuffer<uint> g_frameMeshData : register(t6, space0);

//--------------------------------------------------------------------------------------
// Helper structs
//--------------------------------------------------------------------------------------

#define INVALID_RAY_DIR 100.0f
#define NUM_BINS 8
#define DO_THREAD_GROUP_SWIZZLING
#define DO_RAY_BINNING

struct HitSurface
{
	float3 Pos;
	float2 uv;
	half2 ShadingNormal;
	uint16_t MatID;
	half T;
};

groupshared uint g_binOffset[NUM_BINS];
groupshared uint g_binIndex[NUM_BINS];
groupshared float3 g_sortedOrigin[RT_IND_DIFF_THREAD_GROUP_SIZE_X * RT_IND_DIFF_THREAD_GROUP_SIZE_Y];
groupshared float3 g_sortedDir[RT_IND_DIFF_THREAD_GROUP_SIZE_X * RT_IND_DIFF_THREAD_GROUP_SIZE_Y];
groupshared half2 g_sortedGeoNormals[RT_IND_DIFF_THREAD_GROUP_SIZE_X * RT_IND_DIFF_THREAD_GROUP_SIZE_Y];

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

float2 SignNotZero(float2 v)
{
	return float2((v.x >= 0.0) ? 1.0 : -1.0, (v.y >= 0.0) ? +1.0 : -1.0);
}

float3 MissShading(float3 rayDir)
{
	Texture2D<half3> g_envMap = ResourceDescriptorHeap[g_frame.EnvMapDescHeapOffset];

	// x = sin(theta) * cos(phi)
	// y = cos(theta)
	// z = sin(theta) * sin(phi)
	float3 w = normalize(rayDir);
	float theta = Common::ArcCos(w.y); // [0, PI]
	theta *= ONE_DIV_PI;
	float phi = atan2(w.z, w.x); // [0, PI]
	phi += PI;
	phi *= ONE_DIV_TWO_PI;

	float2 uv = float2(phi, theta);
	float3 color = g_envMap.SampleLevel(g_samLinearClamp, uv, 0.0f);
	
	return color;
}

bool EvaluateVisibility(float3 pos, float3 wi, float3 geometricNormal)
{
	// protect against self-intersection
//	float3 adjustedOrigin = OffsetRayOrigin(pos, geometricNormal, wi);
	//float3 adjustedOrigin = OffsetRayRTG(pos, geometricNormal);
	//float3 adjustedOrigin = pos + 1e-2f * geometricNormal;

	RayQuery < RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
		RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
		RAY_FLAG_CULL_NON_OPAQUE > rayQuery;

	RayDesc ray;
	ray.Origin = pos;
	ray.TMin = g_frame.RayOffset;
	ray.TMax = FLT_MAX;
	ray.Direction = wi;

	// Initialize
	rayQuery.TraceRayInline(g_sceneBVH, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);

	// Traversal
	rayQuery.Proceed();

	// Light source is occluded
	if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		return false;

	return true;
}

bool FindClosestHit(in float3 pos, in float3 wi, in float3 geometricNormal, out HitSurface surface)
{
	// protect against self-intersection
	float3 adjustedOrigin = pos + 1e-1f * geometricNormal;
	//float3 adjustedOrigin = pos + 1.0f * geometricNormal;
//	float3 adjustedOrigin = OffsetRayRTG(pos, geometricNormal);
//	const float3 adjustedOrigin = OffsetRayOrigin(pos, geometricNormal, wi);

	RayQuery < RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
		RAY_FLAG_CULL_NON_OPAQUE > rayQuery;

	RayDesc ray;
	ray.Origin = adjustedOrigin;
	ray.TMin = g_frame.RayOffset;
	ray.TMax = FLT_MAX;
	ray.Direction = wi;

	// Initialize
	rayQuery.TraceRayInline(g_sceneBVH, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);

	// Traversal
	rayQuery.Proceed();

	if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		const uint packedMeshData = g_frameMeshData[rayQuery.CommittedGeometryIndex() + rayQuery.CommittedInstanceID()];
		const uint16_t meshDescHeapIdx = (uint16_t) (packedMeshData >> 16);

		StructuredBuffer<Vertex> VB = ResourceDescriptorHeap[meshDescHeapIdx];
		StructuredBuffer<INDEX_TYPE> IB = ResourceDescriptorHeap[meshDescHeapIdx + 1];

		INDEX_TYPE tri = (INDEX_TYPE) rayQuery.CandidatePrimitiveIndex() * 3;
		INDEX_TYPE i0 = IB[tri];
		INDEX_TYPE i1 = IB[tri + 1];
		INDEX_TYPE i2 = IB[tri + 2];

		Vertex V0 = VB[i0];
		Vertex V1 = VB[i1];
		Vertex V2 = VB[i2];

		const float2 barry = rayQuery.CommittedTriangleBarycentrics();
		float2 uv = V0.TexUV + barry.x * (V1.TexUV - V0.TexUV) + barry.y * (V2.TexUV - V0.TexUV);
		float3 normal = V0.NormalL + barry.x * (V1.NormalL - V0.NormalL) + barry.y * (V2.NormalL - V0.NormalL);
		// TODO should it be 4x3 or 3x4?
		normal = normalize(mul(normal, (float3x3) rayQuery.CandidateObjectToWorld4x3()));

		surface.Pos = rayQuery.WorldRayOrigin() + rayQuery.WorldRayDirection() * rayQuery.CommittedRayT();
		surface.uv = uv;
		surface.ShadingNormal = Common::EncodeUnitNormalAsHalf2(normal);
		surface.MatID = (uint16_t) packedMeshData & 0xff;
		surface.T = (half) rayQuery.CommittedRayT();

		return true;
	}

	return false;
}

bool Trace(in uint Gidx, in float3 origin, in float3 dir, in half2 encodedGeometricNormal,
	out HitSurface hitInfo, out bool isRayValid, out uint16_t sortedIdx)
{
#if defined(DO_RAY_BINNING)
	//
	// ray-binning: find the number of directions that fall into each bin
	//

	// Ref: Cigolle et al, Survey of Efficient Representations for Independent Unit Vectors, JCGT, 2014
	float2 proj = dir.xz / (abs(dir.x) + abs(dir.y) + abs(dir.z));
	proj = (dir.y <= 0.0f) ? ((1.0f - abs(proj.yx)) * SignNotZero(proj)) : proj;

	const bool xGt0 = proj.x >= 0.0f;
	const bool yGt0 = proj.y >= 0.0f;
	const bool upper = abs(proj.x) + abs(proj.y) >= 0.0f;

	bool isInBin[NUM_BINS];
	isInBin[0] = xGt0 && yGt0 && upper;
	isInBin[1] = xGt0 && yGt0 && !upper;
	isInBin[2] = xGt0 && !yGt0 && upper;
	isInBin[3] = xGt0 && !yGt0 && !upper;
	isInBin[4] = !xGt0 && yGt0 && upper;
	isInBin[5] = !xGt0 && yGt0 && !upper;
	isInBin[6] = !xGt0 && !yGt0 && upper;
	isInBin[7] = !xGt0 && !yGt0 && !upper;

	uint16_t laneBin = 0;

	[unroll]
	for (uint16_t i = 1; i < NUM_BINS; i++)
		laneBin += i * isInBin[i];

	uint16_t waveBinSize[NUM_BINS];
	waveBinSize[0] = (uint16_t) WaveActiveCountBits(isInBin[0]);
	waveBinSize[1] = (uint16_t) WaveActiveCountBits(isInBin[1]);
	waveBinSize[2] = (uint16_t) WaveActiveCountBits(isInBin[2]);
	waveBinSize[3] = (uint16_t) WaveActiveCountBits(isInBin[3]);
	waveBinSize[4] = (uint16_t) WaveActiveCountBits(isInBin[4]);
	waveBinSize[5] = (uint16_t) WaveActiveCountBits(isInBin[5]);
	waveBinSize[6] = (uint16_t) WaveActiveCountBits(isInBin[6]);
	waveBinSize[7] = (uint16_t) WaveActiveCountBits(isInBin[7]);

	const uint laneIdx = WaveGetLaneIndex();
	uint16_t prefixSum = laneIdx < NUM_BINS ? waveBinSize[laneIdx] : 0;

	if (laneIdx < NUM_BINS - 1)
	{
		prefixSum = WavePrefixSum(prefixSum);
		InterlockedAdd(g_binOffset[laneIdx + 1], (uint) prefixSum); // g_binOffset[0] is 0
	}

	GroupMemoryBarrierWithGroupSync();

	// all the offsets are now known, sort them

	uint idxInBin;
	InterlockedAdd(g_binIndex[laneBin], 1, idxInBin);

	const uint idx = g_binOffset[laneBin] + idxInBin;
	g_sortedOrigin[idx] = origin;
	g_sortedDir[idx] = dir;
	g_sortedGeoNormals[idx] = encodedGeometricNormal;
	sortedIdx = (uint16_t) idx;

	GroupMemoryBarrierWithGroupSync();

	float3 newOrigin = g_sortedOrigin[Gidx];
	float3 newDir = g_sortedDir[Gidx];
	float3 newGeometricNormal = Common::DecodeUnitNormalFromHalf2(g_sortedGeoNormals[Gidx]);

#else	
	float3 newOrigin = origin;
	float3 newDir = dir;
	float3 newGeometricNormal = DecodeUnitNormalFromHalf2(encodedGeometricNormal);
#endif	

	// compute the incoming radiance from the direction wi
	// don't shade the surface position just yet, defer that to after denoising

	// skip invalid rays
	if (dot(1, newDir) >= INVALID_RAY_DIR)
	{
		isRayValid = false;
		return false;
	}

	isRayValid = true;
	bool hit = FindClosestHit(newOrigin, newDir, newGeometricNormal, hitInfo);

	return hit;
}

float3 DirectLighting(HitSurface hitInfo)
{
	float3 normal = Common::DecodeUnitNormalFromHalf2(hitInfo.ShadingNormal);

	// geometric normal is unavailable
	if (!EvaluateVisibility(hitInfo.Pos, -g_frame.SunDir, normal))
		return 0.0.xxx;

	Material mat = g_materials[hitInfo.MatID];

	half3 baseColor = (half3) mat.BaseColorFactor.rgb;
	if (mat.BaseColorTexture != -1)
	{
		BASE_COLOR_MAP g_baseCol = ResourceDescriptorHeap[g_frame.BaseColorMapsDescHeapOffset + mat.BaseColorTexture];
		baseColor *= g_baseCol.SampleLevel(g_samLinearClamp, hitInfo.uv, 0.0f).rgb;
	}

	half metalness = (half) mat.MetallicFactor;
	if (mat.MetallicRoughnessTexture != -1)
	{
		uint offset = g_frame.MetalnessRoughnessMapsDescHeapOffset + mat.MetallicRoughnessTexture;

		// green channel contains roughness values and blue channel contains metalness values
		METALNESS_ROUGHNESS_MAP g_metallicRoughnessMap = ResourceDescriptorHeap[offset];
		metalness *= g_metallicRoughnessMap.SampleLevel(g_samLinearClamp, hitInfo.uv, 0.0f).b;
	}

	// no normal mapping for now...
	float ndotWi = saturate(dot(normal, -g_frame.SunDir));

	// assume the surface is Lambertian
	half3 diffuseReflectance = baseColor * (1.0h - metalness);
	float3 brdf = BRDF::LambertianBRDF(diffuseReflectance, ndotWi);

	const float3 sigma_t_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
	const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
	const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;

	float3 posW = hitInfo.Pos;
	posW.y += g_frame.PlanetRadius;
	
	float t = Volumetric::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, posW, -g_frame.SunDir);
	float3 tr = Volumetric::EstimateTransmittance(g_frame.PlanetRadius, posW, -g_frame.SunDir, t,
		sigma_t_rayleigh, sigma_t_mie, sigma_t_ozone, 8);

	float3 L_i = brdf * tr * g_frame.SunIlluminance;
	float3 L_e = mat.EmissiveFactor;

	if (mat.EmissiveTexture != -1)
	{
		EMISSIVE_MAP g_emissiveMap = ResourceDescriptorHeap[g_frame.EmissiveMapsDescHeapOffset + mat.EmissiveTexture];
		L_e *= g_emissiveMap.SampleLevel(g_samLinearClamp, hitInfo.uv, 0.0f).rgb;
	}

	return L_i + L_e;
}

float3 ComputeIndirectLi(in uint2 DTid, in uint Gidx, in bool shouldThisLaneTrace, out uint16_t sortedIdx, out half rayT)
{
	// reconstruct position from depth buffer
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float linearDepth = g_depth[DTid];
	linearDepth = Common::ComputeLinearDepthReverseZ(linearDepth, g_frame.CameraNear);

	const uint2 textureDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 posW = Common::WorldPosFromScreenSpacePos(DTid,
		textureDim,
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv,
		g_frame.CurrCameraJitter);

	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	half2 packedNormals = g_normal[DTid];
	float3 shadingNormal = Common::DecodeUnitNormalFromHalf2(packedNormals.xy);
	half2 encodedGeometricNormal = packedNormals.xy;
	
	// sample the cosine-weighted hemisphere above surfact pos
	float3 wi = float3(INVALID_RAY_DIR, INVALID_RAY_DIR, INVALID_RAY_DIR);
	if (shouldThisLaneTrace)
	{
		const uint sampleIdx = g_frame.FrameNum & 31;
		//const uint sampleIdx = 0;
		const float u0 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			DTid.x, DTid.y, sampleIdx, 0);
		const float u1 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			DTid.x, DTid.y, sampleIdx, 1);

		wi = BRDF::SampleLambertianBrdf(shadingNormal, float2(u0, u1));
	}
	
	// trace a ray along wi to find a possible surface point
	bool isRayValid;
	HitSurface hitInfo;
	bool hit = Trace(Gidx, posW, wi, encodedGeometricNormal, hitInfo, isRayValid, sortedIdx);

	// what's the incoming radiance (Li) from this surface point
	float3 L_i = 0.0;

	// if the ray hit a surface, compute direct lighting at the given surface point, otherise do miss shading,
	// which returns the incoming radiance from sky
	if (hit)
	{
		L_i = DirectLighting(hitInfo);
		rayT = hitInfo.T;
	}
	else if (isRayValid)
	{
#if defined(DO_RAY_BINNING)
		float3 newDir = g_sortedDir[Gidx];
#else
		float3 newDir = wi;
#endif
		L_i = MissShading(newDir);
	}

	return L_i;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RT_IND_DIFF_THREAD_GROUP_SIZE_X, RT_IND_DIFF_THREAD_GROUP_SIZE_Y, RT_IND_DIFF_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
#if defined(DO_THREAD_GROUP_SWIZZLING)
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint16_t groupIDFlattened = (uint16_t) Gid.y * g_local.DispatchDimX.x + (uint16_t) Gid.x;
	const uint16_t tileID = groupIDFlattened / g_local.NumGroupsInTile;
	const uint16_t groupIDinTileFlattened = groupIDFlattened % g_local.NumGroupsInTile;

	// TileWidth is a power of 2 for all tiles except possibly the last one
	const uint16_t numFullTiles = g_local.DispatchDimX.x / g_local.TileWidth; // floor(DispatchDimX / TileWidth
	const uint16_t numGroupsInFullTiles = numFullTiles * g_local.NumGroupsInTile;

	uint16_t2 groupIDinTile;
	if (groupIDFlattened >= numGroupsInFullTiles)
	{
		const uint16_t lastTileDimX = g_local.DispatchDimX.x - g_local.TileWidth * numFullTiles; // DispatchDimX & NumGroupsInTile
		groupIDinTile = uint16_t2(groupIDinTileFlattened % lastTileDimX, groupIDinTileFlattened / lastTileDimX);
	}
	else
	{
		groupIDinTile = uint16_t2(groupIDinTileFlattened & (g_local.TileWidth - 1), groupIDinTileFlattened >> g_local.Log2TileWidth);
	}

	const uint16_t swizzledGidFlattened = groupIDinTile.y * g_local.DispatchDimX + tileID * g_local.TileWidth + groupIDinTile.x;
	const uint16_t2 swizzledGid = uint16_t2(swizzledGidFlattened % g_local.DispatchDimX, swizzledGidFlattened / g_local.DispatchDimX);
	const uint16_t2 groupDim = uint16_t2(RT_IND_DIFF_THREAD_GROUP_SIZE_X, RT_IND_DIFF_THREAD_GROUP_SIZE_Y);
	const uint16_t2 swizzledDTid = swizzledGid * groupDim + (uint16_t2) GTid.xy;
#else
	const uint2 swizzledDTid = DTid.xy;
#endif

	// this lane should still participates in non-trace computations
	bool shouldThisLaneTrace = swizzledDTid.x < g_local.InputWidth && swizzledDTid.y < g_local.InputHeight;

#if defined(DO_RAY_BINNING)	
	if (Gidx < NUM_BINS)
	{
		g_binOffset[Gidx] = 0;
		g_binIndex[Gidx] = 0;
	}

	GroupMemoryBarrierWithGroupSync();
#endif	

	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const half isSurfaceMarker = g_baseColor[swizzledDTid].w;

	// this lane still participates in non-trace computations
	shouldThisLaneTrace = shouldThisLaneTrace && (isSurfaceMarker > MIN_ALPHA_CUTOFF);

	uint16_t sortedIdx;
	half rayT = 0.0;
	float3 val = 0.0.xxx;
	val = ComputeIndirectLi(swizzledDTid, Gidx, shouldThisLaneTrace, sortedIdx, rayT);

#if defined(DO_RAY_BINNING)
	g_sortedOrigin[Gidx] = val.xyz;
	g_sortedDir[Gidx].x = rayT;
	
	GroupMemoryBarrierWithGroupSync();

//	if (shouldThisLaneTrace)
	val.xyz = g_sortedOrigin[sortedIdx];
	val = max(0.0, val);
	
	rayT = (half) g_sortedDir[sortedIdx].x;
#endif	
	
	// write the results to memory
	if (shouldThisLaneTrace)
	{
		RWTexture2D<float4> outLiRayT = ResourceDescriptorHeap[g_local.OutputDescHeapIdx];		
		outLiRayT[swizzledDTid] = float4(val.xyz, rayT);
	}
}