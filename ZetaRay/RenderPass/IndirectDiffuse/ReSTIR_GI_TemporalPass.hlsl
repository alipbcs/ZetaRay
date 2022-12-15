#include "Reservoir.hlsli"
#include "../Common/Common.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/Material.h"
#include "../Common/GBuffers.hlsli"
#include "../Common/BRDF.hlsli"
#include "../Common/Sampling.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/RT.hlsli"
#include "../Common/VolumetricLighting.hlsli"

#define INVALID_RAY_DIR 32768.xxx
#define NUM_BINS 8
#define THREAD_GROUP_SWIZZLING 1
#define RAY_BINNING 1
#define COSINE_WEIGHTED_SAMPLING 0
#define USE_RAY_CONES 0

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbTemporalPass> g_local : register(b1);
RaytracingAccelerationStructure g_sceneBVH : register(t0);
StructuredBuffer<Material> g_materials : register(t1);
StructuredBuffer<uint> g_owenScrambledSobolSeq : register(t3);
StructuredBuffer<uint> g_scramblingTile : register(t4);
StructuredBuffer<uint> g_rankingTile : register(t5);
StructuredBuffer<RT::MeshInstance> g_frameMeshData : register(t6);
StructuredBuffer<Vertex> g_sceneVertices : register(t7);
StructuredBuffer<uint> g_sceneIndices : register(t8);

//--------------------------------------------------------------------------------------
// 
//--------------------------------------------------------------------------------------

struct HitSurface
{
	float3 Pos;
	float2 uv;
	half2 ShadingNormal;
	uint16_t MatID;
	half T;
#if USE_RAY_CONES
	half Lambda;
#endif
};

#if RAY_BINNING
groupshared uint g_binOffset[NUM_BINS];
groupshared uint g_binIndex[NUM_BINS];
groupshared float3 g_sortedOrigin[RGI_TEMPORAL_THREAD_GROUP_SIZE_X * RGI_TEMPORAL_THREAD_GROUP_SIZE_Y];
groupshared float3 g_sortedDir[RGI_TEMPORAL_THREAD_GROUP_SIZE_X * RGI_TEMPORAL_THREAD_GROUP_SIZE_Y];
#endif

#if USE_RAY_CONES
groupshared RT::RayCone g_sortedRayCones[RGI_TEMPORAL_THREAD_GROUP_SIZE_X * RGI_TEMPORAL_THREAD_GROUP_SIZE_Y];
#endif

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

float2 SignNotZero(float2 v)
{
	return float2((v.x >= 0.0) ? 1.0 : -1.0, (v.y >= 0.0) ? +1.0 : -1.0);
}

// wo is assumed to be normalized
half3 MissShading(float3 wo)
{
	Texture2D<half3> g_envMap = ResourceDescriptorHeap[g_frame.EnvMapDescHeapOffset];
	
	float2 thetaPhi = Math::SphericalFromCartesian(wo);

	float2 uv = float2(thetaPhi.y * ONE_DIV_TWO_PI, thetaPhi.x * ONE_DIV_PI);
	half3 color = g_envMap.SampleLevel(g_samLinearClamp, uv, 0.0f);
	
	return color;
}

