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

#define THREAD_GROUP_SWIZZLING 1
#define INVALID_RAY_DIR 0.xxx
#define USE_RAY_CONES 1
#define RAY_CONE_K1 1.0f
#define RAY_CONE_K2 0.0f
#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.015f
#define RAY_OFFSET_VIEW_DIST_START 30.0

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_RGI_Spec_Temporal> g_local : register(b1);
RaytracingAccelerationStructure g_sceneBVH : register(t0);
StructuredBuffer<Material> g_materials : register(t1);
StructuredBuffer<uint> g_owenScrambledSobolSeq : register(t3);
StructuredBuffer<uint> g_scramblingTile : register(t4);
StructuredBuffer<uint> g_rankingTile : register(t5);
ByteAddressBuffer g_frameMeshData : register(t6);
StructuredBuffer<Vertex> g_sceneVertices : register(t7);
StructuredBuffer<uint> g_sceneIndices : register(t8);

//--------------------------------------------------------------------------------------
// 
//--------------------------------------------------------------------------------------

struct HitSurface
{
	float3 Pos;
	float2 uv;
	half2 Normal;
	uint16_t MatID;
	
#if USE_RAY_CONES
	half Lambda;
#endif
};

static const uint16_t2 GroupDim = uint16_t2(RGI_SPEC_TEMPORAL_GROUP_DIM_X, RGI_SPEC_TEMPORAL_GROUP_DIM_Y);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

uint2 SwizzleThreadGroup(uint3 DTid, uint3 Gid, uint3 GTid)
{
#if THREAD_GROUP_SWIZZLING
	const uint16_t groupIDFlattened = (uint16_t) Gid.y * g_local.DispatchDimX + (uint16_t) Gid.x;
	const uint16_t tileID = groupIDFlattened / g_local.NumGroupsInTile;
	const uint16_t groupIDinTileFlattened = groupIDFlattened % g_local.NumGroupsInTile;

	// TileWidth is a power of 2 for all tiles except possibly the last one
	const uint16_t numFullTiles = g_local.DispatchDimX / RGI_SPEC_TEMPORAL_TILE_WIDTH; // floor(DispatchDimX / TileWidth
	const uint16_t numGroupsInFullTiles = numFullTiles * g_local.NumGroupsInTile;

	uint16_t2 groupIDinTile;
	if (groupIDFlattened >= numGroupsInFullTiles)
	{
		// DispatchDimX & NumGroupsInTile
		const uint16_t lastTileDimX = g_local.DispatchDimX - RGI_SPEC_TEMPORAL_TILE_WIDTH * numFullTiles;
		groupIDinTile = uint16_t2(groupIDinTileFlattened % lastTileDimX, groupIDinTileFlattened / lastTileDimX);
	}
	else
	{
		groupIDinTile = uint16_t2(
			groupIDinTileFlattened & (RGI_SPEC_TEMPORAL_TILE_WIDTH - 1),
			groupIDinTileFlattened >> RGI_SPEC_TEMPORAL_LOG2_TILE_WIDTH);
	}

	const uint16_t swizzledGidFlattened = groupIDinTile.y * g_local.DispatchDimX + tileID * RGI_SPEC_TEMPORAL_TILE_WIDTH + groupIDinTile.x;
	const uint16_t2 swizzledGid = uint16_t2(swizzledGidFlattened % g_local.DispatchDimX, swizzledGidFlattened / g_local.DispatchDimX);
	const uint16_t2 swizzledDTid = swizzledGid * GroupDim + (uint16_t2) GTid.xy;
#else
	const uint16_t2 swizzledDTid = (uint16_t2) DTid.xy;
#endif
	
	return swizzledDTid;
}

