#include "Reservoir_Diffuse.hlsli"
#include "../Common/Common.hlsli"
#include "../Common/FrameConstants.h"
#include "../../ZetaCore/Core/Material.h"
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
#define TOLERABLE_RELATIVE_RADIANCE_CHANGE 0.2
#define TOLERABLE_RELATIVE_RAY_T_CHANGE 0.35
#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.015f
#define RAY_OFFSET_VIEW_DIST_START 30.0

static const uint16_t2 GroupDim = uint16_t2(RGI_DIFF_TEMPORAL_GROUP_DIM_X, RGI_DIFF_TEMPORAL_GROUP_DIM_Y);

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_RGI_Diff_Temporal> g_local : register(b1);
RaytracingAccelerationStructure g_sceneBVH : register(t0);
ByteAddressBuffer g_materials : register(t1);
ByteAddressBuffer g_owenScrambledSobolSeq : register(t3);
ByteAddressBuffer g_scramblingTile : register(t4);
ByteAddressBuffer g_rankingTile : register(t5);
ByteAddressBuffer g_frameMeshData : register(t6);
StructuredBuffer<Vertex> g_sceneVertices : register(t7);
ByteAddressBuffer g_sceneIndices : register(t8);

//--------------------------------------------------------------------------------------
// 
//--------------------------------------------------------------------------------------

struct HitSurface
{
	float3 Pos;
	float2 uv;
	half2 ShadingNormal;
	uint16_t MatID;
};

#if RAY_BINNING
groupshared uint g_binOffset[NUM_BINS];
groupshared uint g_binIndex[NUM_BINS];
groupshared float3 g_sortedOrigin[RGI_DIFF_TEMPORAL_GROUP_DIM_X * RGI_DIFF_TEMPORAL_GROUP_DIM_Y];
groupshared float3 g_sortedDir[RGI_DIFF_TEMPORAL_GROUP_DIM_X * RGI_DIFF_TEMPORAL_GROUP_DIM_Y];
#endif

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

uint16_t2 SwizzleThreadGroup(uint3 DTid, uint3 Gid, uint3 GTid)
{
#if THREAD_GROUP_SWIZZLING
	const uint16_t groupIDFlattened = (uint16_t) Gid.y * g_local.DispatchDimX + (uint16_t) Gid.x;
	const uint16_t tileID = groupIDFlattened / g_local.NumGroupsInTile;
	const uint16_t groupIDinTileFlattened = groupIDFlattened % g_local.NumGroupsInTile;

	// TileWidth is a power of 2 for all tiles except possibly the last one
	const uint16_t numFullTiles = g_local.DispatchDimX / RGI_DIFF_TEMPORAL_TILE_WIDTH; // floor(DispatchDimX / TileWidth
	const uint16_t numGroupsInFullTiles = numFullTiles * g_local.NumGroupsInTile;

	uint16_t2 groupIDinTile;
	if (groupIDFlattened >= numGroupsInFullTiles)
	{
		const uint16_t lastTileDimX = g_local.DispatchDimX - RGI_DIFF_TEMPORAL_TILE_WIDTH * numFullTiles; // DispatchDimX & NumGroupsInTile
		groupIDinTile = uint16_t2(groupIDinTileFlattened % lastTileDimX, groupIDinTileFlattened / lastTileDimX);
	}
	else
		groupIDinTile = uint16_t2(groupIDinTileFlattened & (RGI_DIFF_TEMPORAL_TILE_WIDTH - 1), groupIDinTileFlattened >> RGI_DIFF_TEMPORAL_LOG2_TILE_WIDTH);

	const uint16_t swizzledGidFlattened = groupIDinTile.y * g_local.DispatchDimX + tileID * RGI_DIFF_TEMPORAL_TILE_WIDTH + groupIDinTile.x;
	const uint16_t2 swizzledGid = uint16_t2(swizzledGidFlattened % g_local.DispatchDimX, swizzledGidFlattened / g_local.DispatchDimX);
	const uint16_t2 swizzledDTid = swizzledGid * GroupDim + (uint16_t2) GTid.xy;
#else
	const uint16_t2 swizzledDTid = (uint16_t2)DTid.xy;
#endif

	return swizzledDTid;
}