bool EvaluateVisibility(float3 pos, float3 wi, float3 normal)
{
	// protect against self-intersection
	float3 adjustedOrigin = pos + normal * 5e-3f;

	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
		RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
		RAY_FLAG_CULL_NON_OPAQUE> rayQuery;

	RayDesc ray;
	ray.Origin = adjustedOrigin;
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

bool FindClosestHit(float3 pos, float3 wi, RT::RayCone rayCone, out HitSurface surface)
{
	RayQuery < RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
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

	if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		const RT::MeshInstance meshData = g_frameMeshData[rayQuery.CommittedGeometryIndex() + rayQuery.CommittedInstanceID()];
		//const uint16_t meshDescHeapIdx = (uint16_t) (packedMeshData >> 16);

		//StructuredBuffer<Vertex> VB = ResourceDescriptorHeap[meshDescHeapIdx];
		//StructuredBuffer<INDEX_TYPE> IB = ResourceDescriptorHeap[meshDescHeapIdx + 1];

		uint tri = rayQuery.CandidatePrimitiveIndex() * 3;
		tri += meshData.BaseIdxOffset;
		uint i0 = g_sceneIndices[tri] + meshData.BaseVtxOffset;
		uint i1 = g_sceneIndices[tri + 1] + meshData.BaseVtxOffset;
		uint i2 = g_sceneIndices[tri + 2] + meshData.BaseVtxOffset;

		Vertex V0 = g_sceneVertices[i0];
		Vertex V1 = g_sceneVertices[i1];
		Vertex V2 = g_sceneVertices[i2];

		const float2 barry = rayQuery.CommittedTriangleBarycentrics();
		float2 uv = V0.TexUV + barry.x * (V1.TexUV - V0.TexUV) + barry.y * (V2.TexUV - V0.TexUV);
		float3 normal = V0.NormalL + barry.x * (V1.NormalL - V0.NormalL) + barry.y * (V2.NormalL - V0.NormalL);
		// TODO should it be 4x3 or 3x4?
		normal = normalize(mul(normal, (float3x3) rayQuery.CandidateObjectToWorld4x3()));

		surface.Pos = rayQuery.WorldRayOrigin() + rayQuery.WorldRayDirection() * rayQuery.CommittedRayT();
		surface.uv = uv;
		surface.ShadingNormal = Math::Encoding::EncodeUnitNormalAsHalf2(normal);
		surface.MatID = meshData.MatID;
		surface.T = (half) rayQuery.CommittedRayT();

#if USE_RAY_CONES		
		float ndotwo = saturate(dot(normal, -wi));
		
		rayCone.Update(rayQuery.CommittedRayT(), 0);
		float lambda = rayCone.ComputeLambda(V0.PosL, V1.PosL, V2.PosL, V0.TexUV, V1.TexUV, V2.TexUV, ndotwo);
		surface.Lambda = half(lambda);
#endif	
		
		return true;
	}

	return false;
}

bool Trace(uint Gidx, float3 origin, float3 dir, RT::RayCone rayCone, out HitSurface hitInfo,
	out bool isRayValid, out uint16_t sortedIdx)
{
#if RAY_BINNING
	//
	// ray-binning: find the number of directions that fall into each bin
	//

	// Ref: Cigolle et al, "Survey of Efficient Representations for Independent Unit Vectors," Journal of Computer Graphics Techniques, 2014.
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
	g_sortedDir[idx].xyz = dir;

#if USE_RAY_CONES
	g_sortedRayCones[idx] = rayCone;
#endif

	sortedIdx = (uint16_t) idx;

	GroupMemoryBarrierWithGroupSync();

	float3 newOrigin = g_sortedOrigin[Gidx];
	float3 newDir = g_sortedDir[Gidx].xyz;
	
#if USE_RAY_CONES
	RT::RayCone newRayCone = g_sortedRayCones[Gidx];
#else
	RT::RayCone newRayCone = rayCone;
#endif
	
#else	
	float3 newOrigin = origin;
	float3 newDir = dir;
	RT::RayCone newRayCone = rayCone;
#endif	

	// compute the incoming radiance from wi
	// don't shade the surface position just yet, defer that to after denoising

	// skip invalid rays
	if (newDir.x == INVALID_RAY_DIR.x)
	{
		isRayValid = false;
		return false;
	}

	isRayValid = true;
	bool hit = FindClosestHit(newOrigin, newDir, newRayCone, hitInfo);

	return hit;
}

