#include "Reservoir_Diffuse.hlsli"
#include "../Common/Math.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/BRDF.hlsli"
#include "../Common/FrameConstants.h"
#include "../../ZetaCore/Core/Material.h"
#include "../Common/Common.hlsli"

#define THREAD_GROUP_SWIZZLING 1
#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.01

static const float2 k_halton[16] =
{
	float2(0.0, -0.33333333333333337),
	float2(-0.5, 0.33333333333333326),
	float2(0.5, -0.7777777777777778),
	float2(-0.75, -0.11111111111111116),
	float2(0.25, 0.5555555555555554),
	float2(-0.25, -0.5555555555555556),
	float2(0.75, 0.11111111111111116),
	float2(-0.875, 0.7777777777777777),
	float2(0.125, -0.9259259259259259),
	float2(-0.375, -0.2592592592592593),
	float2(0.625, 0.40740740740740744),
	float2(-0.625, -0.7037037037037037),
	float2(0.375, -0.03703703703703709),
	float2(-0.125, 0.6296296296296293),
	float2(0.875, -0.4814814814814815),
	float2(-0.9375, 0.18518518518518512)
};

static const uint16_t2 GroupDim = uint16_t2(RGI_DIFF_SPATIAL_GROUP_DIM_X, RGI_DIFF_SPATIAL_GROUP_DIM_Y);

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_RGI_Diff_Spatial> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

float GeometricHeuristic(float sampleDepth, float3 samplePos, float3 currNormal, 
	float3 currPos, float linearDepth, float scale)
{
	float planeDist = dot(currNormal, samplePos - currPos);	
	// lower the tolerance as more samples are accumulated
	float weight = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA * linearDepth * scale;
	
	return weight;
}

float NormalHeuristic(float3 normal, float3 neighborNormal)
{
	// normals within ~20 degrees 
	float weight = abs(dot(normal, neighborNormal)) >= 0.93f;
	
	return weight;
}

float RoughnessHeuristic(float currRoughness, float sampleRoughness)
{
	float n = currRoughness * currRoughness * 0.99f + 0.01f;
	float w = abs(currRoughness - sampleRoughness) / n;

	return saturate(1.0f - w);
}

