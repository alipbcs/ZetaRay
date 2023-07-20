#include "SkyDI_Reservoir.hlsli"
#include "../../Common/Common.hlsli"
#include "../../Common/FrameConstants.h"
#include "../../Common/GBuffers.hlsli"
#include "../../Common/BRDF.hlsli"
#include "../../Common/Sampling.hlsli"
#include "../../Common/StaticTextureSamplers.hlsli"
#include "../../Common/RT.hlsli"
#include "../../Common/VolumetricLighting.hlsli"

#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.005f
#define RAY_OFFSET_VIEW_DIST_START 30.0
#define INCLUDE_VISIBILITY_IN_TARGET 1
#define NUM_RIS_CANDIDATES_MIS 2
#define NUM_RIS_CANDIDATES_VNDF 1
#define WAVE_SIZE 32
#define MAX_RAY_DIR_HEURISTIC_EXP 64.0f
#define THREAD_GROUP_SWIZZLING 1
#define MAX_W_SUM 5.0f
#define MIN_ROUGHNESS_SURFACE_MOTION 0.4
#define CHECKERBOARD_SORT 0

groupshared float g_firstMoment[WAVE_SIZE];
groupshared float g_secondMoment[WAVE_SIZE];

#if CHECKERBOARD_SORT
groupshared uint16_t2 g_dtid[SKY_DI_TEMPORAL_GROUP_DIM_Y * SKY_DI_TEMPORAL_GROUP_DIM_X];
#endif

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_SkyDI_Temporal> g_local : register(b1);
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
	
	// TODO find a better way to protect against self-intersection
	float offsetScale = linearDepth / RAY_OFFSET_VIEW_DIST_START;
	float3 adjustedOrigin = pos + normal * 1e-2f * (1 + offsetScale * 2);

	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
		RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
		RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
		RAY_FLAG_FORCE_OPAQUE> rayQuery;

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

float ComputeTarget(float3 pos, float3 normal, float linearDepth, float3 wi, 
	inout BRDF::SurfaceInteraction surface, out float3 Lo)
{
	const float3 brdfCostheta = BRDF::ComputeSurfaceBRDF(surface, true);

	// sample sky-view LUT
	Texture2D<float4> g_envMap = ResourceDescriptorHeap[g_frame.EnvMapDescHeapOffset];
	const float2 thetaPhi = Math::SphericalFromCartesian(wi);
	float2 uv = float2(thetaPhi.y * ONE_OVER_2_PI, thetaPhi.x * ONE_OVER_PI);

	// undo non-linear sampling
	const float s = thetaPhi.x >= PI_OVER_2 ? 1.0f : -1.0f;
	uv.y = (thetaPhi.x - PI_OVER_2) * 0.5f;
	uv.y = 0.5f + s * sqrt(abs(uv.y) * ONE_OVER_PI);
	
	Lo = g_envMap.SampleLevel(g_samLinearClamp, uv, 0.0f).rgb;
		
#if INCLUDE_VISIBILITY_IN_TARGET
	const bool vis = EvaluateVisibility(pos, wi, normal, linearDepth);
#else
	const bool vis = true;
#endif
		
	const float target = Math::Color::LuminanceFromLinearRGB(Lo * brdfCostheta * vis);

	return target;
}

float2 GetSample(uint2 DTid, uint numSamplesPerFrame, uint offset)
{
	const uint sampleIdx = (g_local.SampleIndex * numSamplesPerFrame + offset) & 31;
	
	const float u0 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			DTid.x, DTid.y, sampleIdx, 6);
	const float u1 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			DTid.x, DTid.y, sampleIdx, 7);
		
	return float2(u0, u1);
}

DIReservoir GenerateCandidatesVNDF(uint2 DTid, float3 posW, float3 normal, float linearDepth, float3 baseColor,
	float metalness, float roughness, inout BRDF::SurfaceInteraction surface, inout RNG rng)
{
	DIReservoir r = DIReservoir::Init();

	[unroll]
	for (int i = 0; i < NUM_RIS_CANDIDATES_VNDF; i++)
	{
		float2 u = GetSample(DTid, NUM_RIS_CANDIDATES_VNDF, i);
		float3 wi = BRDF::SampleSpecularBRDFGGXSmith(surface, normal, u);
		surface.InitComplete(wi, baseColor, metalness, normal);
		float sourcePdf = BRDF::SpecularBRDFGGXSmithPdf(surface);

		float3 Lo;
		const float target = ComputeTarget(posW, normal, linearDepth, wi, surface, Lo);
		const float risWeight = target / max(sourcePdf, 1e-5);
	
		r.Update(risWeight, Lo, wi, target, rng);
	}
	
	r.ComputeW(MAX_W_SUM);
	
#if !INCLUDE_VISIBILITY_IN_TARGET
	if (!EvaluateVisibility(posW, r.wi, normal, linearDepth))
		r.W = 0;
#endif	

	return r;
}

