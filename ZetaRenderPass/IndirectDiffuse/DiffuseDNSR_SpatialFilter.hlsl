#include "Reservoir_Diffuse.hlsli"
#include "../Common/Math.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/Sampling.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"

#define THREAD_GROUP_SWIZZLING 1
#define NUM_SAMPLES 10
#define PLANE_DIST_RELATIVE_DELTA 0.015f

static const float2 k_poissonDisk[NUM_SAMPLES] =
{
	float2(-0.2738789916038513, -0.1372080147266388),
	float2(0.12518197298049927, 0.056990981101989746),
	float2(0.12813198566436768, -0.3150410056114197),
	float2(-0.3912320137023926, 0.14719200134277344),
	float2(-0.13983601331710815, 0.25584501028060913),
	float2(-0.15115699172019958, -0.3827739953994751),
	float2(0.2855219841003418, 0.24613499641418457),
	float2(0.35111498832702637, 0.018658995628356934),
	float2(0.3842160105705261, -0.289002001285553),
	float2(0.14527297019958496, 0.425881028175354)
};

static const float k_gaussian[NUM_SAMPLES] =
{
	0.9399471833920812,
	0.9875914185128816,
	0.9264999409121231,
	0.8910805411044391,
	0.9454378629206168,
	0.8942405252636968,
	0.9104744409458791,
	0.9216444796377524,
	0.8585115766108588,
	0.8749084196560382
};

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbDiffuseDNSRSpatial> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Edge-stopping functions and other helpers
//--------------------------------------------------------------------------------------

float EdgeStoppingGeometry(float sampleDepth, float3 samplePos, float3 currNormal, float currLinearDepth,
	float3 currPos, float scale)
{
	float planeDist = dot(currNormal, samplePos - currPos);
	
	// lower the tolerance as more samples are accumulated
	float weight = abs(planeDist) <= PLANE_DIST_RELATIVE_DELTA * currLinearDepth * scale;

	return weight;
}

float EdgeStoppingNormal(float3 input, float3 sample, float scale)
{
	float cosTheta = dot(input, sample);
	
#if 0
	float angle = Math::ArcCos(abs(cosTheta));
	// tolerance angle becomes narrower as more samples are accumulated
	float tolerance = 0.08726646 + 0.7853981f * scale; // == [5.0, 46.0] degrees
	float weight = pow(saturate((tolerance - angle) / tolerance), g_local.NormalExp);
#else	
	float weight = pow(saturate(cosTheta), g_local.NormalExp);
#endif
	
	return weight;
}

