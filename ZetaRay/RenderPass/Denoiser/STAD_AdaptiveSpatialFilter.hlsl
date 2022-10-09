#include "STAD_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/Material.h"
#include "../Common/Sampling.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"

//#define DO_THREAD_GROUP_SWIZZLING

static const float k_gaussian[] =
{
	0.9300857212086524,
	0.898377426556872,
	0.9145349333803761,
	0.9374567429916906,
	0.84340178882948,
	0.9035463062375412,
	0.999228804920024,
	0.7360665037444651
};

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbSTADSpatialFilter> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);
StructuredBuffer<uint> g_owenScrambledSobolSeq : register(t0, space0);
StructuredBuffer<uint> g_scramblingTile : register(t1, space0);
StructuredBuffer<uint> g_rankingTile : register(t2, space0);

//--------------------------------------------------------------------------------------
// Edge-stopping functions and other helpers
//--------------------------------------------------------------------------------------

float GeometryWeight(float sampleDepth, float3 samplePos, float3 currNormal, float3 currPos, float scale)
{
	float planeDist = dot(currNormal, samplePos - currPos);
	
	// lower the tolerance as more samples are accumulated
	float tolerance = g_local.MaxPlaneDist * scale;
	float weight = saturate(tolerance - abs(planeDist) / max(tolerance, 1e-4f));
	
	return weight;
}

float NormalWeight(float3 input, float3 sample, float scale)
{
	float cosTheta = dot(input, sample);
	float angle = Common::ArcCos(cosTheta);
	// tolerance angle becomes narrower as more samples are accumulated
	float tolerance = 0.08726646 + 0.7853981f * scale; // == [5.0, 46.0] degrees 
	float weight = pow(saturate((tolerance - angle) / tolerance), g_local.NormalExp);
	
	return weight;
}

// normal is assumed to be normalized
float2 GetWorldPosUVFromSurfaceLocalCoord(in float3 pos, in float3 normal, in float2 xz, out float3 samplePosW)
{
	float3 posLocal = float3(xz.x, 0.0f, xz.y);
	
	// build rotation quaternion that maps y = (0, 1, 0) to surface normal
	float4 q = Common::QuaternionFromY(normal);
	samplePosW = pos + Common::RotateVector(posLocal, q);
	
	float4 posNDC = mul(float4(samplePosW, 1.0f), g_frame.CurrViewProj);
	posNDC.xy /= posNDC.w;
	posNDC.y = -posNDC.y;

	return posNDC.xy * 0.5f + 0.5f;
}