DIReservoir GenerateCandidatesMIS(uint2 DTid, float3 posW, float3 normal, float linearDepth, float3 baseColor, float metalness,
	float roughness, inout BRDF::SurfaceInteraction surface, inout RNG rng)
{
	DIReservoir r = DIReservoir::Init();	
	float3 wi_z;
	float p_z;
	float p_other_z;

	// MIS -- take a light sample and a brdf sample, then combine them using the balance heuristic
	float rhoDiffuse = Math::Color::LuminanceFromLinearRGB(baseColor * (1 - metalness));
	
	// sample env. map
	{
		const float2 u = GetSample(DTid, NUM_RIS_CANDIDATES_MIS, 0);
		float lightPdf;
		
#if 0
		float3 lightWi = Sampling::SampleCosineWeightedHemisphere(u, lightPdf);
		float3 lightWi = float3(lightWi.x, lightWi.z, -lightWi.y);
#else
		float3 lightWi = BRDF::SampleLambertianBrdf(normal, u, lightPdf);
#endif
		// compute brdf pdf with light sample
		surface.InitComplete(lightWi, baseColor, metalness, normal);
		const float F = saturate(Math::Color::LuminanceFromLinearRGB(surface.F));
		p_other_z = F * BRDF::SpecularBRDFGGXSmithPdf(surface) + (1 - F) * saturate(dot(lightWi, normal)) * ONE_OVER_PI;

		// resample
		float3 Lo;
		const float target = ComputeTarget(posW, normal, linearDepth, lightWi, surface, Lo);
		const float risWeight = target / lightPdf;
		r.Update(risWeight, Lo, lightWi, target, rng);
	
		wi_z = lightWi;
		p_z = lightPdf;
	}
	
	// sample brdf
	{
		const float2 u = GetSample(DTid, NUM_RIS_CANDIDATES_MIS, 1);
		float3 brdfWi;
		float brdfPdf;
		
		// decide between diffuse and specular brdfs using (estimated -- wi is unknown at this point) Fresnel
		const float F_est = mad(0.0926187f, exp(-2.95942632f * surface.ndotwo), 0.11994875f);
		const float pSpecular = metalness > MIN_METALNESS_METAL ? 1.0f : F_est / max(F_est + rhoDiffuse, 1e-4f);
		
		if (rng.Uniform() < pSpecular)
		{
			brdfWi = BRDF::SampleSpecularBRDFGGXSmith(surface, normal, u);
			surface.InitComplete(brdfWi, baseColor, metalness, normal);
			brdfPdf = BRDF::SpecularBRDFGGXSmithPdf(surface);
			
			brdfPdf *= pSpecular;
		}
		else
		{
			brdfWi = BRDF::SampleLambertianBrdf(normal, u, brdfPdf);
			surface.InitComplete(brdfWi, baseColor, metalness, normal);
			
			brdfPdf *= (1 - pSpecular);
		}
		
		// also compute light pdf with brdf sample
#if 0
		p_other_z = saturate(brdfWi.y) * ONE_OVER_PI;
#else
		p_other_z = saturate(dot(brdfWi, normal)) * ONE_OVER_PI;
#endif
		
		// resample
		float3 Lo;
		const float target = ComputeTarget(posW, normal, linearDepth, brdfWi, surface, Lo);
		const float risWeightSpecular = target / max(brdfPdf, 1e-4);
		const bool wasCandidatePicked = r.Update(risWeightSpecular, Lo, brdfWi, target, rng);
		
		p_z = wasCandidatePicked ? brdfPdf : p_z;
		wi_z = wasCandidatePicked ? brdfWi : wi_z;
	}
	
	// balance heuristic
	const float m = p_z / max(p_z + p_other_z, 1e-6);
	
	// compute W
	r.ComputeW(MAX_W_SUM, m);
	
#if !INCLUDE_VISIBILITY_IN_TARGET
	if (!EvaluateVisibility(posW, r.wi, normal, linearDepth))
		r.W = 0;
#endif	
	
	return r;
}

float2 MeanStdFromMoments(float firstMoment, float secondMoment, float N)
{
	float mean = firstMoment / N;
	float std = abs(secondMoment - (firstMoment * firstMoment) / N);
	// apply Bessel's correction to get an unbiased sample variance
	std /= (N - 1.0f);
	std = sqrt(std);

	return float2(mean, std);
}