// wi is assumed to be normalized
float3 MissShading(float3 wi)
{
	Texture2D<half4> g_envMap = ResourceDescriptorHeap[g_frame.EnvMapDescHeapOffset];
	
	float2 thetaPhi = Math::SphericalFromCartesian(wi);
	float2 uv = float2(thetaPhi.y * ONE_DIV_TWO_PI, thetaPhi.x * ONE_DIV_PI);

	// undo non-linear sampling
	float s = thetaPhi.x >= PI_DIV_2 ? 1.0f : -1.0f;
	uv.y = (thetaPhi.x - PI_DIV_2) * 0.5f;
	uv.y = 0.5f + s * sqrt(abs(uv.y) * ONE_DIV_PI);
	
	float3 color = g_envMap.SampleLevel(g_samLinearClamp, uv, 0.0f).rgb;
	
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

bool FindClosestHit(float3 pos, float3 wi, RT::RayCone rayCone, out HitSurface surface, out bool isRayValid)
{
	// skip invalid rays
	if (dot(wi - INVALID_RAY_DIR, 1) == 0.0f)
	{
		isRayValid = false;
		return false;
	}

	isRayValid = true;

	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_NON_OPAQUE> rayQuery;

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

		uint tri = rayQuery.CommittedPrimitiveIndex() * 3;
		tri += meshData.BaseIdxOffset;
		uint i0 = g_sceneIndices[tri] + meshData.BaseVtxOffset;
		uint i1 = g_sceneIndices[tri + 1] + meshData.BaseVtxOffset;
		uint i2 = g_sceneIndices[tri + 2] + meshData.BaseVtxOffset;

		Vertex V0 = g_sceneVertices[i0];
		Vertex V1 = g_sceneVertices[i1];
		Vertex V2 = g_sceneVertices[i2];

		const float2 barry = rayQuery.CommittedTriangleBarycentrics();
		float2 uv = V0.TexUV + barry.x * (V1.TexUV - V0.TexUV) + barry.y * (V2.TexUV - V0.TexUV);
		
		// transform to world space
		float3 normal = V0.NormalL + barry.x * (V1.NormalL - V0.NormalL) + barry.y * (V2.NormalL - V0.NormalL);
		normal = normalize(normal);
		normal = Math::Transform::RotateVector(normal, meshData.Rotation);
		
		// reverse the normal if ray hit the backfacing side
		if (dot(normal, -rayQuery.WorldRayDirection()) < 0)
			normal *= -1.0f;
				
		surface.Pos = rayQuery.WorldRayOrigin() + rayQuery.WorldRayDirection() * rayQuery.CommittedRayT();
		surface.uv = uv;
		surface.Normal = Math::Encoding::EncodeUnitNormal(normal);
		surface.MatID = meshData.MatID;
		
#if USE_RAY_CONES		
		rayCone.Update(rayQuery.CommittedRayT(), 0);
		
		// just need to apply the scale transformation, rotation and translation 
		// preserve area
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

float3 DirectLighting(HitSurface hitInfo, float3 wo)
{
	const float3 normal = Math::Encoding::DecodeUnitNormal(hitInfo.Normal);

	if (!EvaluateVisibility(hitInfo.Pos, -g_frame.SunDir, normal))
		return 0.0.xxx;

	const Material mat = g_materials[hitInfo.MatID];		
	
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
		//baseColor = mip > 6 ? float3(1, 0, 0) : float3(0, 0.5, 0.5);
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

	const float ndotWi = saturate(dot(normal, -g_frame.SunDir));

	// assume the surface is Lambertian
	const float3 diffuseReflectance = baseColor * (1.0f - metalness);
	const float3 brdf = BRDF::LambertianBRDF(diffuseReflectance, ndotWi);

	const float3 sigma_t_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
	const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
	const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;

	float3 posW = hitInfo.Pos;
	posW.y += g_frame.PlanetRadius;
	
	const float t = Volumetric::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, posW, -g_frame.SunDir);
	const float3 tr = Volumetric::EstimateTransmittance(g_frame.PlanetRadius, posW, -g_frame.SunDir, t,
		sigma_t_rayleigh, sigma_t_mie, sigma_t_ozone, 8);
	
	const float3 L_o = brdf * tr * g_frame.SunIlluminance;
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

SpecularSample Li(float3 posW, float3 normal, float3 wi, float linearDepth, RT::RayCone rayCone)
{
	SpecularSample ret;
		
	// TODO find a better way to protect against self-intersection
	float offsetScale = linearDepth / RAY_OFFSET_VIEW_DIST_START;
	float3 adjustedOrigin = posW + normal * 1e-2f * (1 + offsetScale * 2);
	
	// trace a ray along wi to find closest surface point
	bool isRayValid;
	HitSurface hitInfo;
	bool hit = FindClosestHit(adjustedOrigin, wi, rayCone, hitInfo, isRayValid);

	// incident radiance from closest hit point
	ret.Lo = 0.0.xxx;

	// if the ray hit a surface, compute direct lighting at the hit point, otherwise 
	// return the incoming sky radiance
	if (hit)
	{
		ret.Lo = (half3) DirectLighting(hitInfo, -wi);
		ret.Pos = hitInfo.Pos;
		ret.Normal = hitInfo.Normal;
	}
	else if (isRayValid)
	{
		ret.Lo = MissShading(wi);
		
		// account for transmittance
		float3 temp = posW; // make a copy to avoid accumulating floating-point error
		temp.y += g_frame.PlanetRadius;
		float t = Volumetric::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, temp, wi);
		ret.Pos = posW + t * wi;
		ret.Normal = Math::Encoding::EncodeUnitNormal(-normalize(ret.Pos));
	}
	
	return ret;
}

float4 PlaneHeuristic(float3 histPositions[4], float3 currNormal, float3 currPos, float linearDepth)
{
	float4 planeDist = float4(dot(currNormal, histPositions[0] - currPos),
		dot(currNormal, histPositions[1] - currPos),
		dot(currNormal, histPositions[2] - currPos),
		dot(currNormal, histPositions[3] - currPos));
	
	float4 weights = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA * linearDepth;
	
	return weights;
}

float RayTHeuristic(float3 reservoirSamplePos, float3 posW, float sampleRayT)
{
	float currRayT = length(reservoirSamplePos - posW);
	float relativeDiff = saturate(abs(currRayT - sampleRayT) / max(g_local.HitDistSigmaScale * currRayT, 1e-4));
	float w = 1.0 - relativeDiff;
	
	return w;
}

float GetTemporalM(float3 posW, float rayT, float localCurvature, float linearDepth)
{
	// smaller RIS weight during temporal resampling when 
	// 1. reflection is closer to ray origin
	// 2. surface is curved
	float d = length(posW);
	float f = rayT / max(rayT + d, 1e-6f);
	
	// TODO roughness can be considered as well
	const float maxCurvature = 0.1f;
	float curvedMScale = saturate(maxCurvature - abs(localCurvature)) / maxCurvature;
	curvedMScale *= curvedMScale;
	curvedMScale *= curvedMScale;
	curvedMScale = min(0.2, curvedMScale);
	float m_max = g_local.M_max * curvedMScale;
	
	float M = smoothstep(0, 1, f) * m_max;
	M = max(1, M);
	
	return M;
}

// Ref: D. Zhdan, "Fast Denoising with Self-Stabilizing Recurrent Blurs," GDC, 2020.
float4 RoughnessHeuristic(float currRoughness, float4 sampleRoughness)
{
	float n = currRoughness * currRoughness * 0.99f + 0.01f;
	float4 w = abs(currRoughness - sampleRoughness) / n;
	w = saturate(1.0f - w);
	w *= sampleRoughness <= g_local.RoughnessCutoff;
	
	return w;
}

void TemporalResample(uint2 DTid, float3 posW, float3 normal, float linearDepth, BRDF::SurfaceInteraction surface,
	float3 baseColor, float isMetallic, float roughness, float localCurvature, inout SpecularReservoir r, inout RNG rng)
{
	if (roughness <= g_local.MinRoughnessResample)
		return;
	
	const bool emptyReservoir = r.M == 0;
	
	// reverse reproject current pixel
	float reflectionRayT;
	float2 prevUV = RGI_Spec_Util::VirtualMotionReproject(posW, roughness, surface, r.SamplePos, localCurvature, linearDepth, 
		g_frame.TanHalfFOV, g_frame.PrevViewProj, reflectionRayT);
	
	if (!Math::IsWithinBoundsInc(prevUV, 1.0f.xx))
		return;

	// combine 2x2 neighborhood reservoirs around reprojected pixel
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 f = prevUV * renderDim;
	const float2 topLeft = floor(f - 0.5f); // e.g if p0 is at (20.5, 30.5), then topLeft would be (20, 30)
	const float2 offset = f - (topLeft + 0.5f);
	const float2 topLeftTexelUV = (topLeft + 0.5f) / renderDim;

	const uint2 offsets[4] = { uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(1, 1) };	
	float4 weights = 1.0.xxxx;
	
	// screen-bounds check
	[unroll]
	for (int prevIdx = 0; prevIdx < 4; prevIdx++)
	{
		const uint2 prevPixel = uint2(topLeft) + offsets[prevIdx];
		
		if (prevPixel.x >= renderDim.x || prevPixel.y >= renderDim.y)
			weights[prevIdx] = 0.0f;
	}

	// plane-based heuristic
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float4 prevDepths = g_prevDepth.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	prevDepths = Math::Transform::LinearDepthFromNDC(prevDepths, g_frame.CameraNear);

	float3 prevPos[4];
	
	[unroll]
	for (int posIdx = 0; posIdx < 4; posIdx++)
	{
		float2 prevUV = topLeftTexelUV + offsets[posIdx] / renderDim;
		prevPos[posIdx] = Math::Transform::WorldPosFromUV(prevUV, 
			prevDepths[posIdx], 
			g_frame.TanHalfFOV, 
			g_frame.AspectRatio,
			g_frame.PrevViewInv);
	}
	
	weights *= PlaneHeuristic(prevPos, normal, posW, linearDepth);

	// roughness heuristic
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float4 prevRoughnesses = g_metalnessRoughness.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
	
	weights *= RoughnessHeuristic(roughness, prevRoughnesses);
	
	if (dot(1, weights) < 1e-4)
		return;

	// hit-distance heuristic
	const float currRayT = length(r.SamplePos - posW);
	float4 rayTWeights = 0.0.xxxx;
	
	Texture2D<float4> g_reservoir_A = ResourceDescriptorHeap[g_local.PrevTemporalReservoir_A_DescHeapIdx];
	Texture2D<half4> g_reservoir_B = ResourceDescriptorHeap[g_local.PrevTemporalReservoir_B_DescHeapIdx];
	Texture2D<half2> g_reservoir_C = ResourceDescriptorHeap[g_local.PrevTemporalReservoir_C_DescHeapIdx];
	Texture2D<half4> g_reservoir_D = ResourceDescriptorHeap[g_local.PrevTemporalReservoir_D_DescHeapIdx];
	// defer reading the rest of reservoir data to relieve register pressure
	float3 prevReservoirSamplePos[4];

	[unroll]
	for (int resIdx = 0; resIdx < 4; resIdx++)
	{
		const uint2 prevPixel = uint2(topLeft) + offsets[resIdx];
		if (weights[resIdx] == 0.0f)
			continue;

		prevReservoirSamplePos[resIdx] = g_reservoir_A[prevPixel].xyz;
		
		const float3 distToNeighborSample = posW - prevReservoirSamplePos[resIdx];
		const float sampleRayT = length(distToNeighborSample);

		// TODO hit-distance weight leads to temporal artifacts for curved surfaces
		rayTWeights[resIdx] = localCurvature != 0.0 ? 1.0f : RayTHeuristic(r.SamplePos, posW, sampleRayT);
	}
	
	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);
	
	rayTWeights = emptyReservoir ? 1.0.xxxx : rayTWeights;
	weights *= rayTWeights * bilinearWeights;
	weights *= weights > 1e-4;
	const float weightSum = dot(1, weights);
	
	if (weightSum < 1e-4)
		return;
	
	weights /= weightSum;
	
	// smaller RIS weight when hit distance is small (sharper reflections)
	const float temporalM = emptyReservoir ? g_local.M_max : GetTemporalM(posW, currRayT, localCurvature, linearDepth);
		
	[unroll]
	for (int k = 0; k < 4; k++)
	{	
		if (weights[k] == 0.0f)
			continue;

		const uint2 prevPixel = uint2(topLeft) + offsets[k];
		
		SpecularReservoir prev;
		prev.SamplePos = prevReservoirSamplePos[k];
		prev.Li = g_reservoir_B[prevPixel].rgb;
		prev.M = g_reservoir_B[prevPixel].w;
		prev.W = g_reservoir_D[prevPixel].a;
		prev.SampleNormal = g_reservoir_C[prevPixel];
		
		// q -> reused path, r -> current pixel's path
		const float3 secondToFirst_r = posW - prev.SamplePos;
		const float3 wi = normalize(-secondToFirst_r);
		const float3 brdfCostheta_r = RGI_Spec_Util::RecalculateSpecularBRDF(wi, baseColor, isMetallic, surface);
		float jacobianDet = 1.0f;

		if (g_local.PdfCorrection)
			jacobianDet = RGI_Spec_Util::JacobianDeterminant(prevPos[k], prev.SamplePos, wi, secondToFirst_r, prev);

		r.Combine(prev, temporalM, weights[k], jacobianDet, brdfCostheta_r, rng);
	}
}

