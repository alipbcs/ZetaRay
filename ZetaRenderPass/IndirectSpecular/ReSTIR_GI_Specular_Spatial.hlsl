#include "Reservoir_Specular.hlsli"
#include "../Common/Math.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../../ZetaCore/Core/Material.h"

#define THREAD_GROUP_SWIZZLING 1
#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.02f
#define M_max 15

static const uint16_t2 GroupDim = uint16_t2(RGI_SPEC_SPATIAL_GROUP_DIM_X, RGI_SPEC_SPATIAL_GROUP_DIM_Y);

static const float2 k_samples[16] =
{
	float2(0.06045313262973884, -0.7786273259781794),
	float2(-0.9605067340338866, 0.2039486024469801),
	float2(0.8185416830901944, 0.97774287535859),
	float2(-0.19387811764034812, -0.002862899439824318),
	float2(0.7488839419532154, -0.48644963211102565),
	float2(-0.27472948763673866, 0.526023240065749),
	float2(0.49792329222488196, 0.25407237778400127),
	float2(-0.5009319838559589, -0.6868809847447852),
	float2(0.2593420651361287, -0.2662317698503073),
	float2(-0.701277304675877, 0.8023228089140253),
	float2(0.8821750776990636, 0.4975203997307518),
	float2(-0.09431946889027976, -0.5033405923292801),
	float2(0.5034130028604717, -0.8882860904687584),
	float2(-0.48614958581707035, 0.11835310036871238),
	float2(0.21945876304870415, 0.6905468385084759),
	float2(-0.8077813185588439, -0.2442943317892171)
};

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_RGI_Spec_Spatial> g_local : register(b1);
RaytracingAccelerationStructure g_sceneBVH : register(t0);

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
	const uint16_t numFullTiles = g_local.DispatchDimX / RGI_SPEC_SPATIAL_TILE_WIDTH; // floor(DispatchDimX / TileWidth
	const uint16_t numGroupsInFullTiles = numFullTiles * g_local.NumGroupsInTile;

	uint16_t2 groupIDinTile;
	if (groupIDFlattened >= numGroupsInFullTiles)
	{
		// DispatchDimX & NumGroupsInTile
		const uint16_t lastTileDimX = g_local.DispatchDimX - RGI_SPEC_SPATIAL_TILE_WIDTH * numFullTiles;
		groupIDinTile = uint16_t2(groupIDinTileFlattened % lastTileDimX, groupIDinTileFlattened / lastTileDimX);
	}
	else
	{
		groupIDinTile = uint16_t2(
			groupIDinTileFlattened & (RGI_SPEC_SPATIAL_TILE_WIDTH - 1),
			groupIDinTileFlattened >> RGI_SPEC_SPATIAL_LOG2_TILE_WIDTH);
	}

	const uint16_t swizzledGidFlattened = groupIDinTile.y * g_local.DispatchDimX + tileID * RGI_SPEC_SPATIAL_TILE_WIDTH + groupIDinTile.x;
	const uint16_t2 swizzledGid = uint16_t2(swizzledGidFlattened % g_local.DispatchDimX, swizzledGidFlattened / g_local.DispatchDimX);
	const uint16_t2 swizzledDTid = swizzledGid * GroupDim + (uint16_t2) GTid.xy;
#else
	const uint16_t2 swizzledDTid = (uint16_t2)DTid.xy;
#endif
	
	return swizzledDTid;
}

float PlaneHeuristic(float sampleDepth, float3 samplePos, float3 currNormal, float3 currPos, float linearDepth)
{
	float planeDist = dot(currNormal, samplePos - currPos);
	float weight = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA * linearDepth;
	
	return weight;
}

float NormalHeuristic(float3 input, float3 sample, float alpha)
{
	float cosTheta = dot(input, sample);
	float angle = Math::ArcCos(cosTheta);
	
	// tolerance angle becomes narrower based on specular lobe half angle
	// Ref: D. Zhdan, "Fast Denoising with Self-Stabilizing Recurrent Blurs," GDC, 2020.
	float scale = alpha / (1.0 + alpha);
	float tolerance = 0.08726646 + 0.27925268 * scale; // == [5.0, 16.0] degrees 
	//float weight = pow(saturate((tolerance - angle) / tolerance), g_local.NormalExp);
	float weight = saturate((tolerance - angle) / tolerance);
	weight *= weight;
	
	return weight;
}

// helps with high-frequency roughness textures
// Ref: D. Zhdan, "Fast Denoising with Self-Stabilizing Recurrent Blurs," GDC, 2020.
float RoughnessWeight(float currRoughness, float sampleRoughness)
{
	float n = currRoughness * currRoughness * 0.99f + 5e-3f;
	float w = abs(currRoughness - sampleRoughness) / n;
	w  = saturate(1.0f - w);
	w *= sampleRoughness <= g_local.RoughnessCutoff;
	
	return w;
}

float RayTHeuristic(float currRayT, float sampleRayT)
{
	float relativeDiff = saturate(abs(currRayT - sampleRayT) / max(g_local.HitDistSigmaScale * currRayT, 1e-4));
	float w = 1.0 - relativeDiff;
	
	return w;
}