float3 DirectLighting(HitSurface hitInfo, float3 wo)
{
	float3 normal = Math::Encoding::DecodeUnitNormalFromHalf2(hitInfo.ShadingNormal);

	if (!EvaluateVisibility(hitInfo.Pos, -g_frame.SunDir, normal))
		return 0.0.xxx;

	Material mat = g_materials[hitInfo.MatID];

	float mipOffset = g_frame.MipBias;
	//float mipOffset = 0;
		
	half3 baseColor = (half3) mat.BaseColorFactor.rgb;
	if (mat.BaseColorTexture != -1)
	{
		uint offset = NonUniformResourceIndex(g_frame.BaseColorMapsDescHeapOffset + mat.BaseColorTexture);
		BASE_COLOR_MAP g_baseCol = ResourceDescriptorHeap[offset];
		
#if USE_RAY_CONES
		uint w;
		uint h;
		g_baseCol.GetDimensions(w, h);
		mipOffset += RT::RayCone::ComputeTextureMipmapOffset(hitInfo.Lambda, w, h);
#endif	
		
		baseColor *= g_baseCol.SampleLevel(g_samLinearClamp, hitInfo.uv, mipOffset).rgb;
	}

	half metalness = (half) mat.MetallicFactor;
	if (mat.MetalnessRoughnessTexture != -1)
	{
		uint offset = NonUniformResourceIndex(g_frame.MetalnessRoughnessMapsDescHeapOffset + mat.MetalnessRoughnessTexture);
		// green & blue channels contain roughness & metalness values respectively
		METALNESS_ROUGHNESS_MAP g_metalnessRoughnessMap = ResourceDescriptorHeap[offset];

#if 0	
		uint w;
		uint h;
		g_metalnessRoughnessMap.GetDimensions(w, h);
		mipOffset = RT::RayCone::ComputeTextureMipmapOffset(hitInfo.Lambda, w, h);
#endif	

		metalness *= g_metalnessRoughnessMap.SampleLevel(g_samLinearClamp, hitInfo.uv, mipOffset).b;
	}

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
	
	float3 L_o = brdf * tr * g_frame.SunIlluminance;
	float3 L_e = mat.EmissiveFactor;

	if (mat.EmissiveTexture != -1)
	{
		uint offset = NonUniformResourceIndex(g_frame.EmissiveMapsDescHeapOffset + mat.EmissiveTexture);	
		EMISSIVE_MAP g_emissiveMap = ResourceDescriptorHeap[offset];
		
#if 0	
		uint w;
		uint h;
		g_emissiveMap.GetDimensions(w, h);
		mipOffset = RT::RayCone::ComputeTextureMipmapOffset(hitInfo.Lambda, w, h);
#endif
		
		L_e *= g_emissiveMap.SampleLevel(g_samLinearClamp, hitInfo.uv, mipOffset).rgb;
	}

	return L_o + L_e;
}

Sample ComputeLi(uint2 DTid, uint Gidx, float3 posW, float3 normal, float3 wi, RT::RayCone rayCone, 
	out uint16_t sortedIdx)
{
	Sample ret;
	
	// trace a ray along wi to find closest surface point
	bool isSortedRayValid;
	HitSurface hitInfo;
	float3 adjustedOrigin = posW + 5e-3f * normal;		// protect against self-intersection

	bool hit = Trace(Gidx, adjustedOrigin, wi, rayCone, hitInfo, isSortedRayValid, sortedIdx);

	// what's the outgoing radiance from the closest hit point towards wo
	ret.Lo = 0.0.xxx;

	// if the ray hit a surface, compute direct lighting at the given surface point, otherise 
	// returns the incoming radiance from sky
	if (hit)
	{
		ret.Lo = (half3) DirectLighting(hitInfo, -wi);
		ret.Pos = hitInfo.Pos;
		ret.Normal = hitInfo.ShadingNormal;
		ret.RayT = hitInfo.T;
	}
	else if (isSortedRayValid)
	{
#if RAY_BINNING
		float3 newDir = g_sortedDir[Gidx];
#else
		float3 newDir = wi;
#endif
		ret.Lo = MissShading(newDir);
		
		float3 temp = posW;		// make a copy to avoid accumulating floating-point error
		temp.y += g_frame.PlanetRadius;
		float t = Volumetric::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, temp, newDir);
		ret.Pos = posW + t * newDir;
		ret.Normal = Math::Encoding::EncodeUnitNormalAsHalf2(-normalize(ret.Pos));
		ret.RayT = (half) t;
	}
	
	return ret;
}