SpecularReservoir UpdateAndResample(uint2 DTid, float3 posW, float3 normal, float linearDepth, SpecularSample s,
	BRDF::SurfaceInteraction surface, float3 baseColor, float isMetallic, float roughness, bool isSampleValid, 
	float localCurvature, inout RNG rng)
{
	// initialize the reservoir with the newly traced sample
	SpecularReservoir r = SpecularReservoir::Init();

	if (isSampleValid)
	{
		const float3 brdfCosthetaDivPdf = BRDF::SpecularBRDFGGXSmithDivPdf(surface);
		const float3 brdfCostheta = BRDF::SpecularBRDFGGXSmith(surface);
		
		// target = Li(-wi) * BRDF(wi, wo) * |ndotwi|
		// source = Pdf(wi)
		const float risWeight = Math::Color::LuminanceFromLinearRGB(s.Lo * brdfCosthetaDivPdf);
	
		r.Update(risWeight, s, brdfCostheta, rng);
	}
		
	if (g_local.DoTemporalResampling && g_local.IsTemporalReservoirValid)
		TemporalResample(DTid, posW, normal, linearDepth, surface, baseColor, isMetallic, roughness, localCurvature, r, rng);
	
	return r;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RGI_SPEC_TEMPORAL_GROUP_DIM_X, RGI_SPEC_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint2 swizzledDTid = SwizzleThreadGroup(DTid, Gid, GTid);

	if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
		return;

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[swizzledDTid.xy];

	if(depth == 0)
		return;
		
	// reconstruct position from depth buffer
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 posW = Math::Transform::WorldPosFromScreenSpace(swizzledDTid.xy,
		renderDim,
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	// shading normal
	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_normal[swizzledDTid.xy]);
		
	// roughness and metallic mask
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float2 mr = g_metalnessRoughness[swizzledDTid.xy];

	// roughness cutoff
	const bool roughnessBelowThresh = mr.y <= g_local.RoughnessCutoff;
	if(!roughnessBelowThresh)
		return;
	
	const bool traceThisFrame = true;

	// sample the incident direction using the distribution of visible normals (VNDF)
	const float3 wo = normalize(g_frame.CameraPos - posW);
	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::InitPartial(normal, mr.y, wo);
	float3 wi = INVALID_RAY_DIR;
	
	if (traceThisFrame)
	{
		const float u0 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			swizzledDTid.x, swizzledDTid.y, g_local.SampleIndex, 4);
		const float u1 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			swizzledDTid.x, swizzledDTid.y, g_local.SampleIndex, 5);

		wi = BRDF::SampleSpecularBRDFGGXSmith(surface, float2(u0, u1));
	}
	
	Texture2D<float> g_curvature = ResourceDescriptorHeap[g_local.CurvatureSRVDescHeapIdx];	
	const float k = g_curvature[swizzledDTid.xy];
	