void DoSpatialResampling(uint2 DTid, float3 posW, float3 normal, float linearDepth, float roughness,
	inout DiffuseReservoir r, inout RNG rng)
{
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];

	// as M goes up, radius becomes smaller and vice versa
	const float mScale = smoothstep(1, MAX_TEMPORAL_M, r.M);
	const float biasToleranceScale = max(1 - mScale, 0.2f);
	//const float searchRadius = g_local.IsFirstPass ? g_local.Radius1st : g_local.Radius2nd;
	//const float searchRadius = g_local.IsFirstPass ? 42 : 15;
	const float maxRadius = g_local.IsFirstPass ? 36 : 4;
	const float searchRadius = 5 + smoothstep(0, 1, max(r.M - 3, 0) / MAX_TEMPORAL_M) * maxRadius;
	
	const float u0 = rng.Uniform();
	const float theta = u0 * TWO_PI;
	const float sinTheta = sin(theta);
	const float cosTheta = cos(theta);
	// number of samples impacts both quality and performance
	const int numIterations = g_local.IsFirstPass ? 8 : 6;	
	const float3 x1_r = posW;	// q -> reused path, r -> current pixel's path

	const int2 renderDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);
	const int baseOffset = g_local.IsFirstPass ? 0 : 8;
	
	for (int i = 0; i < numIterations; i++)
	{
		// rotate sample sequence
		float2 sampleLocalXZ = k_halton[baseOffset + i] * searchRadius;
		float2 rotatedXZ;
		rotatedXZ.x = dot(sampleLocalXZ, float2(cosTheta, -sinTheta));
		rotatedXZ.y = dot(sampleLocalXZ, float2(sinTheta, cosTheta));
		float2 relativeSamplePos = rotatedXZ;
		const float2 relSamplePosAbs = abs(relativeSamplePos);
			
		// make sure sampled pixel isn't pixel itself
		if (relSamplePosAbs.x <= 0.5f && relSamplePosAbs.y <= 0.5f)
		{
			relativeSamplePos = relSamplePosAbs.x > relSamplePosAbs.y ?
				float2(sign(relativeSamplePos.x) * (0.5f + 1e-5f), relativeSamplePos.y) :
				float2(relativeSamplePos.x, sign(relativeSamplePos.y) * (0.5f + 1e-5f));
		}

		const int2 samplePosSS = round(float2(DTid) + relativeSamplePos);
	
		if (Math::IsWithinBoundsExc(samplePosSS, renderDim))
		{
			const float sampleDepth = Math::Transform::LinearDepthFromNDC(g_currDepth[samplePosSS], g_frame.CameraNear);
			float3 samplePosW = Math::Transform::WorldPosFromScreenSpace(samplePosSS, renderDim,
				sampleDepth,
				g_frame.TanHalfFOV,
				g_frame.AspectRatio,
				g_frame.CurrViewInv);
			const float w_z = GeometricHeuristic(sampleDepth, samplePosW, normal, posW, linearDepth, 
				biasToleranceScale);
					
			// for some reason normals-based heuristic adds a lot of bias
#if 0
			const float3 sampleNormal = Math::Encoding::DecodeUnitNormal(g_currNormal[samplePosSS]);
			const float w_n = NormalHeuristic(normal, sampleNormal);
#else
			const float w_n = 1.0;
#endif
			
			float sampleRoughness = g_metalnessRoughness[samplePosSS].y;
			const float w_r = RoughnessHeuristic(roughness, sampleRoughness);

			float sampleMetalness = g_metalnessRoughness[samplePosSS].x;
			const float w_m = sampleMetalness < MIN_METALNESS_METAL;
			
			//const float weight = w_z * w_n * w_r * w_m;
			const float weight = w_z * w_r * w_m;

			if (weight < 1e-3)
				continue;

			DiffuseReservoir neighborReservoir = RGI_Diff_Util::ReadInputReservoir(samplePosSS, 
				g_local.InputReservoir_A_DescHeapIdx,
				g_local.InputReservoir_B_DescHeapIdx, 
				g_local.InputReservoir_C_DescHeapIdx);

			float jacobianDet = 1.0f;
			const float3 x1_q = samplePosW;
			const float3 x2_q = neighborReservoir.SamplePos;
			const float3 secondToFirst_r = x1_r - x2_q;
			const float3 wi = normalize(-secondToFirst_r);

			if (g_local.PdfCorrection)
				jacobianDet = RGI_Diff_Util::JacobianDeterminant(x1_q, x2_q, wi, secondToFirst_r, neighborReservoir);
	
			r.Combine(neighborReservoir, wi, normal, MAX_SPATIAL_M, weight, jacobianDet, rng);
		}
	}	
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(RGI_DIFF_SPATIAL_GROUP_DIM_X, RGI_DIFF_SPATIAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, GroupDim, g_local.DispatchDimX,
		RGI_DIFF_SPATIAL_TILE_WIDTH, RGI_DIFF_SPATIAL_LOG2_TILE_WIDTH, g_local.NumGroupsInTile);
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

	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	float2 mr = g_metalnessRoughness[swizzledDTid];

	if (mr.x >= MIN_METALNESS_METAL)
		return;

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
	
	DiffuseReservoir r = RGI_Diff_Util::ReadInputReservoir(swizzledDTid, g_local.InputReservoir_A_DescHeapIdx,
			g_local.InputReservoir_B_DescHeapIdx, g_local.InputReservoir_C_DescHeapIdx);
			
//	if (g_local.IsFirstPass || r.M < 2)
	if (g_local.DoSpatialResampling)
	{
		RNG rng = RNG::Init(swizzledDTid, g_frame.FrameNum, renderDim);
		DoSpatialResampling(swizzledDTid, posW, normal, linearDepth, mr.y, r, rng);
	}

	if (g_local.IsFirstPass)
	{
		RGI_Diff_Util::WriteOutputReservoir(swizzledDTid, r, g_local.OutputReservoir_A_DescHeapIdx, g_local.OutputReservoir_B_DescHeapIdx,
			g_local.OutputReservoir_C_DescHeapIdx);
	}
	else
	{
		RGI_Diff_Util::PartialWriteOutputReservoir(swizzledDTid, r, g_local.OutputReservoir_A_DescHeapIdx, g_local.OutputReservoir_B_DescHeapIdx);
	}
}