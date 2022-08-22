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
StructuredBuffer<Reservoir> g_histReservoir : register(t2, space0);
StructuredBuffer<AliasTableEntry> g_envMapAliasTable : register(t3, space0);
StructuredBuffer<float2> g_halton : register(t4, space0);
RWStructuredBuffer<Reservoir> g_currReservoir : register(u0, space0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

bool EvaluateVisibility(in float3 posW, in float3 wi, in float3 geometricNormal, inout Reservoir r)
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
	{
		r.W = 0.0f;		// invalidate reservoir
		return false;
	}
		
	return true;
}

// Resample set of samples drawn from the distribution of env-map-patches using the unshadowed path 
// contribution as the target function (i.e. L_e * brdf * cos(theta)).
Reservoir SampleLeWithRIS(in Texture2D<half3> envMap, in float3 posW, inout uint rngState, 
	inout SurfaceInteraction surface, out float3 candidateWi)
{
	Reservoir r = ResetReservoir();
	
	for (int i = 0; i < g_local.NumRISCandidates; i++)
	{	
		// sample env-map patches according to power (i.e. Lumen)
		float patchPdf;
		uint j = SampleEnvMapAliasTable(g_envMapAliasTable, RandUniform2D(rngState), g_local.NumPatches, patchPdf);
		EnvMapPatch patch = g_envMapPatches[j];
		
		// probability of sampling the given patch * uniform sampling in the patch surface area
		float sourcePdf = patchPdf * g_local.OndDivPatchArea;
		float phi = lerp(patch.Phi1, patch.Phi1 + g_local.dPhi, RandUniform(rngState));
		float cosTheta = patch.CosTheta1 - RandUniform(rngState) * (patch.CosTheta1 - patch.CosTheta2);
		float theta = ArcCos(cosTheta);		
		// here, theta is in [0, PI / 2], multiply by 1 / PI to get value in [0, 1] 
		float2 uv = float2(phi * ONE_DIV_TWO_PI, theta * ONE_DIV_PI);
		
		float4 targetFunction = ComputeTargetFunction(envMap, uv, cosTheta, phi, posW, g_frame.WorldRadius, surface);
		float weight = targetFunction.x / sourcePdf;
		r.WeightSum += weight;
		
		// WRS
		float u = RandUniform(rngState);
		bool updateReservoir = u < (weight / r.WeightSum);
		
		if(updateReservoir)
		{
			r.TargetFunction = targetFunction.x;
			r.EnvMapUV = uv;
			candidateWi = targetFunction.yzw;
		}
	}
	
	r.M = g_local.NumRISCandidates;
	r.W = r.WeightSum / (r.M * r.TargetFunction);
	
	return r;
}

// Original RIS assumes candidates are all drawn from the same probability
// distribution (PDF). Since rendered scenes have some degree of spatio-temporal
// locality, we can increase the effective number of canidates for RIS further by 
// utilizing this locality. Essentially, this means we can combine the reservoir for 
// pixel q with its (spatio-temporal) neighbors q_1, q_2, ..., q_k.
//
// Paper shows that combining reservoirs remains an unbiased estimate of 1 / P(y) when every  
// P_q(y) > 0 for spatio-temporal neighbor distribution q. This condition is violated 
// when surfaces have different orientations visibilties (y is occluded from the neighbor).
//
// To debias, mentioned condition needs to be checked for every candidate sample. This could 
// be expensive due to need to trace a ray for every candidate. As a cheaper alternative, heuristics
// similar to ones typically used in denoiser can be used to guess whether P_q(y) > 0.
//
// Finally, drawing samples from different distributions could lead to high variance as 
// MC estimator f / p could fluctuate when p is poor match for f. Multiple-Importance
// Sampling is a popular choice that deals with this issue by applying a weight,
// which mitigates variance spikes when at least one of the PDFs is a good match
// for integrand f.
float4 ComputeBiasHeuristicWeights(in float2 prevPosTS, in float3 currNormal, in float currLinearZ,
	in float2 dxdy, in uint2 inputDim, in float3 prevWo, out SurfaceInteraction prevSurface, out float3 prevPosW)
{
	// w-----------z
	// |-----------|
	// |--prev-----|
	// |-----------|
	// x-----------y
	float2 f = prevPosTS * inputDim;
	int2 topLeft = floor(f - 0.5f);
	float2 offset = f - 0.5f - topLeft;
	
	// gather is faster than taking four samples individually
	GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const uint4 prevNormalsEncoded = g_prevNormal.GatherRed(g_samPointClamp, prevPosTS).wzxy;
	float3 prevNormals[4];

	[unroll]
	for (int i = 0; i < 4; i++)
		prevNormals[i] = DecodeUnitNormalFromUint(prevNormalsEncoded[i]);

	// prev Depth
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float4 prevDepths = g_prevDepth.GatherRed(g_samPointClamp, prevPosTS).wzxy;
	prevDepths = ComputeLinearDepth(prevDepths, g_frame.CameraNear, g_frame.CameraFar);
		
	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
										   offset.x * (1.0f - offset.y),
										   (1.0f - offset.x) * offset.y,
										   offset.x * offset.y);
	
	float4 weights = NormalBiasHeuristic(prevNormals, currNormal, g_local.NormalAngleThreshold) *
					 DepthBiasHeuristic(prevDepths, currLinearZ, dxdy, g_local.DepthToleranceScale) *
					 bilinearWeights;
	
	// weight must be zero for out-of-bound samples
	const float4 isInBounds = float4(IsInRange(topLeft, inputDim),
									 IsInRange(topLeft + uint2(1, 0), inputDim),
									 IsInRange(topLeft + uint2(0, 1), inputDim),
									 IsInRange(topLeft + uint2(1, 1), inputDim));
	
	weights *= isInBounds;
	
	float3 prevShadingNormal = prevNormals[0] * weights.x + 
		prevNormals[1] * weights.y +
		prevNormals[2] * weights.z + 
		prevNormals[3] * weights.w;
	
	// regular bilinear-sampling for Base-Color and MetalnessRoughness
	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::BASE_COLOR];
	float3 prevBaseColor = g_baseColor.SampleLevel(g_samLinearClamp, prevPosTS, 0.0f).rgb;

	GBUFFER_METALLIC_ROUGHNESS g_prevMetallicRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	const half2 prevMetalnessRoughness = g_prevMetallicRoughness.SampleLevel(g_samLinearClamp, prevPosTS, 0.0f);

	// use Shading Normal in place of Geometric Normal to avoid an extra load. Here Geometric Normal is
	// not needed
	prevSurface = ComputePartialSurfaceInteraction(prevShadingNormal, 
		prevMetalnessRoughness.y, prevMetalnessRoughness.x, 
		prevWo, prevBaseColor);
	
	// reconstruct previous world position
	float prevLinearDepth = dot(prevDepths, bilinearWeights);
	prevPosW = WorldPosFromTexturePos(prevPosTS, prevLinearDepth, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv);
	
	return weights;
}