#if USE_RAY_CONES
	const float phi = RAY_CONE_K1 * k + RAY_CONE_K2;
	RT::RayCone rayCone = RT::RayCone::InitFromGBuffer(g_frame.PixelSpreadAngle, phi, linearDepth);
#else
	RT::RayCone rayCone = RT::RayCone::InitFromGBuffer(g_frame.PixelSpreadAngle, 0, linearDepth);
#endif	
	
	SpecularSample tracedSample = Li(posW, normal, wi, linearDepth, rayCone);
	
	// resampling

	// base color is needed for Fresnel
	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float3 baseColor = g_baseColor[swizzledDTid.xy].rgb;
		
	surface.InitComplete(wi, baseColor, mr.x);
		
	RNG rng = RNG::Init(swizzledDTid.xy, g_frame.FrameNum, renderDim);
	SpecularReservoir r = UpdateAndResample(swizzledDTid.xy, posW, normal, linearDepth, tracedSample, surface,
		baseColor, mr.x, mr.y, traceThisFrame, k, rng);
		
	RGI_Spec_Util::WriteReservoir(swizzledDTid.xy, r, g_local.CurrTemporalReservoir_A_DescHeapIdx,
			g_local.CurrTemporalReservoir_B_DescHeapIdx,
			g_local.CurrTemporalReservoir_C_DescHeapIdx,
			g_local.CurrTemporalReservoir_D_DescHeapIdx);
}