float2 SignNotZero(float2 v)
{
	return float2((v.x >= 0.0) ? 1.0 : -1.0, (v.y >= 0.0) ? +1.0 : -1.0);
}

// wi is assumed to be normalized
float3 MissShading(float3 wi)
{
	Texture2D<half4> g_envMap = ResourceDescriptorHeap[g_frame.EnvMapDescHeapOffset];
	
	float2 thetaPhi = Math::SphericalFromCartesian(wi);
	float2 uv = float2(thetaPhi.y * ONE_OVER_2_PI, thetaPhi.x * ONE_OVER_PI);

	// undo non-linear sampling
	float s = thetaPhi.x >= PI_OVER_2 ? 1.0f : -1.0f;
	uv.y = (thetaPhi.x - PI_OVER_2) * 0.5f;
	uv.y = 0.5f + s * sqrt(abs(uv.y) * ONE_OVER_PI);
	
	float3 color = g_envMap.SampleLevel(g_samLinearClamp, uv, 0.0f).rgb;
	
	return color;
}

bool EvaluateVisibility(float3 pos, float3 wi, float3 normal)
{
	// protect against self-intersection
	float3 adjustedOrigin = pos + normal * 5e-3f;

	RayQuery < RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
		RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
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

	// Light source is occluded
	if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		return false;

	return true;
}

bool FindClosestHit(float3 pos, float3 wi, out HitSurface surface)
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
		const uint byteOffset = (rayQuery.CommittedGeometryIndex() + rayQuery.CommittedInstanceID()) * sizeof(RT::MeshInstance);
		const RT::MeshInstance meshData = g_frameMeshData.Load<RT::MeshInstance>(byteOffset);

		uint tri = rayQuery.CandidatePrimitiveIndex() * 3;
		tri += meshData.BaseIdxOffset;
		uint i0 = g_sceneIndices.Load<uint>(tri * sizeof(uint)) + meshData.BaseVtxOffset;
		uint i1 = g_sceneIndices.Load<uint>((tri + 1) * sizeof(uint)) + meshData.BaseVtxOffset;
		uint i2 = g_sceneIndices.Load<uint>((tri + 2) * sizeof(uint)) + meshData.BaseVtxOffset;

		Vertex V0 = g_sceneVertices[i0];
		Vertex V1 = g_sceneVertices[i1];
		Vertex V2 = g_sceneVertices[i2];

		const float2 barry = rayQuery.CommittedTriangleBarycentrics();
		float2 uv = V0.TexUV + barry.x * (V1.TexUV - V0.TexUV) + barry.y * (V2.TexUV - V0.TexUV);
		
		float3 normal = V0.NormalL + barry.x * (V1.NormalL - V0.NormalL) + barry.y * (V2.NormalL - V0.NormalL);
		normal = normalize(normal);
		normal = Math::Transform::RotateVector(normal, meshData.Rotation);

		// reverse the normal if ray hit the backfacing side
		if (dot(normal, -rayQuery.WorldRayDirection()) < 0)
			normal *= -1.0f;

		surface.Pos = rayQuery.WorldRayOrigin() + rayQuery.WorldRayDirection() * rayQuery.CommittedRayT();
		surface.uv = uv;
		surface.ShadingNormal = Math::Encoding::EncodeUnitNormal(normal);
		surface.MatID = meshData.MatID;

		return true;
	}

	return false;
}

bool Trace(uint Gidx, float3 origin, float3 dir, out HitSurface hitInfo, out bool isRayValid, 
	out uint16_t sortedIdx)
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
	sortedIdx = (uint16_t) idx;

	GroupMemoryBarrierWithGroupSync();

	float3 newOrigin = g_sortedOrigin[Gidx];
	float3 newDir = g_sortedDir[Gidx].xyz;
#else	
	float3 newOrigin = origin;
	float3 newDir = dir;
#endif	

	// skip invalid rays
	if (newDir.x == INVALID_RAY_DIR.x)
	{
		isRayValid = false;
		return false;
	}

	isRayValid = true;
	bool hit = FindClosestHit(newOrigin, newDir, hitInfo);

	return hit;
}