bool IsLaneActive(uint laneIdx, uint4 activeMask)
{
	const uint laneIdxMod32 = laneIdx & 31;
	const uint laneIdxDiv32 = laneIdx >> 5;
	
	bool ret = ((activeMask.x & laneIdxMod32) == laneIdxMod32) * (laneIdxDiv32 == 0);

	return ret;
}

// Note: causes a darkening bias
bool PrefilterOutliers(uint gidx, float3 posW, float roughness, float linearDepth, inout DIReservoir r)
{
	const float N = SKY_DI_TEMPORAL_GROUP_DIM_X * SKY_DI_TEMPORAL_GROUP_DIM_Y;
	const uint waveIdx = gidx / WaveGetLaneCount();
	const uint numWaves = N / WaveGetLaneCount();
	const uint laneIdx = WaveGetLaneIndex();

	// fit a normal distribution to data as a way to detect outliers
	float wSumFirstMoment = WaveActiveSum(r.w_sum);
	float wSumSecondMoment = WaveActiveSum(r.w_sum * r.w_sum);

	if (laneIdx == 0)
	{
		g_firstMoment[waveIdx] = wSumFirstMoment;
		g_secondMoment[waveIdx] = wSumSecondMoment;
	}
	
	// heuristic to detect object boundaries; subdivide wave vertically into four 2x4 regions and
	// check to see if all the points lie on the same plane
	const bool le = (laneIdx & 7) < 4;
	const uint quadrant = (gidx & 15) < 8 ? (le ? 0 : 1) : (le ? 2 : 3);
	const uint idx = quadrant * 4;
	
	// 0  ... 7  8  ... 15
	// 16 ... 23 24 ... 31
	const uint4 waveActiveLanes = WaveActiveBallot(true);
	const bool p0Valid = IsLaneActive(idx, waveActiveLanes);
	const bool p1Valid = IsLaneActive(idx + 1, waveActiveLanes);
	const bool p2Valid = IsLaneActive(idx + 16, waveActiveLanes);
	const float3 P0 = p0Valid ? WaveReadLaneAt(posW, idx) : 0.0.xxx;
	const float3 P1 = p1Valid ? WaveReadLaneAt(posW, idx + 1) : 0.0.xxx;
	const float3 P2 = p2Valid ? WaveReadLaneAt(posW, idx + 16) : 0.0.xxx;
	const float3 planeNormal = normalize(cross(P1 - P0, P2 - P0));
	const float distFromPlane = dot(posW - P0, planeNormal);
	bool liesOnPlane = (p0Valid && p1Valid && p2Valid) ? (abs(distFromPlane) / linearDepth) < 2e-1 : false;
	
	GroupMemoryBarrierWithGroupSync();
	
	wSumFirstMoment = laneIdx < numWaves ? g_firstMoment[laneIdx].x : 0.0f;
	wSumFirstMoment = WaveActiveSum(wSumFirstMoment);

	wSumSecondMoment = laneIdx < numWaves ? g_secondMoment[laneIdx].x : 0.0f;
	wSumSecondMoment = WaveActiveSum(wSumSecondMoment);
	
	const float2 wSumMeanStd = MeanStdFromMoments(wSumFirstMoment, wSumSecondMoment, N);
	
	// and across the 2x4 region
	const uint4 mask = WaveMatch(quadrant);
	const uint q = WaveMultiPrefixSum(liesOnPlane, mask) + liesOnPlane;
	const uint finalLaneIdx = 19 + quadrant * 4;
	liesOnPlane = IsLaneActive(finalLaneIdx, waveActiveLanes) ? WaveReadLaneAt(q, finalLaneIdx) == 8 : liesOnPlane;
	
	//const float numToleratedWSumStds = mad(smoothstep(0, 0.6, roughness), 6.0f, 2.25f);
	//const float numToleratedWSumStds = mad(smoothstep(0, 0.6, max(roughness, 0.15)), 5.5f, 2.5f);
	const float numToleratedWSumStds = mad(smoothstep(0, 0.6, max(roughness, 0.15)), 6.0f, 3.0f);
	const bool wSumOutsideTolerance = r.w_sum > mad(numToleratedWSumStds, wSumMeanStd.y, wSumMeanStd.x);
	
	if (wSumOutsideTolerance && liesOnPlane)
		r = DIReservoir::Init();
	
	return liesOnPlane;
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

// helps with high-frequency roughness textures
// Ref: D. Zhdan, "Fast Denoising with Self-Stabilizing Recurrent Blurs," GDC, 2020.
float4 RoughnessHeuristic(float currRoughness, float4 sampleRoughness)
{
	float4 n = currRoughness * currRoughness * 0.99f + 0.05f;
	float4 w = abs(currRoughness - sampleRoughness) / n;

	return saturate(1.0f - w);
}

float RayDirHeuristic(float3 currWi, float3 neighborWi, float roughness)
{
	float weight = saturate(dot(currWi, neighborWi));
	// more stringent for smoother surfaces
	float exp = mad(smoothstep(0, 1, 1 - roughness), MAX_RAY_DIR_HEURISTIC_EXP, 1.0f);
	return pow(weight, exp);	
}

float GetTemporalM(float roughness, float metalness)
{
	bool isMetallic = metalness > MIN_METALNESS_METAL;
	float M_metal = smoothstep(0, 0.35f, roughness * roughness);
	float M_dielectric = smoothstep(0, 0.8f, roughness);
	float M = isMetallic ? M_metal : M_dielectric;
	M *= g_local.M_max;
	M = roughness < 0.1 ? 2 : M;
	
	return M;
}

void TemporalResample(uint2 DTid, float3 posW, float3 normal, float linearDepth, float3 baseColor, float metalness, float roughness, 
	float localCurvature, bool tracedThisFrame, inout DIReservoir r, inout BRDF::SurfaceInteraction surface, inout RNG rng)
{
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float baseColorLum = Math::Color::LuminanceFromLinearRGB(baseColor);
	surface.InitComplete(r.wi, baseColor, metalness, normal);

	// reverse reproject current pixel
	float3 posPlanet = posW;
	posPlanet.y += g_frame.PlanetRadius;
	const float rayT = Volumetric::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, posPlanet, r.wi);
	
	const float2 prevUV_virtual = SkyDI_Util::VirtualMotionReproject(posW, roughness, surface, rayT, localCurvature, linearDepth,
		g_frame.TanHalfFOV, g_frame.PrevViewProj);
	
	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
	const float2 motionVec = g_motionVector[DTid.xy];
	const float2 prevUV_surface = (DTid + 0.5) / renderDim - motionVec;
	
	float2 prevUV = roughness > MIN_ROUGHNESS_SURFACE_MOTION ? prevUV_surface : prevUV_virtual;
		
	// TODO corner case, not sure why, but following is needed
	if (g_local.CheckerboardTracing && roughness < MIN_ROUGHNESS_SURFACE_MOTION && baseColorLum <= MAX_LUM_VNDF)
		prevUV = tracedThisFrame ? FLT_MAX.xx : prevUV;
	
	if (any(prevUV > 1.0f.xx) || any(prevUV) < 0)
		return;

	// combine 2x2 neighborhood reservoirs around reprojected pixel
	const float2 f = prevUV * renderDim;
	const float2 topLeft = floor(f - 0.5f);
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
			g_frame.PrevViewInv,
			g_frame.PrevProjectionJitter);
	}
	
	weights *= PlaneHeuristic(prevPos, normal, posW, linearDepth);

	// roughness weight
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float4 prevRoughness = g_metalnessRoughness.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
	weights *= RoughnessHeuristic(roughness, prevRoughness);
	
	// ray dir weight
	float3 prevWi[4];
	
	[unroll]
	for (int i = 0; i < 4; i++)
	{
		if (weights[i] <= 1e-3)
			continue;
		
		const uint2 prevPixel = uint2(topLeft) + offsets[i];
		prevWi[i] = SkyDI_Util::PartialReadReservoir_ReuseWi(prevPixel, g_local.PrevTemporalReservoir_A_DescHeapIdx);
		weights[i] *= dot(abs(r.wi), 1) == 0 || !tracedThisFrame ? 1.0f : RayDirHeuristic(r.wi, prevWi[i], roughness);
	}
	
	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);
	
	weights *= bilinearWeights;
	weights *= weights > 1e-3;
	
	const float weightSum = dot(1, weights);
	if (weightSum < 1e-4)
		return;
	
	weights /= weightSum;
	const float maxM = GetTemporalM(roughness, metalness);
	
	[unroll]
	for (int k = 0; k < 4; k++)
	{
		if (weights[k] == 0.0f)
			continue;

		const uint2 prevPixel = uint2(topLeft) + offsets[k];	
		DIReservoir prev = SkyDI_Util::PartialReadReservoir_ReuseRest(prevPixel, g_local.PrevTemporalReservoir_A_DescHeapIdx, prevWi[k]);
		
		// recompute BRDF at current pixel given temporal reservoir's sample
		surface.InitComplete(prev.wi, baseColor, metalness, normal);
		const float3 brdfCostheta = BRDF::ComputeSurfaceBRDF(surface);
		const float target = Math::Color::LuminanceFromLinearRGB(prev.Li * brdfCostheta);
				
		// potenital bias as unbiased reuse requires visibility to be checked for all 
		// temporal reservoirs
		
		r.Combine(prev, maxM, weights[k], target, rng);
	}
	
	r.ComputeW(MAX_W_SUM);
		