void SpatialResample(uint2 DTid, float3 posW, float3 normal, float linearDepth, BRDF::SurfaceInteraction surface,
	float3 baseColor, float roughness, float isMetallic, inout SpecularReservoir r, inout RNG rng)
{
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];

	const int2 renderDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float rayT = length(r.SamplePos - posW);
	
	// smaller hit distance results in sharper reflections
	float d = length(posW);
	float f = saturate(rayT / max(rayT + d, 1e-6f));
	float searchRadius = lerp(roughness, 1.0, f) * g_local.Radius;
	searchRadius = max(1, searchRadius);
	
	// lower number of iterations when radius is smaller
	const int numIterations = round(smoothstep(0, 1, f) * g_local.NumIterations);
	
	// rotate the sample sequence per pixel
	const float u0 = rng.RandUniform();
	const float theta = u0 * TWO_PI;
	const float sinTheta = sin(theta);
	const float cosTheta = cos(theta);		
	
	for (int i = 0; i < numIterations; i++)
	{
		float2 sampleUV = k_samples[i] * searchRadius;
		float2 rotated;
		rotated.x = dot(sampleUV, float2(cosTheta, -sinTheta));
		rotated.y = dot(sampleUV, float2(sinTheta, cosTheta));
			
#if 1
		const int2 samplePosSS = round(float2(DTid) + rotated);
		
		if (samplePosSS.x == DTid.x && samplePosSS.y == DTid.y)
			continue;
#else
		// shift if sample pixel is pixel itself
		const float2 rotatedAbs = abs(rotated);
		if (rotatedAbs.x <= 0.5f && rotatedAbs.y <= 0.5f)
		{
			rotated = rotatedAbs.x > rotatedAbs.y ?
				float2(sign(rotated.x) * (0.5f + 1e-5f), rotated.y) :
				float2(rotated.x, sign(rotated.y) * (0.5f + 1e-5f));
		}
		const int2 samplePosSS = round(float2(DTid) + rotated);
#endif

		if (Math::IsWithinBoundsExc(samplePosSS, renderDim))
		{
			const float sampleDepth = Math::Transform::LinearDepthFromNDC(g_currDepth[samplePosSS], g_frame.CameraNear);
			float3 samplePosW = Math::Transform::WorldPosFromScreenSpace(samplePosSS, 
				renderDim,
				sampleDepth,
				g_frame.TanHalfFOV,
				g_frame.AspectRatio,
				g_frame.CurrViewInv);
			const float w_z = PlaneHeuristic(sampleDepth, samplePosW, normal, posW, linearDepth);
			
			const float3 sampleNormal = Math::Encoding::DecodeUnitNormal(g_currNormal[samplePosSS]);
			const float w_n = NormalHeuristic(normal, sampleNormal, surface.alpha);
			
			const float sampleRoughnes = g_metalnessRoughness[samplePosSS].y;
			const float w_r = RoughnessWeight(roughness, sampleRoughnes);
								
			SpecularReservoir neighbor = RGI_Spec_Util::PartialReadReservoir_Reuse(samplePosSS,
				g_local.InputReservoir_A_DescHeapIdx,
				g_local.InputReservoir_B_DescHeapIdx,
				g_local.InputReservoir_C_DescHeapIdx,
				g_local.InputReservoir_D_DescHeapIdx);

			const float sampleRayT = length(neighbor.SamplePos - posW);
			const float w_t = RayTHeuristic(rayT, sampleRayT);

			float weight = w_z * w_n * w_r * w_t;
			if (weight <= 1e-3)
				continue;
			
			const float3 secondToFirst_r = posW - neighbor.SamplePos;
			const float3 wi = normalize(-secondToFirst_r);
			float3 brdfCostheta_r = RGI_Spec_Util::RecalculateSpecularBRDF(wi, baseColor, isMetallic, surface);
			float jacobianDet = 1.0f;

			if (g_local.PdfCorrection)
				jacobianDet = RGI_Spec_Util::JacobianDeterminant(posW, neighbor.SamplePos, wi, secondToFirst_r, neighbor);

			r.Combine(neighbor, M_max, weight, jacobianDet, brdfCostheta_r, rng);
		}
	}	
}

void WriteReservoir(uint2 DTid, SpecularReservoir r)
{
	RGI_Spec_Util::PartialWriteReservoir_NoNormalW(DTid, r, g_local.OutputReservoir_A_DescHeapIdx,
			g_local.OutputReservoir_B_DescHeapIdx,
			g_local.OutputReservoir_D_DescHeapIdx);
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(RGI_SPEC_SPATIAL_GROUP_DIM_X, RGI_SPEC_SPATIAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint2 swizzledDTid = SwizzleThreadGroup(DTid, Gid, GTid);
	
	if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
		return;
	
	// reconstruct position from depth buffer
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[swizzledDTid];

	// skip sky pixels
	if (depth == 0.0)
		return;
		
	// roughness and metallic mask
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float2 mr = g_metalnessRoughness[swizzledDTid];

	// roughness cutoff
	if (mr.y > g_local.RoughnessCutoff)
		return;

	SpecularReservoir r = RGI_Spec_Util::PartialReadReservoir_NoW(swizzledDTid,
			g_local.InputReservoir_A_DescHeapIdx,
			g_local.InputReservoir_B_DescHeapIdx,
			g_local.InputReservoir_C_DescHeapIdx,
			g_local.InputReservoir_D_DescHeapIdx);

	if (!g_local.DoSpatialResampling || mr.y <= g_local.MinRoughnessResample)
	{
		WriteReservoir(swizzledDTid, r);
		return;
	}

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

	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float3 baseColor = g_baseColor[swizzledDTid].rgb;
		
	const float3 wo = normalize(g_frame.CameraPos - posW);
	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::InitPartial(normal, mr.y, wo);
	surface.diffuseReflectance = baseColor * (1.0f - mr.x);
		
	RNG rng = RNG::Init(swizzledDTid, g_frame.FrameNum, renderDim);
	SpatialResample(swizzledDTid, posW, normal, linearDepth, surface, baseColor, mr.y, mr.x, r, rng);
	
	WriteReservoir(swizzledDTid, r);		
}
