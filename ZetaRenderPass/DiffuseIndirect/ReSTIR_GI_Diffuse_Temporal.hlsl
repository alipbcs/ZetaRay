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
#define USE_RAY_CONES 0
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

#if USE_RAY_CONES
	half Lambda;
#endif
};

#if RAY_BINNING
groupshared uint g_binOffset[NUM_BINS];
groupshared uint g_binIndex[NUM_BINS];
groupshared float3 g_sortedOrigin[RGI_DIFF_TEMPORAL_GROUP_DIM_X * RGI_DIFF_TEMPORAL_GROUP_DIM_Y];
groupshared float3 g_sortedDir[RGI_DIFF_TEMPORAL_GROUP_DIM_X * RGI_DIFF_TEMPORAL_GROUP_DIM_Y];
#endif

#if USE_RAY_CONES
groupshared RT::RayCone g_sortedRayCones[RGI_DIFF_TEMPORAL_GROUP_DIM_X * RGI_DIFF_TEMPORAL_GROUP_DIM_Y];
#endif

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

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

	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
		RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
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
		const uint byteOffset = (rayQuery.CommittedGeometryIndex() + rayQuery.CommittedInstanceID()) * sizeof(RT::MeshInstance);
		const RT::MeshInstance meshData = g_frameMeshData.Load < RT::MeshInstance > (byteOffset);

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

#if USE_RAY_CONES		
		rayCone.Update(rayQuery.CommittedRayT(), 0);
		
		float3 v0W = meshData.Scale * V0.PosL;
		float3 v1W = meshData.Scale * V1.PosL;
		float3 v2W = meshData.Scale * V2.PosL;
		
		float ndotwo = dot(normal, -wi);
		float lambda = rayCone.Lambda(v0W, v1W, v2W, V0.TexUV, V1.TexUV, V2.TexUV, ndotwo);
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
	float3 normal = Math::Encoding::DecodeUnitNormal(hitInfo.ShadingNormal);
	const uint byteOffset = hitInfo.MatID * sizeof(Material);
	const Material mat = g_materials.Load<Material>(byteOffset);

	const bool isUnoccluded = dot(-g_frame.SunDir, normal) <= 0 ? false : EvaluateVisibility(hitInfo.Pos, -g_frame.SunDir, normal);
	float3 L_o = 0.0.xxx;

	if (isUnoccluded)
	{
		float3 baseColor = Math::Color::UnpackRGB(mat.BaseColorFactor);
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

		float metalness = mat.GetMetalness();
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
	
	float3 L_e = Math::Color::UnpackRGB(mat.EmissiveFactorNormalScale);

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

bool Li(uint2 DTid, uint Gidx, float3 posW, float3 normal, float3 wi, float linearDepth,
	RT::RayCone rayCone, out uint16_t sortedIdx, out DiffuseSample ret)
{
	float offsetScale = linearDepth / RAY_OFFSET_VIEW_DIST_START;
	float3 adjustedOrigin = posW + normal * 1e-2f * (1 + offsetScale * 2);

	// trace a ray along wi to find closest surface point
	bool isSortedRayValid;
	HitSurface hitInfo;
	bool hit = Trace(Gidx, adjustedOrigin, wi, rayCone, hitInfo, isSortedRayValid, sortedIdx);

	// what's the incident radiance from the closest hit point
	ret.Lo = 0.0.xxx;
	ret.Pos = INVALID_SAMPLE_POS;
	ret.Normal = INVALID_SAMPLE_NORMAL;

	// if the ray hit a surface, compute direct lighting at the hit point, otherwise 
	// return the incoming radiance from the sky
	if (hit)
	{
		ret.Lo = (half3) DirectLighting(hitInfo, -wi);
		ret.Pos = hitInfo.Pos;
		ret.Normal = hitInfo.ShadingNormal;
		
		return true;
	}
#if SKY_MISS_SHADING
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
		ret.Normal = Math::Encoding::EncodeUnitNormal(-normalize(ret.Pos));
		
		return true;
	}
#endif
	
	return false;
}

float4 GeometricHeuristic(float3 histPositions[4], float3 currNormal, float3 currPos, float linearDepth)
{
	float4 planeDist = float4(dot(currNormal, histPositions[0] - currPos),
		dot(currNormal, histPositions[1] - currPos),
		dot(currNormal, histPositions[2] - currPos),
		dot(currNormal, histPositions[3] - currPos));
	
	float4 weights = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA * linearDepth;
	
	return weights;
}

void TemporalResample(uint2 DTid, float3 posW, float3 normal, float linearDepth, inout DiffuseReservoir r, inout RNG rng)
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
	
	const float4 geoWeights = GeometricHeuristic(prevPos, normal, posW, linearDepth);
	
	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);
	
	float4 weights = geoWeights * bilinearWeights;
	weights *= weights > 1e-3;
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
		
		DiffuseReservoir prevReservoir = RGI_Diff_Util::ReadInputReservoir(prevPixel, g_local.PrevTemporalReservoir_A_DescHeapIdx,
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
			jacobianDet = RGI_Diff_Util::JacobianDeterminant(x1_q, x2_q, wi, secondToFirst_r, prevReservoir);
		
		r.Combine(prevReservoir, wi, normal, MAX_TEMPORAL_M, weights[i], jacobianDet, rng);
	}
}