float3 Filter(int2 DTid, float3 centerColor, float3 normal, float linearDepth, float tspp)
{
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	Texture2D<half4> g_inTemporalCache = ResourceDescriptorHeap[g_local.TemporalCacheInDescHeapIdx];

	// 1 / 32 <= x <= 1
	const float accSpeed = tspp / (float) g_local.MaxTspp;

	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 uv = (DTid + 0.5f) / renderDim;
	const float3 pos = Math::Transform::WorldPosFromUV(uv, linearDepth, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv);

	RNG rng = RNG::Init(DTid, g_frame.FrameNum, uint2(g_frame.RenderWidth, g_frame.RenderHeight));
	const float u0 = rng.RandUniform();

	const float theta = u0 * TWO_PI;
	const float sinTheta = sin(theta);
	const float cosTheta = cos(theta);

	float3 weightedColor = 0.0.xxx;
	float weightSum = 0.0f;
	
	[unroll]
	for (int i = 0; i < NUM_SAMPLES; i++)
	{
		// rotate
		float2 sampleLocalXZ = k_poissonDisk[i];
		float2 rotatedXZ;
		rotatedXZ.x = dot(sampleLocalXZ, float2(cosTheta, -sinTheta));
		rotatedXZ.y = dot(sampleLocalXZ, float2(sinTheta, cosTheta));

		const float filterRadiusScale = smoothstep(1.0 / 32.0, 1.0, accSpeed);
		const float filterRadius = lerp(g_local.MaxFilterRadius * g_local.FilterRadiusScale, g_local.MinFilterRadius, filterRadiusScale);
		float2 relativeSamplePos = rotatedXZ * filterRadius;
		const float2 relSamplePosAbs = abs(relativeSamplePos);
		
		// make sure sampled pixel isn't pixel itself
		if (relSamplePosAbs.x <= 0.5f && relSamplePosAbs.y <= 0.5f)
		{
			relativeSamplePos = relSamplePosAbs.x > relSamplePosAbs.y ?
				float2(sign(relativeSamplePos.x) * (0.5f + 1e-5f), relativeSamplePos.y) :
				float2(relativeSamplePos.x, sign(relativeSamplePos.y) * (0.5f + 1e-5f));
		}
	
		const int2 samplePosSS = round((float2) DTid + relativeSamplePos);

		if (Math::IsWithinBoundsExc(samplePosSS, (int2) renderDim))
		{
			const float sampleDepth = Math::Transform::LinearDepthFromNDC(g_currDepth[samplePosSS], g_frame.CameraNear);
			const float3 samplePosW = Math::Transform::WorldPosFromScreenSpace(samplePosSS, renderDim,
				sampleDepth,
				g_frame.TanHalfFOV,
				g_frame.AspectRatio,
				g_frame.CurrViewInv);
			const float w_z = EdgeStoppingGeometry(sampleDepth, samplePosW, normal, linearDepth, pos, 1);
					
			const float3 sampleNormal = Math::Encoding::DecodeUnitNormal(g_currNormal[samplePosSS]);
			const float normalToleranceScale = 1.0f;
			const float w_n = EdgeStoppingNormal(normal, sampleNormal, normalToleranceScale);
					
			const float3 sampleColor = g_inTemporalCache[samplePosSS].rgb;

			const float weight = w_z * w_n * k_gaussian[i];
			if (weight < 1e-4)
				continue;
			
			weightedColor += weight * sampleColor;
			weightSum += weight;
		}
	}
	
	float3 filtered = weightSum > 1e-3 ? weightedColor / weightSum : 0.0.xxx;
	float s = weightSum > 1e-3 ? accSpeed : 0.0f;	
	s = min(s, 0.3f);
	
	filtered = lerp(filtered, centerColor, s);
	
	return filtered;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

static const uint16_t2 GroupDim = uint16_t2(DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_X, DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_Y);

[numthreads(DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_X, DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint16_t groupIDFlattened = (uint16_t) Gid.y * g_local.DispatchDimX + (uint16_t) Gid.x;
	const uint16_t tileID = groupIDFlattened / g_local.NumGroupsInTile;
	const uint16_t groupIDinTileFlattened = groupIDFlattened % g_local.NumGroupsInTile;

	// TileWidth is a power of 2 for all tiles except possibly the last one
	const uint16_t numFullTiles = g_local.DispatchDimX / DiffuseDNSR_SPATIAL_TILE_WIDTH; // floor(DispatchDimX / TileWidth
	const uint16_t numGroupsInFullTiles = numFullTiles * g_local.NumGroupsInTile;

	uint16_t2 groupIDinTile;
	if (groupIDFlattened >= numGroupsInFullTiles)
	{
		const uint16_t lastTileDimX = g_local.DispatchDimX - DiffuseDNSR_SPATIAL_TILE_WIDTH * numFullTiles; // DispatchDimX & NumGroupsInTile
		groupIDinTile = uint16_t2(groupIDinTileFlattened % lastTileDimX, groupIDinTileFlattened / lastTileDimX);
	}
	else
		groupIDinTile = uint16_t2(groupIDinTileFlattened & (DiffuseDNSR_SPATIAL_TILE_WIDTH - 1), groupIDinTileFlattened >> DiffuseDNSR_SPATIAL_LOG2_TILE_WIDTH);

	const uint16_t swizzledGidFlattened = groupIDinTile.y * g_local.DispatchDimX + tileID * DiffuseDNSR_SPATIAL_TILE_WIDTH + groupIDinTile.x;
	const uint16_t2 swizzledGid = uint16_t2(swizzledGidFlattened % g_local.DispatchDimX, swizzledGidFlattened / g_local.DispatchDimX);
	const uint16_t2 swizzledDTid = swizzledGid * GroupDim + (uint16_t2) GTid.xy;
#else
	const uint16_t2 swizzledDTid = (uint16_t2) DTid.xy;
#endif

	if (!Math::IsWithinBoundsExc(swizzledDTid, uint16_t2(g_frame.RenderWidth, g_frame.RenderHeight)))
		return;
	
	// current frame's depth
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_currDepth[swizzledDTid];
	
	// skip sky pixels
	if (depth == 0.0)
		return;

	// skip metallic surfaces
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	float metalness = g_metalnessRoughness[DTid.xy].x;
	if (metalness > MAX_METALNESS)
		return;

	Texture2D<float4> g_inTemporalCache = ResourceDescriptorHeap[g_local.TemporalCacheInDescHeapIdx];
	float4 integratedVals = g_inTemporalCache[swizzledDTid];
	
	// current frame's normal
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_currNormal[swizzledDTid].xy);
		
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);

	float3 filtered = Filter(swizzledDTid, integratedVals.xyz, normal, linearDepth, integratedVals.w);
	RWTexture2D<float4> g_outTemporalCache = ResourceDescriptorHeap[g_local.TemporalCacheOutDescHeapIdx];
	g_outTemporalCache[swizzledDTid] = float4(filtered, integratedVals.w);
}