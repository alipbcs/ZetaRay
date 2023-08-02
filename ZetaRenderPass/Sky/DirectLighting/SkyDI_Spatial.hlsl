#include "SkyDI_Reservoir.hlsli"
#include "../../Common/Math.hlsli"
#include "../../Common/GBuffers.hlsli"
#include "../../Common/FrameConstants.h"
#include "../../Common/RT.hlsli"
#include "../../Common/Common.hlsli"

#define THREAD_GROUP_SWIZZLING 1
#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.01f
#define M_max 10
#define UNBIASED_RESUE 0
#define MAX_RAY_DIR_HEURISTIC_EXP 2.0f
#define MAX_NUM_SPATIAL_SAMPLES_METAL 5
#define MAX_NUM_SPATIAL_SAMPLES_DIELECTRIC 8
#define MAX_W_SUM 2.0f

static const float2 k_halton[8] =
{
	float2(0.0, -0.33333333333333337),
	float2(-0.5, 0.33333333333333326),
	float2(0.5, -0.7777777777777778),
	float2(-0.75, -0.11111111111111116),
	float2(0.25, 0.5555555555555554),
	float2(-0.25, -0.5555555555555556),
	float2(0.75, 0.11111111111111116),
	float2(-0.875, 0.7777777777777777)
};

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_SkyDI_Spatial> g_local : register(b1);
RaytracingAccelerationStructure g_sceneBVH : register(t0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

bool EvaluateVisibility(float3 pos, float3 wi, float3 normal, float linearDepth)
{
	if (wi.y < 0)
		return false;
	
	float tMin;
	const float3 adjustedOrigin = RT::OffsetRay(pos, normal, linearDepth, tMin);

	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
		RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
		RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
		RAY_FLAG_FORCE_OPAQUE> rayQuery;

	RayDesc ray;
	ray.Origin = adjustedOrigin;
	ray.TMin = tMin;
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

float PlaneHeuristic(float sampleDepth, float3 samplePos, float3 currNormal, float3 currPos, 
	float linearDepth, float scale)
{
	float planeDist = dot(currNormal, samplePos - currPos);
	float weight = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA * linearDepth * scale;
	
	return weight;
}

float RayDirHeuristic(float3 currWi, float3 neighborWi, float roughness)
{
	float weight = saturate(dot(currWi, neighborWi));
	// more stringent for smoother surfaces
	float exp = 1.0f + smoothstep(0, 1, 1 - roughness) * MAX_RAY_DIR_HEURISTIC_EXP;
	return pow(weight, exp);
}

// helps with high-frequency roughness textures
// Ref: D. Zhdan, "Fast Denoising with Self-Stabilizing Recurrent Blurs," GDC, 2020.
float RoughnessHeuristic(float currRoughness, float sampleRoughness)
{
	float n = currRoughness * currRoughness * 0.99f + 0.05f;
	float w = abs(currRoughness - sampleRoughness) / n;

	return saturate(1.0f - w);
}

float3 RecalculateBRDFWithWi(float3 wi, float3 F0, float3 diffuseReflectance, float3 normal, inout BRDF::SurfaceInteraction surface)
{
	if (surface.ndotwo <= 0.0f)
		return 0.0.xxx;
	
	float3 wh = normalize(wi + surface.wo);
	surface.ndotwi = saturate(dot(normal, wi));
	
	if (surface.ndotwi <= 0.0f)
		return 0.0.xxx;
	
	surface.ndotwh = saturate(dot(normal, wh));
	surface.whdotwo = saturate(dot(wh, surface.wo)); // == whdotwi
	surface.F = BRDF::FresnelSchlick(F0, surface.whdotwo);

	float3 specularBrdf = BRDF::SpecularBRDFGGXSmith(surface);
	float3 diffuseBrdf = (1.0f.xxx - surface.F) * diffuseReflectance * surface.ndotwi;

	return diffuseBrdf + specularBrdf;
}

void SpatialResample(uint2 DTid, float3 posW, float3 normal, float linearDepth, BRDF::SurfaceInteraction surface,
	float3 baseColor, bool metallic, float roughness, inout SkyDIReservoir r, inout RNG rng)
{
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];

	const float3 diffuseReflectance = baseColor * (1.0f - metallic) * ONE_OVER_PI;
	const float3 F0 = lerp(0.04f.xxx, baseColor, metallic);

	const float mScale = smoothstep(1, 20, r.M);
	const float biasToleranceScale = max(1 - mScale, 0.2f);

	// avoid tracing a shadow ray if current reservoir sample doesn't change
	const float3 preResampleWi = r.wi;
	const int2 renderDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);
	
	// rotate the sample sequence per pixel
	const float u0 = rng.Uniform();
	const float theta = u0 * TWO_PI;
	const float sinTheta = sin(theta);
	const float cosTheta = cos(theta);		
	
#if UNBIASED_RESUE
	float Z = r.M;
#endif
	
	bool reservoirSampleChanged = false;
	const bool disoccluded = r.M <= 2;
	
	// take fewer samples for metallic surfaces, proportional to roughness
	int numSpatialSamplesMetal = round(smoothstep(0, 0.15, roughness * roughness) * MAX_NUM_SPATIAL_SAMPLES_METAL);
	numSpatialSamplesMetal = max(numSpatialSamplesMetal, 1);
	int numSpatialSamplesDielectrinc = round(smoothstep(0, 0.2, roughness * roughness) * MAX_NUM_SPATIAL_SAMPLES_DIELECTRIC);
	numSpatialSamplesDielectrinc = max(numSpatialSamplesDielectrinc, 2);
	int numSpatialSamples = metallic ? numSpatialSamplesMetal : numSpatialSamplesDielectrinc;
	
	const float radius = 8 + smoothstep(0, 0.5, roughness) * 12;
	
	for (int i = 0; i < numSpatialSamples; i++)
	{
		float2 sampleUV = k_halton[i];
		float2 rotated;
		rotated.x = dot(sampleUV, float2(cosTheta, -sinTheta));
		rotated.y = dot(sampleUV, float2(sinTheta, cosTheta));
		rotated *= radius;
			
		const int2 samplePosSS = round(float2(DTid) + rotated);		
		if (samplePosSS.x == DTid.x && samplePosSS.y == DTid.y)
			continue;

		if (Math::IsWithinBounds(samplePosSS, renderDim))
		{
			const float neighborDepth = g_currDepth[samplePosSS];
			if(neighborDepth == 0.0)
				continue;

			const float sampleLinearDepth = Math::Transform::LinearDepthFromNDC(neighborDepth, g_frame.CameraNear);
			float3 samplePosW = Math::Transform::WorldPosFromScreenSpace(samplePosSS, 
				renderDim,
				sampleLinearDepth,
				g_frame.TanHalfFOV,
				g_frame.AspectRatio,
				g_frame.CurrViewInv,
				g_frame.CurrProjectionJitter);
			const float w_z = PlaneHeuristic(sampleLinearDepth, samplePosW, normal, posW, linearDepth, biasToleranceScale);

			const float sampleRoughness = g_metallicRoughness[samplePosSS].y;
			const float w_r = RoughnessHeuristic(roughness, sampleRoughness);
			
			SkyDIReservoir neighbor = SkyDI_Util::PartialReadReservoir_Shading(samplePosSS,
				g_local.InputReservoir_A_DescHeapIdx);

	#if 0
			float w_d = dot(abs(r.wi), 1) == 0 ? 1.0f : RayDirHeuristic(r.wi, neighbor.wi, roughness);
			w_d *= dot(neighbor.wi, normal) > 0;
	#else
			float w_d = dot(neighbor.wi, normal) > 0;
	#endif
			
			float weight = saturate(w_z * w_d * w_r);
			if (weight <= 1e-3)
				continue;
					
			const float3 brdfCostheta = RecalculateBRDFWithWi(neighbor.wi, F0, diffuseReflectance, normal, surface);
			const float target = Math::Color::LuminanceFromLinearRGB(neighbor.Li * brdfCostheta);

			reservoirSampleChanged = reservoirSampleChanged || r.Combine(neighbor, M_max, weight, target, rng);
			
#if UNBIASED_RESUE
			if (EvaluateVisibility(posW, neighbor.wi, normal, linearDepth))
				Z += neighbor.M;
#endif
		}
	}	
	
#if UNBIASED_RESUE	
	r.M = Z;
#endif

	if(reservoirSampleChanged)
	{
		float maxWSum = roughness < 0.35 ? MAX_W_SUM : FLT_MAX;
		r.ComputeW(maxWSum);

		if (abs(dot(1, abs(r.wi - preResampleWi))) > 1e-4)
		{
			if (!EvaluateVisibility(posW, r.wi, normal, linearDepth))
				r.W = 0;
		}
	}	
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(SKY_DI_SPATIAL_GROUP_DIM_X, SKY_DI_SPATIAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, uint16_t2(SKY_DI_SPATIAL_GROUP_DIM_X, SKY_DI_SPATIAL_GROUP_DIM_Y),
		g_local.DispatchDimX, SKY_DI_SPATIAL_TILE_WIDTH, SKY_DI_SPATIAL_LOG2_TILE_WIDTH, g_local.NumGroupsInTile);
#else
	const uint2 swizzledDTid = DTid.xy;
#endif
	
	if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
		return;
	
	// reconstruct position from depth buffer
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[swizzledDTid];

	// skip sky pixels
	if (depth == 0.0)
		return;
		
	// roughness and metallic mask
	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	const float2 mr = g_metallicRoughness[swizzledDTid];

	bool isMetallic;
	bool hasBaseColorTexture;
	bool isEmissive;
	GBuffer::DecodeMetallic(mr.x, isMetallic, hasBaseColorTexture, isEmissive);
	
	if (isEmissive)
		return;
	
	SkyDIReservoir r = SkyDI_Util::ReadReservoir(swizzledDTid,
			g_local.InputReservoir_A_DescHeapIdx,
			g_local.InputReservoir_B_DescHeapIdx);

	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float3 baseColor = g_baseColor[swizzledDTid].rgb;
	const float baseColorLum = Math::Color::LuminanceFromLinearRGB(baseColor);

	bool skip = mr.y < g_local.MinRoughnessResample && (isMetallic || baseColorLum < MAX_LUM_VNDF);
	
	if (!g_local.DoSpatialResampling || skip)
	{
		SkyDI_Util::PartialWriteReservoir(swizzledDTid, r, g_local.OutputReservoir_A_DescHeapIdx);
		return;
	}

	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 posW = Math::Transform::WorldPosFromScreenSpace(swizzledDTid,
		renderDim,
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv,
		g_frame.CurrProjectionJitter);
	
	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_normal[swizzledDTid]);
		
	const float3 wo = normalize(g_frame.CameraPos - posW);
	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::Init(normal, wo, isMetallic, mr.y, baseColor);
		
	RNG rng = RNG::Init(swizzledDTid, g_frame.FrameNum);
	SpatialResample(swizzledDTid, posW, normal, linearDepth, surface, baseColor, isMetallic, mr.y, r, rng);
	
	SkyDI_Util::PartialWriteReservoir(swizzledDTid, r, g_local.OutputReservoir_A_DescHeapIdx);
}