float3 DirectLighting(HitSurface hitInfo, float3 wo)
{
	float3 normal = Math::Encoding::DecodeUnitNormal(hitInfo.ShadingNormal);
	const uint byteOffset = hitInfo.MatID * sizeof(Material);
	const Material mat = g_materials.Load < Material > (byteOffset);

	const bool isUnoccluded = dot(-g_frame.SunDir, normal) <= 0 ? false : EvaluateVisibility(hitInfo.Pos, -g_frame.SunDir, normal);
	float3 L_o = 0.0.xxx;

	if (isUnoccluded)
	{
		float3 baseColor = mat.BaseColorFactor.rgb;
		if (mat.BaseColorTexture != -1)
		{
			uint offset = NonUniformResourceIndex(g_frame.BaseColorMapsDescHeapOffset + mat.BaseColorTexture);
			BASE_COLOR_MAP g_baseCol = ResourceDescriptorHeap[offset];
			float mip = g_frame.MipBias;
		
#if USE_RAY_CONES
			uint w;
			uint h;
			g_baseCol.GetDimensions(w, h);
			mip += g_frame.MipBias + RT::RayCone::TextureMipmapOffset(hitInfo.Lambda, w, h);
#endif			
			baseColor *= g_baseCol.SampleLevel(g_samLinearWrap, hitInfo.uv, mip).rgb;
		}

		float metalness = mat.MetallicFactor;
		if (mat.MetalnessRoughnessTexture != -1)
		{
			uint offset = NonUniformResourceIndex(g_frame.MetalnessRoughnessMapsDescHeapOffset + mat.MetalnessRoughnessTexture);
			METALNESS_ROUGHNESS_MAP g_metalnessRoughnessMap = ResourceDescriptorHeap[offset];
			float mip = g_frame.MipBias;

#if USE_RAY_CONES
			uint w;
			uint h;
			g_metalnessRoughnessMap.GetDimensions(w, h);
			mip += g_frame.MipBias + RT::RayCone::TextureMipmapOffset(hitInfo.Lambda, w, h);
#endif				
			metalness *= g_metalnessRoughnessMap.SampleLevel(g_samLinearWrap, hitInfo.uv, mip).r;
		}

		float ndotWi = saturate(dot(normal, -g_frame.SunDir));

		// assume the surface is Lambertian
		float3 diffuseReflectance = baseColor * (1.0f - metalness);
		float3 brdf = BRDF::LambertianBRDF(diffuseReflectance, ndotWi);

		const float3 sigma_t_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
		const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
		const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;

		float3 posW = hitInfo.Pos;
		posW.y += g_frame.PlanetRadius;
	
		float t = Volumetric::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, posW, -g_frame.SunDir);
		float3 tr = Volumetric::EstimateTransmittance(g_frame.PlanetRadius, posW, -g_frame.SunDir, t,
		sigma_t_rayleigh, sigma_t_mie, sigma_t_ozone, 6);
	
		L_o = brdf * tr * g_frame.SunIlluminance;
	}
	
	float3 L_e = mat.EmissiveFactor;

	if (mat.EmissiveTexture != -1)
	{
		uint offset = NonUniformResourceIndex(g_frame.EmissiveMapsDescHeapOffset + mat.EmissiveTexture);
		EMISSIVE_MAP g_emissiveMap = ResourceDescriptorHeap[offset];
		float mip = g_frame.MipBias;

#if USE_RAY_CONES
		uint w;
		uint h;
		g_emissiveMap.GetDimensions(w, h);
		mip += g_frame.MipBias + RT::RayCone::TextureMipmapOffset(hitInfo.Lambda, w, h);
#endif	
		L_e *= g_emissiveMap.SampleLevel(g_samLinearWrap, hitInfo.uv, mip).rgb;
	}

	return L_o + L_e;
}