float3 Filter(int2 DTid, float3 centerColor, float3 normal, float linearDepth, float tspp)
{
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	Texture2D<float4> g_inTemporalCache = ResourceDescriptorHeap[g_local.TemporalCacheInDescHeapIdx];

	// 1 / 32 <= x <= 1
	const float oneSubAccSpeed = (g_local.MaxTspp - tspp + 1.0f) / (float) g_local.MaxTspp;
	
	// 1 / 32 <= x <= 1
	const float accSpeed = tspp / (float) g_local.MaxTspp;

	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 uv = (DTid + 0.5f) / renderDim;
	const float3 pos = Common::WorldPosFromUV(uv, linearDepth, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv, g_frame.CurrCameraJitter);

	// as tspp goes up, radius becomes smaller and vice versa
	//const float radiusScale = sqrt(oneSubAccSpeed);
	const float kernelRadius = g_local.FilterRadiusBase * g_local.FilterRadiusScale;
	
	const float sampleIdx = g_frame.FrameNum & 31;
	const float u0 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			DTid.x, DTid.y, sampleIdx, 2);

	const float theta = u0 * TWO_PI;
	const float sinTheta = sin(theta);
	const float cosTheta = cos(theta);

	float3 weightedColor = 0.0.xxx;
	float weightSum = 0.0f;
	
	[unroll]
	for (int i = 0; i < 8; i++)
	{
		// rotate
		float2 sampleLocalXZ = k_pmjbn[i];
		sampleLocalXZ *= kernelRadius;
		float2 rotatedXZ;
		rotatedXZ.x = dot(sampleLocalXZ, float2(cosTheta, -sinTheta));
		rotatedXZ.y = dot(sampleLocalXZ, float2(sinTheta, cosTheta));
		
		// local coordiante system XYZ where Y points in the direction of surface normal
		// map from that local space to world space, then project and compute the corresponding UV
		float3 samplePosW;
		float2 samplePosUV = GetWorldPosUVFromSurfaceLocalCoord(pos, normal, rotatedXZ, samplePosW);
		//int2 samplePosSS = (int2) round(samplePosUV * float2(g_frame.RenderWidth, g_frame.RenderHeight));
				
		if (Common::IsWithinBounds(samplePosUV, 1.0f.xx))
		{
			float sampleDepth = g_currDepth.SampleLevel(g_samPointClamp, samplePosUV, 0.0f);
			sampleDepth = Common::ComputeLinearDepthReverseZ(sampleDepth, g_frame.CameraNear);
			const float w_z = GeometryWeight(sampleDepth, samplePosW, normal, pos, oneSubAccSpeed);
					
			float2 encodedNormal = g_currNormal.SampleLevel(g_samPointClamp, samplePosUV, 0.0f);
			const float3 sampleNormal = Common::DecodeUnitNormalFromHalf2(encodedNormal);
			//const float normalToleranceScale = saturate(oneSubAccSpeed * 16.0f);
			const float normalToleranceScale = 1.0f;
			const float w_n = NormalWeight(normal, sampleNormal, normalToleranceScale);
					
			const float3 sampleColor = g_inTemporalCache.SampleLevel(g_samPointClamp, samplePosUV, 0.0f).rgb;
			//const float3 sampleColor = (samplePosSS.x == DTid.x && samplePosSS.y == DTid.y) ? 0.0.xxx : float3(0.1f, 0.0f, 0.0f);

			const float weight = w_z * w_n * k_gaussian[i];
			weightedColor += weight * sampleColor;
			weightSum += weight;
		}
	}
	
	float3 filtered = weightSum > 1e-3 ? weightedColor / weightSum : 0.0.xxx;
	float s = weightSum > 1e-3 ? accSpeed : 0.0f;
	
	s = min(s, 0.8f);
	filtered = lerp(filtered, centerColor, s);
	
	return filtered;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(STAD_SPATIAL_FILTER_THREAD_GROUP_SIZE_X, STAD_SPATIAL_FILTER_THREAD_GROUP_SIZE_Y, STAD_SPATIAL_FILTER_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if defined(DO_THREAD_GROUP_SWIZZLING)
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint16_t groupIDFlattened = (uint16_t) Gid.y * g_local.DispatchDimX.x + (uint16_t) Gid.x;
	const uint16_t tileID = groupIDFlattened / g_local.NumGroupsInTile;
	const uint16_t groupIDinTileFlattened = groupIDFlattened % g_local.NumGroupsInTile;

	// TileWidth is a power of 2 for all tiles except possibly the last one
	const uint16_t numFullTiles = (uint16_t) g_local.DispatchDimX.x / g_local.TileWidth; // floor(DispatchDimX / TileWidth
	const uint16_t numGroupsInFullTiles = numFullTiles * g_local.NumGroupsInTile;

	uint16_t2 groupIDinTile;
	if (groupIDFlattened >= numGroupsInFullTiles)
	{
		const uint16_t lastTileDimX = g_local.DispatchDimX.x - g_local.TileWidth * numFullTiles; // DispatchDimX & NumGroupsInTile
		groupIDinTile = uint16_t2(groupIDinTileFlattened % lastTileDimX, groupIDinTileFlattened / lastTileDimX);
	}
	else
	{
		groupIDinTile = uint16_t2(groupIDinTileFlattened & (g_local.TileWidth - 1), groupIDinTileFlattened >> g_local.Log2TileWidth);
	}

	const uint16_t swizzledGidFlattened = groupIDinTile.y * g_local.DispatchDimX + tileID * g_local.TileWidth + groupIDinTile.x;
	const uint16_t2 swizzledGid = uint16_t2(swizzledGidFlattened % g_local.DispatchDimX, swizzledGidFlattened / g_local.DispatchDimX);
	const uint16_t2 groupDim = uint16_t2(WAVELET_TRANSFORM_THREAD_GROUP_SIZE_X, WAVELET_TRANSFORM_THREAD_GROUP_SIZE_Y);
	const uint16_t2 swizzledDTid = swizzledGid * groupDim + (uint16_t2) GTid.xy;
#else
	const uint16_t2 swizzledDTid = (uint16_t2) DTid.xy;
#endif

	if (!Common::IsWithinBounds(swizzledDTid, uint16_t2(g_frame.RenderWidth, g_frame.RenderHeight)))
		return;
	
	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float isSurfaceMarker = g_baseColor[DTid.xy].w;

	// skip if this pixel belongs to sky
	if (isSurfaceMarker < MIN_ALPHA_CUTOFF)
		return;

	// integrated data
	Texture2D<float4> g_inTemporalCache = ResourceDescriptorHeap[g_local.TemporalCacheInDescHeapIdx];
	float4 integratedVals = g_inTemporalCache[swizzledDTid];
	
	// current frame's normals
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Common::DecodeUnitNormalFromHalf2(g_currNormal[swizzledDTid].xy);
		
	// current frame's depth
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float linearDepth = Common::ComputeLinearDepthReverseZ(g_currDepth[swizzledDTid], g_frame.CameraNear);

	float3 filtered = Filter(swizzledDTid, integratedVals.xyz, normal, linearDepth, integratedVals.w);
	RWTexture2D<float4> g_outTemporalCache = ResourceDescriptorHeap[g_local.TemporalCacheOutDescHeapIdx];
	g_outTemporalCache[swizzledDTid] = float4(filtered, integratedVals.w);
}