// Notes:
// 1. Assumes at least one temporal neighbor is compatible
// 2. sigma (biasWeights) == 1
Reservoir SampleHistory(in Texture2D<half3> envMap, in uint2 DTid, in uint2 textureDim, in float2 prevPosTS,
	in float4 biasWeights, in float3 posW, inout SurfaceInteraction surface, inout uint rngState)
{
	Reservoir histReservoirs[4];
	
	//	w-----------z
	//	|-----------|
	//	|--prev-----|
	//	|-----------|
	//	x-----------y
	const float2 prevScreenPos = prevPosTS * textureDim;
	const int2 topLeft = floor(prevScreenPos - 0.5f);
	const uint flattenedTopLeftTexPos = topLeft.y * textureDim.x + topLeft.x;

	// w -> (0, 0)
	uint buffIndices[4];
	buffIndices[0] = flattenedTopLeftTexPos;
	buffIndices[1] = DTid.x < textureDim.x - 1 ? flattenedTopLeftTexPos + 1 : flattenedTopLeftTexPos;				// z -> (0, 1)
	buffIndices[2] = (DTid.y < textureDim.y - 1) ? flattenedTopLeftTexPos + textureDim.x : flattenedTopLeftTexPos;  // x -> (1, 0)
	buffIndices[3] = (DTid.x < textureDim.x - 1) && (DTid.y < textureDim.y - 1) ?									// y -> (1, 1)
					  flattenedTopLeftTexPos + textureDim.x + 1 :
					  flattenedTopLeftTexPos;
	
	[unroll]
	for (uint i = 0; i < 4; i++)
	{
		if (biasWeights[i] > g_local.TemporalSampleBiasWeightThreshold)
			histReservoirs[i] = g_histReservoir[buffIndices[i]];
		else
			histReservoirs[i] = ResetReservoir();
	}
	
	Reservoir temporalReservoir = ResetReservoir();
	float m;
	
	// "emulate" bilinear-interpolation of reservoirs by combining four nearest temporal reservoirs
	// using the current pixel as target
	[unroll]
	for (int n = 0; n < 4; n++)
	{
		// skip this neighbor
		if (biasWeights[n] < g_local.TemporalSampleBiasWeightThreshold)
			continue;

		// target function
		const float2 uv = histReservoirs[n].EnvMapUV;
		const float2 phiTheta = uv * float2(TWO_PI, PI);
		const float4 targetFunction = ComputeTargetFunction(envMap, uv, cos(phiTheta.y), phiTheta.x, posW, g_frame.WorldRadius, surface);
		
		// rcp(histReservoirs[n].W) == normalized_target(r.y)
		// * M to account for number of samples that contributed
		const float risWeight = targetFunction.x * histReservoirs[n].W * histReservoirs[n].M;
		temporalReservoir.WeightSum += risWeight;
		
		// WRS (incompatible neighbors can't be picked due to "biasWeights" check)
		float u = RandUniform(rngState);
		bool updateReservoir = u < (risWeight / temporalReservoir.WeightSum);
		temporalReservoir.M += histReservoirs[n].M;

		if (updateReservoir)
		{
			temporalReservoir.TargetFunction = targetFunction.x;
			temporalReservoir.EnvMapUV = uv;
			temporalReservoir.WasTemporalReservoirVisible = histReservoirs[n].WasTemporalReservoirVisible;
			m = biasWeights[n];
		}
	}
	
	temporalReservoir.M = min(temporalReservoir.M, g_local.NumRISCandidates * g_local.MaxMScale);
	temporalReservoir.W = m * temporalReservoir.WeightSum / temporalReservoir.TargetFunction;

	return temporalReservoir;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

//[WaveSize(8)]
[numthreads(ReSTIR_THREAD_GROUP_SIZE_X, ReSTIR_THREAD_GROUP_SIZE_Y, ReSTIR_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	float4 baseColor = g_baseColor[DTid.xy];

	// not on the surface of the scene
	const float EPS = 0.0001f;
	if (baseColor.w < MIN_ALPHA_CUTOFF + EPS)
		return;

	// seed the RNG
	const uint2 textureDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	uint rngState = InitRNG(DTid.xy, g_frame.FrameNum, textureDim);
		
	// reconstruct position from depth buffer
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float linearDepth = g_depth[DTid.xy];
	linearDepth = ComputeLinearDepth(linearDepth, g_frame.CameraNear, g_frame.CameraFar);

	const float2 posTS = (DTid.xy + 0.5f) / textureDim;
	const float3 posW = WorldPosFromTexturePos(posTS, linearDepth, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv);
	
	// fill in the surface data
	const float3 wo = normalize(g_frame.CameraPos - posW);

	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const half2 packedNormals = g_normal[DTid.xy];
	const float3 shadingNormal = DecodeUnitNormalFromHalf2(packedNormals.xy);
	//const float3 geometricNormal = DecodeUnitNormalFromHalf2(packedNormals.zw);

	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	const half2 metalnessRoughness = g_metallicRoughness[DTid.xy];

	SurfaceInteraction surface = ComputePartialSurfaceInteraction(shadingNormal, 
		metalnessRoughness.y,
		metalnessRoughness.x,
		wo, 
		baseColor.xyz);
	
	Texture2D<half3> g_envMap = ResourceDescriptorHeap[g_frame.EnvMapDescHeapOffset];
	
	// use RIS with unshadowd path contribution as the target function to pick a candidate sample
	float3 candidateWi;
	Reservoir r = SampleLeWithRIS(g_envMap, posW, rngState, surface, candidateWi);
	
	// invalidate reservoir if it's occluded
	//EvaluateVisibility(posW, candidateWi, geometricNormal, r);
	
	// backprojection (in texture space)
	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
	const half2 motionVec = g_motionVector[DTid.xy];
	
	const float2 prevPosTS = posTS - motionVec;
	const uint currPixelIdx1D = DTid.y * textureDim.x + DTid.x;
		
	// history pixel went outside the screen
	if (any(abs(prevPosTS) - prevPosTS))
	{
		g_currReservoir[currPixelIdx1D] = r;
		return;
	}
	
	Texture2D<float2> g_linearDepthGrad = ResourceDescriptorHeap[g_local.LinearDepthGradDescHeapIdx];
	const float2 dxdy = g_linearDepthGrad[DTid.xy];

	// Heuristics to reject/accept spatiotemporal neighbors
	SurfaceInteraction prevSurface;
	float3 prevPosW;
	float4 biasWeights = ComputeBiasHeuristicWeights(prevPosTS, shadingNormal, linearDepth, dxdy, textureDim, wo, prevSurface, prevPosW);
	const float oneNorm = dot(1.0f, biasWeights);
	
	// target distribution for the temporal neighbors was "too" different, reuse would probably 
	// lead to increased bias. Spatial pass tries to compensate by taking more samples for such cases
	if (oneNorm < 4 * g_local.TemporalSampleBiasWeightThreshold)
	{
		g_currReservoir[currPixelIdx1D] = r;
		return;
	}
	
	// normalize
	biasWeights /= oneNorm;
	Reservoir temporalReservoir = SampleHistory(g_envMap, DTid.xy, textureDim, prevPosTS, biasWeights, posW, surface, rngState);
	
	// temporal reuse
	const float origCurrReservoirTarget = r.TargetFunction;
	const float origCurrReservoirW = r.W;
	const float2 origCurrReservoirUV = r.EnvMapUV;
	float M = 0.0f;
	float m_c = 1.0f;
	DoPairwiseMIS(g_envMap, posW, prevPosW, g_frame.WorldRadius, temporalReservoir, r, prevSurface, surface, m_c, M, rngState);
	EndPairwiseMIS(M, origCurrReservoirUV, origCurrReservoirTarget, origCurrReservoirW, 1, r, m_c, rngState);
	
	r.DidTemporalReuse = true;
	
	g_currReservoir[currPixelIdx1D] = r;
}