DiffuseSample Li(uint2 DTid, uint Gidx, float3 posW, float3 normal, float3 wi, float linearDepth, out uint16_t sortedIdx)
{
	DiffuseSample ret;
	
	// protect against self-intersection
	float offsetScale = linearDepth / RAY_OFFSET_VIEW_DIST_START;
	float3 adjustedOrigin = posW + normal * 1e-2f * (1 + offsetScale * 2);

	// trace a ray along wi to find closest surface point
	bool isRayValid;
	HitSurface hitInfo;
	bool hit = Trace(Gidx, adjustedOrigin, wi, hitInfo, isRayValid, sortedIdx);

	// what's the outgoing radiance from the closest hit point towards wo
	ret.Lo = 0.0.xxx;

	// if the ray hit a surface, compute direct lighting at the given surface point, otherise 
	// returns the incoming radiance from sky
	if (hit)
	{
		ret.Lo = (half3) DirectLighting(hitInfo, -wi);
		ret.Pos = hitInfo.Pos;
		ret.Normal = hitInfo.ShadingNormal;
	}
	else if (isRayValid)
	{
#if RAY_BINNING
		float3 newDir = g_sortedDir[Gidx];
#else
		float3 newDir = wi;
#endif
		ret.Lo = MissShading(newDir);
		float3 temp = posW;
		temp.y += g_frame.PlanetRadius;
		float t = Volumetric::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, temp, newDir);
		ret.Pos = posW + t * newDir;
		ret.Normal = Math::Encoding::EncodeUnitNormal(-normalize(ret.Pos));
	}
	
	return ret;
}

DiffuseReservoir SampleTemporalReservoir(uint2 DTid, float3 currPos, float currLinearDepth, float3 currNormal)
{
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	
	// reverse reproject current pixel
	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
	const half2 motionVec = g_motionVector[DTid];
	const float2 currUV = (DTid + 0.5f) / renderDim;
	const float2 prevUV = currUV - motionVec;

	if (!Math::IsWithinBoundsInc(prevUV, 1.0f.xx))
	{
		DiffuseReservoir r = DiffuseReservoir::Init();
		return r;
	}

	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float prevDepth = Math::Transform::LinearDepthFromNDC(g_prevDepth.SampleLevel(g_samPointClamp, prevUV, 0), g_frame.CameraNear);
	
	const float3 prevPos = Math::Transform::WorldPosFromUV(prevUV, prevDepth, g_frame.TanHalfFOV, g_frame.AspectRatio,
			g_frame.PrevViewInv);
	
	const float planeDist = dot(currNormal, prevPos - currPos);
	const bool isDisoccluded = abs(planeDist) > DISOCCLUSION_TEST_RELATIVE_DELTA * currLinearDepth;
	
	if (isDisoccluded)
	{
		DiffuseReservoir r = DiffuseReservoir::Init();
		return r;		
	}
	
	// nearest neighbor
	//float2 posSS = prevUV * renderDim;
	//int2 pixel = round(posSS - 0.5f);

	DiffuseReservoir prevReservoir = RGI_Diff_Util::ReadInputReservoir(g_samPointClamp, prevUV, g_local.PrevTemporalReservoir_A_DescHeapIdx,
		g_local.PrevTemporalReservoir_B_DescHeapIdx, g_local.PrevTemporalReservoir_C_DescHeapIdx);

	return prevReservoir;
}

