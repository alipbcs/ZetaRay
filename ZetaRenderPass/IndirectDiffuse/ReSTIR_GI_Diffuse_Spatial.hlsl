#include "Reservoir_Diffuse.hlsli"
#include "../Common/Math.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../../ZetaCore/Core/Material.h"

#define THREAD_GROUP_SWIZZLING 1

// following two have a noticeable impact on both quality and performance -- lower
// values can lead to low-frequency noise but is more cache friendly, whereas
// higher values lead to cache thrashing but get rid of the noise
#define SAMPLE_RADIUS_1ST 28
#define SAMPLE_RADIUS_2ND 16

#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.015f

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
RaytracingAccelerationStructure g_sceneBVH : register(t0);
StructuredBuffer<Material> g_materials : register(t1);
StructuredBuffer<uint> g_owenScrambledSobolSeq : register(t3);
StructuredBuffer<uint> g_scramblingTile : register(t4);
StructuredBuffer<uint> g_rankingTile : register(t5);
StructuredBuffer<uint> g_frameMeshData : register(t6);

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
	const uint16_t numFullTiles = g_local.DispatchDimX / RGI_DIFF_SPATIAL_TILE_WIDTH; // floor(DispatchDimX / TileWidth
	const uint16_t numGroupsInFullTiles = numFullTiles * g_local.NumGroupsInTile;

	uint16_t2 groupIDinTile;
	if (groupIDFlattened >= numGroupsInFullTiles)
	{
		const uint16_t lastTileDimX = g_local.DispatchDimX - RGI_DIFF_SPATIAL_TILE_WIDTH * numFullTiles; // DispatchDimX & NumGroupsInTile
		groupIDinTile = uint16_t2(groupIDinTileFlattened % lastTileDimX, groupIDinTileFlattened / lastTileDimX);
	}
	else
		groupIDinTile = uint16_t2(groupIDinTileFlattened & (RGI_DIFF_SPATIAL_TILE_WIDTH - 1), groupIDinTileFlattened >> RGI_DIFF_SPATIAL_LOG2_TILE_WIDTH);

	const uint16_t swizzledGidFlattened = groupIDinTile.y * g_local.DispatchDimX + tileID * RGI_DIFF_SPATIAL_TILE_WIDTH + groupIDinTile.x;
	const uint16_t2 swizzledGid = uint16_t2(swizzledGidFlattened % g_local.DispatchDimX, swizzledGidFlattened / g_local.DispatchDimX);
	const uint16_t2 swizzledDTid = swizzledGid * GroupDim + (uint16_t2) GTid.xy;
#else
	const uint16_t2 swizzledDTid = (uint16_t2)DTid.xy;
#endif

	return swizzledDTid;
}

float GeometryWeight(float sampleDepth, float3 samplePos, float3 currNormal, float3 currPos, float linearDepth, float scale)
{
	float planeDist = dot(currNormal, samplePos - currPos);
	
	// lower the tolerance as more samples are accumulated
	//float tolerance = g_local.MaxPlaneDist * scale;
	//float weight = saturate(tolerance - abs(planeDist) / max(tolerance, 1e-4f));
	float weight = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA * linearDepth;
	
	return weight;
}

float NormalWeight(float3 input, float3 sample, float scale)
{
	float cosTheta = dot(input, sample);
	float angle = Math::ArcCos(cosTheta);
	// tolerance angle becomes narrower as more samples are accumulated
	float tolerance = 0.08726646 + 0.27925268 * scale; // == [5.0, 16.0] degrees 
	float weight = pow(saturate((tolerance - angle) / tolerance), g_local.NormalExp);
	
	return weight;
}

void DoSpatialResampling(uint16_t2 DTid, float3 posW, float3 normal, float linearDepth, 
	inout DiffuseReservoir r, inout RNG rng)
{
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];

	// as M goes up, radius becomes smaller and vice versa
	const float mScale = smoothstep(1, MAX_TEMPORAL_M, r.M);
	const float searchRadius = g_local.IsFirstPass ? SAMPLE_RADIUS_1ST : SAMPLE_RADIUS_2ND;

	//float biasToleranceScale = 1.0 - mScale * 0.5;
	float biasToleranceScale = max(1.0 - mScale * mScale, 0.5);
	biasToleranceScale = g_local.IsFirstPass ? biasToleranceScale : biasToleranceScale * 2.0f;
	
	const float u0 = rng.RandUniform();
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
		float2 sampleLocalXZ = k_halton[baseOffset + i];
		float2 rotatedXZ;
		rotatedXZ.x = dot(sampleLocalXZ, float2(cosTheta, -sinTheta));
		rotatedXZ.y = dot(sampleLocalXZ, float2(sinTheta, cosTheta));
		float2 relativeSamplePos = rotatedXZ * searchRadius;
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
			const float w_z = GeometryWeight(sampleDepth, samplePosW, normal, posW, linearDepth, biasToleranceScale);
					
			const float3 sampleNormal = Math::Encoding::DecodeUnitNormal(g_currNormal[samplePosSS]);
			const float w_n = NormalWeight(normal, sampleNormal, biasToleranceScale);

			const float weight = w_z * w_n;
			if (weight < 1e-3)
				continue;
					
			DiffuseReservoir neighborReservoir = RGI_Diff_Util::ReadInputReservoir(samplePosSS, g_local.InputReservoir_A_DescHeapIdx,
				g_local.InputReservoir_B_DescHeapIdx, g_local.InputReservoir_C_DescHeapIdx);

			float jacobianDet = 1.0f;
			const float3 x1_q = samplePosW;
			const float3 x2_q = neighborReservoir.SamplePos;
			const float3 secondToFirst_r = x1_r - x2_q;
			const float3 wi = normalize(-secondToFirst_r);

			if (g_local.PdfCorrection)
				jacobianDet = RGI_Diff_Util::JacobianDeterminant(x1_q, x2_q, wi, secondToFirst_r, neighborReservoir);
	
			float mm = g_local.IsFirstPass ? MAX_SPATIAL_M : MAX_SPATIAL_M * 8;
			r.Combine(neighborReservoir, wi, normal, mm, weight, jacobianDet, rng);
		}
	}	
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(RGI_DIFF_SPATIAL_GROUP_DIM_X, RGI_DIFF_SPATIAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint16_t2 swizzledDTid = SwizzleThreadGroup(DTid, Gid, GTid);
	
	if (!Math::IsWithinBoundsExc(swizzledDTid, uint16_t2(g_frame.RenderWidth, g_frame.RenderHeight)))
		return;
	
	// reconstruct position from depth buffer
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[swizzledDTid];

	// skip sky pixels
	if (depth == 0.0)
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
//	{
		RNG rng = RNG::Init(swizzledDTid, g_frame.FrameNum, renderDim);
		DoSpatialResampling(swizzledDTid, posW, normal, linearDepth, r, rng);
//	}

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