#if !INCLUDE_VISIBILITY_IN_TARGET
	if (!EvaluateVisibility(posW, r.wi, normal, linearDepth))
		r.W = 0;
#endif	
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[WaveSize(WAVE_SIZE)]
[numthreads(SKY_DI_TEMPORAL_GROUP_DIM_X, SKY_DI_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, uint16_t2(SKY_DI_TEMPORAL_GROUP_DIM_X, SKY_DI_TEMPORAL_GROUP_DIM_Y),
		g_local.DispatchDimX, SKY_DI_TEMPORAL_TILE_WIDTH, SKY_DI_TEMPORAL_LOG2_TILE_WIDTH, g_local.NumGroupsInTile);
#else
	const uint2 swizzledDTid = DTid.xy;
#endif

#if CHECKERBOARD_SORT
	g_dtid[Gidx] = uint16_t2(swizzledDTid);
	int numThreadDiv2 = (SKY_DI_TEMPORAL_GROUP_DIM_X * SKY_DI_TEMPORAL_GROUP_DIM_Y) >> 1;
	bool inFirstHalf = Gidx < numThreadDiv2;
	
	GroupMemoryBarrierWithGroupSync();
	
	swizzledDTid = inFirstHalf ? g_dtid[Gidx << 1] : g_dtid[((Gidx - numThreadDiv2) << 1) + 1];
#endif

	if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
		return;

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[swizzledDTid];
	
	if(depth == 0)
		return;
		
	// reconstruct position from depth buffer
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 posW = Math::Transform::WorldPosFromScreenSpace(swizzledDTid,
		renderDim,
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv,
		g_frame.CurrProjectionJitter);
	
	// shading normal
	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_normal[swizzledDTid]);
		
	// roughness and metallic mask
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float2 mr = g_metalnessRoughness[swizzledDTid];

	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float3 baseColor = g_baseColor[swizzledDTid].rgb;

	const float3 wo = normalize(g_frame.CameraPos - posW);
	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::InitPartial(normal, mr.y, wo);

	// generate candidates
	RNG rng = RNG::Init(swizzledDTid, g_frame.FrameNum, renderDim);
	const float baseColorLum = Math::Color::LuminanceFromLinearRGB(baseColor);
		
	// resampling
	bool skipResampling = mr.y < g_local.MinRoughnessResample && (mr.x > MIN_METALNESS_METAL || baseColorLum < MAX_LUM_VNDF);

	bool traceThisFrame = g_local.CheckerboardTracing ?
		((swizzledDTid.x + swizzledDTid.y) & 0x1) == (g_frame.FrameNum & 0x1) :
		true;

	// always trace if surface is mirror like
	traceThisFrame = (traceThisFrame || mr.y < g_local.MinRoughnessResample);
	
	DIReservoir r = DIReservoir::Init();

	if(traceThisFrame)
	{
		if (mr.x >= MIN_METALNESS_METAL || baseColorLum <= MAX_LUM_VNDF)
			r = GenerateCandidatesVNDF(swizzledDTid, posW, normal, linearDepth, baseColor, mr.x, mr.y, surface, rng);
		else
			r = GenerateCandidatesMIS(swizzledDTid, posW, normal, linearDepth, baseColor, mr.x, mr.y, surface, rng);
	}
	
	if (g_local.DoTemporalResampling && !skipResampling)
	{
		GBUFFER_CURVATURE g_curvature = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::CURVATURE];
		float localCurvature = g_curvature[swizzledDTid];
		float adjustedLocalCurvature = (localCurvature > 1e-3) * pow((1 + linearDepth) / linearDepth, 4);
		// having a nonzero curvature for flat surfaces helps with reducing temporal artifacts
		adjustedLocalCurvature = max(0.02, adjustedLocalCurvature);
		
		if (g_local.PrefilterReservoirs)
			PrefilterOutliers(Gidx, posW, mr.y, linearDepth, r);
	
		TemporalResample(swizzledDTid, posW, normal, linearDepth, baseColor, mr.x, mr.y, adjustedLocalCurvature, traceThisFrame,
			r, surface, rng);
	}

	SkyDI_Util::WriteReservoir(swizzledDTid, r, g_local.CurrTemporalReservoir_A_DescHeapIdx,
			g_local.CurrTemporalReservoir_B_DescHeapIdx);
}