void Validate(DiffuseSample s, float3 posW, inout DiffuseReservoir r, inout RNG rng)
{
	// TODO not sure what's the best way to do this -- there are either 
	// false positives or false negatives
	const float3 d = r.SamplePos - posW;
	const float reservoirHitDist = sqrt(dot(d, d));
	const float currHitDist = s.RayT;
	float dt2 = abs(currHitDist - reservoirHitDist);
	dt2 *= dt2;
	const float relativeRayTChange = saturate(dt2 / max(currHitDist, g_frame.RayOffset));
	
	const float sampleLum = Math::Color::LuminanceFromLinearRGB(s.Lo);
	const float reservoirLum = Math::Color::LuminanceFromLinearRGB(r.Li);
	const float relativeRadianceChange = sampleLum == 0.0 ? saturate(abs(sampleLum - reservoirLum)) : 
		saturate(abs(sampleLum - reservoirLum) / max(sampleLum, 1e-4));
	
	if (relativeRadianceChange <= TOLERABLE_RELATIVE_RADIANCE_CHANGE)
	{
		r.SamplePos = s.Pos;
		r.SampleNormal = s.Normal;
		r.Li = half3(s.Lo);
	}
	else if (relativeRayTChange <= TOLERABLE_RELATIVE_RAY_T_CHANGE)
	{
		r.SamplePos = s.Pos;
		r.SampleNormal = s.Normal;
		r.Li = half3(s.Lo);

		float adjustment = 1 - smoothstep(TOLERABLE_RELATIVE_RADIANCE_CHANGE, 1, relativeRadianceChange);
		r.M = half(max(1, r.M * adjustment));
		r.w_sum *= adjustment;
	}
	else
	{
		const float sourcePdf = ONE_OVER_2_PI;
		const float risWeight = sampleLum / sourcePdf;

		r.SamplePos = s.Pos;
		r.SampleNormal = s.Normal;
		r.Li = half3(s.Lo);
		r.M = 1;
		
		// following should be the correct thing to do, but for some reason casuses artifacts
		// TODO investigate
	#if 0		
		r.w_sum = risWeight;
	#else
		r.w_sum = 0;
	#endif
	}
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RGI_DIFF_TEMPORAL_GROUP_DIM_X, RGI_DIFF_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint16_t2 swizzledDTid = SwizzleThreadGroup(DTid, Gid, GTid);

	// this lane should still participates in non-trace computations
	const bool isWithinScreenBounds = swizzledDTid.x < g_frame.RenderWidth && swizzledDTid.y < g_frame.RenderHeight;

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

	// this lane still participates in non-trace computations
	bool reservoirValid = isWithinScreenBounds && (depth != 0);
	
	// reconstruct position
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 posW = Math::Transform::WorldPosFromScreenSpace(swizzledDTid,
		renderDim,
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_normal[swizzledDTid]);

	// metallic mask
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float m = g_metalnessRoughness[swizzledDTid].r;
	
	// skip metals
	// metallic factor shoud be binary, but some scenes have invalid values, so instead of testing against 0,
	// add a small threshold
	reservoirValid &= (m <= MAX_METALNESS_DIELECTRIC);

	// validate the reservoirs from two frames ago
	const bool tracedLastFrame = g_local.CheckerboardTracing ?
		((swizzledDTid.x + swizzledDTid.y) & 0x1) == (g_local.FrameCounter & 0x1) :
		true;

	DiffuseReservoir r = SampleTemporalReservoir(swizzledDTid, posW, linearDepth, normal);
	
	// skip tracing if reservoir's sample is invalid
	bool isReservoirEmpty = r.M == 0;
	const bool needValidation = reservoirValid && !isReservoirEmpty &&
		(!g_local.CheckerboardTracing || !tracedLastFrame);
	
	const float3 wi = needValidation ? normalize(r.SamplePos - posW) : INVALID_RAY_DIR;
	
	uint16_t sortedIdx;
	DiffuseSample s = Li(swizzledDTid, Gidx, posW, normal, wi, linearDepth, sortedIdx);
	
#if RAY_BINNING
	g_sortedOrigin[Gidx] = RGI_Diff_Util::PackSample(s.Normal, s.Lo, s.RayT);
	g_sortedDir[Gidx] = s.Pos;
	
	GroupMemoryBarrierWithGroupSync();

	// retrieve the non-sorted vals
	RGI_Diff_Util::UnpackSample(g_sortedOrigin[sortedIdx], s.Normal, s.Lo, s.RayT);
	s.Pos = g_sortedDir[sortedIdx].xyz;
#endif	
	
	if (reservoirValid)
	{
		if (needValidation)
		{
			RNG rng = RNG::Init(swizzledDTid, g_frame.FrameNum, renderDim);
			Validate(s, posW, r, rng);		
		}

		RGI_Diff_Util::WriteOutputReservoir(swizzledDTid, r, g_local.CurrTemporalReservoir_A_DescHeapIdx, g_local.CurrTemporalReservoir_B_DescHeapIdx,
			g_local.CurrTemporalReservoir_C_DescHeapIdx);
	}
}