float4 GeometricHeuristic(float3 histPositions[4], float3 currNormal, float3 currPos)
{
	float4 planeDist = float4(dot(currNormal, histPositions[0] - currPos),
		dot(currNormal, histPositions[1] - currPos),
		dot(currNormal, histPositions[2] - currPos),
		dot(currNormal, histPositions[3] - currPos));
	
//	float4 weights = saturate(1 - abs(planeDist) / g_local.MaxPlaneDist);
	float4 weights = planeDist <= g_local.MaxPlaneDist;
	
	return weights;
}

void SampleTemporalReservoirAndResample(uint2 DTid, float3 posW, float3 normal, inout Reservoir r, inout RNG rng)
{
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	
	// reverse reproject current pixel
	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
	const half2 motionVec = g_motionVector[DTid.xy];
	const float2 currUV = (DTid + 0.5f) / renderDim;
	const float2 prevUV = currUV - motionVec;

	if (!Math::IsWithinBoundsInc(prevUV, 1.0f.xx))
		return;

	// retrieve the 2x2 neighborhood reservoirs around reprojected pixel

	const float2 f = prevUV * renderDim;
	const float2 topLeft = floor(f - 0.5f); // e.g if p0 is at (20.5, 30.5), then topLeft would be (20, 30)
	const float2 offset = f - (topLeft + 0.5f);
	const float2 topLeftTexelUV = (topLeft + 0.5f) / renderDim;
	
	// previous frame's depth
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float4 prevDepths = g_prevDepth.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	prevDepths = Math::Transform::LinearDepthFromNDC(prevDepths, g_frame.CameraNear);

	float2 prevUVs[4];
	prevUVs[0] = topLeftTexelUV;
	prevUVs[1] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 0.0f);
	prevUVs[2] = topLeftTexelUV + float2(0.0f, 1.0f / g_frame.RenderHeight);
	prevUVs[3] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 1.0f / g_frame.RenderHeight);
	
	float3 prevPos[4];
	
	[unroll]
	for (int j = 0; j < 4; j++)
	{
		prevPos[j] = Math::Transform::WorldPosFromUV(prevUVs[j], prevDepths[j], g_frame.TanHalfFOV, g_frame.AspectRatio,
			g_frame.PrevViewInv);
	}
	
	const float4 geoWeights = GeometricHeuristic(prevPos, normal, posW);
	
	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);
	
	float4 weights = geoWeights * bilinearWeights;
	weights *= weights > 1e-4;
	float weightSum = dot(1, weights);
	
	if(weightSum < 1e-4)
		return;
	
	weights /= weightSum;	// renormalize
	
	const uint2 offsets[4] = { uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(1, 1) };
	
	// q -> reused path, r -> current pixel's path
	const float3 x1_r = posW;
	
	[unroll]
	for (int i = 0; i < 4; i++)
	{
		uint2 prevPixel = uint2(topLeft) + offsets[i];

		if (!Math::IsWithinBoundsExc(prevPixel, uint2(renderDim)) || weights[i] == 0.0)
			continue;
		
		Reservoir prevReservoir = ReadInputReservoir(prevPixel, g_local.PrevTemporalReservoir_A_DescHeapIdx, 
			g_local.PrevTemporalReservoir_B_DescHeapIdx, g_local.PrevTemporalReservoir_C_DescHeapIdx);

		
		// TODO following seems the logical thing to do, but for some it causes a strange 
		// bug where surface illumination slowly pulsates between light and dark over time
#if 0
		if(prevReservoir.SampleNormal.x == INVALID_SAMPLE_NORMAL.x)
		{
			weights[i] = 0;
			weightSum = dot(1, weights);
			
			if(weightSum <= 1e-4)
				break;
			
			weights /= weightSum;
			
			continue;
		}
#endif		

		float jacobianDet = 1.0f;
		const float3 x1_q = prevPos[i];
		const float3 x2_q = prevReservoir.SamplePos;
		const float3 secondToFirst_r = x1_r - x2_q;
		const float3 wi = normalize(-secondToFirst_r);
		
		if (g_local.PdfCorrection)
			jacobianDet = JacobianDeterminant(x1_q, x2_q, wi, secondToFirst_r, prevReservoir);
		
		r.Combine(prevReservoir, wi, normal, MAX_TEMPORAL_M, weights[i], jacobianDet, rng);
	}
}