DiffuseReservoir UpdateAndResample(uint2 DTid, float3 posW, float3 normal, float linearDepth, float sourcePdf,
	DiffuseSample s, bool isSampleValid, bool hit, inout RNG rng)
{
	DiffuseReservoir r = DiffuseReservoir::Init();

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
		TemporalResample(DTid, posW, normal, linearDepth, r, rng);
	
	return r;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[WaveSize(32)]
[numthreads(RGI_DIFF_TEMPORAL_GROUP_DIM_X, RGI_DIFF_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, GroupDim, g_local.DispatchDimX,
		RGI_DIFF_TEMPORAL_TILE_WIDTH, RGI_DIFF_TEMPORAL_LOG2_TILE_WIDTH, g_local.NumGroupsInTile);
#else
	const uint2 swizzledDTid = DTid.xy;
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
	bool isPixelValid = isWithinScreenBounds && (depth != 0);
	
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
	const half2 encodedNormal = g_normal[swizzledDTid];
	const float3 normal = Math::Encoding::DecodeUnitNormal(encodedNormal);
	
	// metallic mask
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float m = g_metalnessRoughness[swizzledDTid].r;
	
	// skip metallic surfaces
	// metallic factor shoud be binary, but some scenes have invalid values, so instead of testing against 0,
	// add a small threshold
	isPixelValid &= (m < MIN_METALNESS_METAL);

	// sample the cosine-weighted hemisphere above pos
	float3 wi = INVALID_RAY_DIR;
	float pdf;
	
	if (isPixelValid && traceThisFrame)
	{
		const float u0 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			swizzledDTid.x, swizzledDTid.y, g_local.SampleIndex, 0);
		const float u1 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			swizzledDTid.x, swizzledDTid.y, g_local.SampleIndex, 1);

#if COSINE_WEIGHTED_SAMPLING
		wi = BRDF::SampleLambertianBrdf(normal, float2(u0, u1), pdf);
#else
		wi = Sampling::UniformSampleHemisphere(float2(u0, u1), pdf);
		
		float3 T;
		float3 B;
		Math::Transform::revisedONB(normal, T, B);
		wi = wi.x * T + wi.y * B + wi.z * normal;
#endif
	}
	
#if USE_RAY_CONES
	uint n = uint(asuint16(encodedNormal.y)) << 16 | uint(asuint16(encodedNormal.x));
	g_sortedOrigin[Gidx].xy = float2(asfloat(n), linearDepth);
	g_sortedDir[Gidx] = posW;

	GroupMemoryBarrierWithGroupSync();
	
	const float k = EstimateLocalCurvature(normal, posW, linearDepth, GTid.xy, isPixelValid);
	RT::RayCone rayCone = RT::RayCone::InitFromGBuffer(g_frame.PixelSpreadAngle, k, linearDepth);
#else
	RT::RayCone rayCone = RT::RayCone::InitFromGBuffer(g_frame.PixelSpreadAngle, 0, linearDepth);
#endif	
	
	uint16_t sortedIdx;
	DiffuseSample retSample;
	bool hit = Li(swizzledDTid, Gidx, posW, normal, wi, linearDepth, rayCone, sortedIdx, retSample);
	
#if RAY_BINNING
	g_sortedOrigin[Gidx] = RGI_Diff_Util::PackSample(retSample.Normal, retSample.Lo, retSample.RayT);
	g_sortedDir[Gidx] = retSample.Pos;
	
	GroupMemoryBarrierWithGroupSync();

	// retrieve the non-sorted vals
	RGI_Diff_Util::UnpackSample(g_sortedOrigin[sortedIdx], retSample.Normal, retSample.Lo, retSample.RayT);
	retSample.Pos = g_sortedDir[sortedIdx].xyz;
#endif	
	
	// resampling
	if (isPixelValid)
	{
		DiffuseReservoir r = DiffuseReservoir::Init();

		RNG rng = RNG::Init(swizzledDTid, g_local.FrameCounter, renderDim);
						
		//const float cosTheta = saturate(pdf * PI);
		r = UpdateAndResample(swizzledDTid, posW, normal, linearDepth, pdf, retSample, traceThisFrame, hit, rng);
		
		// TODO under checkerboarding, result can be NaN when ray binning is enabled.
		// Need further investigation to figure out the cause. Following seems to mitigate
		// the issue for now with no apparent artifacts
#if RAY_BINNING
		if (isnan(r.SamplePos.x) && !traceThisFrame)
			r = DiffuseReservoir::Init();
#endif	

		// TODO when sun is at the horizon, there could be NaN propagation -- temporary
		// fix
		if (isnan(r.w_sum))
			r = DiffuseReservoir::Init();
		
		RGI_Diff_Util::WriteOutputReservoir(swizzledDTid, r, g_local.CurrTemporalReservoir_A_DescHeapIdx, g_local.CurrTemporalReservoir_B_DescHeapIdx,
		g_local.CurrTemporalReservoir_C_DescHeapIdx);
	}
}