Reservoir DoTemporalResampling(uint2 DTid, float3 posW, float3 normal, float sourcePdf, Sample s, 
	bool isSampleValid, inout RNG rng)
{
	Reservoir r = Reservoir::Init();

	// cosine term in the target cancels out cosine term in the source pdf (i.e. cos(theta) / PI)
#if (COSINE_WEIGHTED_SAMPLING && INCLUDE_COSINE_TERM_IN_TARGET)
	sourcePdf = ONE_DIV_PI;
#endif		

	if (isSampleValid)
	{
		const float lum = Math::Color::LuminanceFromLinearRGB(s.Lo);
		const float risWeight = lum / max(1e-4f, sourcePdf);
	
		r.Update(risWeight, s, rng);
	}
	
	if (g_local.DoTemporalResampling && g_local.IsTemporalReservoirValid)
		SampleTemporalReservoirAndResample(DTid, posW, normal, r, rng);
	
	return r;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

static const uint16_t2 GroupDim = uint16_t2(RGI_TEMPORAL_THREAD_GROUP_SIZE_X, RGI_TEMPORAL_THREAD_GROUP_SIZE_Y);

[WaveSize(32)]
[numthreads(RGI_TEMPORAL_THREAD_GROUP_SIZE_X, RGI_TEMPORAL_THREAD_GROUP_SIZE_Y, RGI_TEMPORAL_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint16_t groupIDFlattened = (uint16_t) Gid.y * g_local.DispatchDimX + (uint16_t) Gid.x;
	const uint16_t tileID = groupIDFlattened / g_local.NumGroupsInTile;
	const uint16_t groupIDinTileFlattened = groupIDFlattened % g_local.NumGroupsInTile;

	// TileWidth is a power of 2 for all tiles except possibly the last one
	const uint16_t numFullTiles = g_local.DispatchDimX / RGI_TEMPORAL_TILE_WIDTH; // floor(DispatchDimX / TileWidth
	const uint16_t numGroupsInFullTiles = numFullTiles * g_local.NumGroupsInTile;

	uint16_t2 groupIDinTile;
	if (groupIDFlattened >= numGroupsInFullTiles)
	{
		const uint16_t lastTileDimX = g_local.DispatchDimX - RGI_TEMPORAL_TILE_WIDTH * numFullTiles; // DispatchDimX & NumGroupsInTile
		groupIDinTile = uint16_t2(groupIDinTileFlattened % lastTileDimX, groupIDinTileFlattened / lastTileDimX);
	}
	else
		groupIDinTile = uint16_t2(groupIDinTileFlattened & (RGI_TEMPORAL_TILE_WIDTH - 1), groupIDinTileFlattened >> RGI_TEMPORAL_LOG2_TILE_WIDTH);

	const uint16_t swizzledGidFlattened = groupIDinTile.y * g_local.DispatchDimX + tileID * RGI_TEMPORAL_TILE_WIDTH + groupIDinTile.x;
	const uint16_t2 swizzledGid = uint16_t2(swizzledGidFlattened % g_local.DispatchDimX, swizzledGidFlattened / g_local.DispatchDimX);
	const uint16_t2 swizzledDTid = swizzledGid * GroupDim + (uint16_t2) GTid.xy;
#else
	const uint16_t2 swizzledDTid = (uint16_t2)DTid.xy;
#endif

	// this lane should still participates in non-trace computations
	const bool isWithinScreenBounds = swizzledDTid.x < g_frame.RenderWidth && swizzledDTid.y < g_frame.RenderHeight;
	const bool traceThisFrame = g_local.CheckerboardTracing ?
		((swizzledDTid.x + swizzledDTid.y) & 0x1) == (g_local.FrameCounter & 0x1) :
		true;
	
#if RAY_BINNING
	if (Gidx < NUM_BINS)
	{
		g_binOffset[Gidx] = 0;
		g_binIndex[Gidx] = 0;
	}

	GroupMemoryBarrierWithGroupSync();
#endif	
	
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[swizzledDTid];

	// can't early exit due to ray binning
	const bool isPixelValid = isWithinScreenBounds && (depth != 0);
	
	// reconstruct position from depth buffer
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);

	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 posW = Math::Transform::WorldPosFromScreenSpace(swizzledDTid,
		renderDim,
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormalFromHalf2(g_normal[swizzledDTid]);
	
	// sample the cosine-weighted hemisphere above pos
	float3 wi = INVALID_RAY_DIR;
	float pdf;
	
	if (isPixelValid && traceThisFrame)
	{
		//const uint sampleIdx = 0;
		const float u0 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			swizzledDTid.x, swizzledDTid.y, g_local.SampleIndex, 0);
		const float u1 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			swizzledDTid.x, swizzledDTid.y, g_local.SampleIndex, 1);

#if COSINE_WEIGHTED_SAMPLING
		wi = BRDF::SampleLambertianBrdf(normal, float2(u0, u1), pdf);
#else
		wi = Sampling::UniformSampleHemisphere(float2(u0, u1), pdf);
		float4 q = Math::Transform::QuaternionFromY(normal);
		// transform from local space to world space
		wi = Math::Transform::RotateVector(wi, q);
#endif
	}
	
#if USE_RAY_CONES
	// approximate curvature
	const int laneIdx = WaveGetLaneIndex();
	const int neighborX = laneIdx & 0x1 ? max(0, laneIdx - 1) : laneIdx + 1;
	const int neighborY = laneIdx >= 16 ? max(0, laneIdx - 16) : laneIdx + 16;
	
	const float3 dNormaldx = laneIdx & 0x1 ? normal - WaveReadLaneAt(normal, neighborX) : WaveReadLaneAt(normal, neighborX) - normal;
	const float3 dNormaldy = laneIdx >= 16 ? normal - WaveReadLaneAt(normal, neighborY) : WaveReadLaneAt(normal, neighborY) - normal;
	
	// eq. (31) in Ray Tracing Gems 1, ch. 20
	const float phi = length(dNormaldx + dNormaldy);

	RT::RayCone rayCone = RT::RayCone::Init(g_frame.PixelSpreadAngle, phi, linearDepth);
#else
	RT::RayCone rayCone = RT::RayCone::Init(g_frame.PixelSpreadAngle, 0, linearDepth);
#endif	
	
	uint16_t sortedIdx;
	Sample retSample = ComputeLi(swizzledDTid, Gidx, posW, normal, wi, rayCone, sortedIdx);
	
#if RAY_BINNING
	g_sortedOrigin[Gidx] = PackNormalLiRayT(retSample.Normal, retSample.Lo, retSample.RayT);
	g_sortedDir[Gidx] = retSample.Pos;
	
	GroupMemoryBarrierWithGroupSync();

	// retrieve the non-sorted vals
	UnpackNormalLiRayT(g_sortedOrigin[sortedIdx], retSample.Normal, retSample.Lo, retSample.RayT);
	retSample.Pos = g_sortedDir[sortedIdx].xyz;
#endif	
	
	// resampling
	if (isPixelValid)
	{
		RNG rng = RNG::Init(swizzledDTid, g_local.FrameCounter, renderDim);
						
		//const float cosTheta = saturate(pdf * PI);
		Reservoir r = DoTemporalResampling(swizzledDTid, posW, normal, pdf, retSample, traceThisFrame, rng);
		
		// TODO under checkerboarding, result can be NaN when ray binning is enabled.
		// Need further investigation to figure out the cause. Following seems to mitigate
		// the issue for now with no apparent artifacts
#if RAY_BINNING
		if (isnan(r.SamplePos.x) && !traceThisFrame)
			r = Reservoir::Init();
#endif	
		
		WriteOutputReservoir(swizzledDTid, r, g_local.CurrTemporalReservoir_A_DescHeapIdx, g_local.CurrTemporalReservoir_B_DescHeapIdx,
			g_local.CurrTemporalReservoir_C_DescHeapIdx);
	}
	else if (isWithinScreenBounds)
	{
		Reservoir r = Reservoir::Init();
		WriteOutputReservoir(swizzledDTid, r, g_local.CurrTemporalReservoir_A_DescHeapIdx, g_local.CurrTemporalReservoir_B_DescHeapIdx,
			g_local.CurrTemporalReservoir_C_DescHeapIdx);
	}
}