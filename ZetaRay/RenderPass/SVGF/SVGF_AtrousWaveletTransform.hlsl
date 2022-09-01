// Wavelet transform decomposes a function into a set of wavelets where each one has a correponding
// scale (a) and location. Scale (dilation) determines how wide or narrow the signal is. Therefore,
// higher a lets us capture lower frequency information and lower a lets us capture higher frequency 
// information.

// TODO currently denoised value is as the next frame's temporal cache. So essentially, we're temporally
// integrating denoised values rather than the raw raytraced values. SVGF paper used
// the output of first filter pass. This is a tradeoff between bias introduced from filtering and
// temporal lag.

#define FILTER_3x3
#include "SVGF_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/Material.h"

//#define DO_THREAD_GROUP_SWIZZLING

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbAtrousWaveletFilter> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame: register(b1);

//--------------------------------------------------------------------------------------
// Edge-stopping functions and other helpers
//--------------------------------------------------------------------------------------

float GeometryWeight(float sampleDepth, float2 sampleUV, float3 currNormal, float3 currPos)
{
	float3 samplePos = WorldPosFromTexturePos(sampleUV, sampleDepth, g_frame.TanHalfFOV,
		g_frame.AspectRatio, g_frame.CurrViewInv, g_frame.CurrCameraJitter);
	
	float planeDist = dot(currNormal, samplePos - currPos);
	float weight = saturate(1.0f - abs(planeDist) / g_local.MaxPlaneDist);
	
	return weight;
}

float NormalWeight(float3 input, float3 sample)
{
	return pow(max(0.0f, dot(input, sample)), g_local.NormalSigma);
}

float LuminanceWeight(float lum, float sampleLum, float lumStd, int2 offset)
{
	float lumDiff = abs(lum - sampleLum);
	float std = g_local.LumSigma * lumStd + 0.1f;
	
	// increase the tolerance for pixels that are further apart
	std *= 1.0f / length(offset);

	// bilateral range filtering
	// filter more if variance is high (likely there's more noise)	
	float weight = exp(-lumDiff / std);
	
	return weight;
}

void Filter(in int16_t2 DTid, in float3 integratedColor, in float integratedLum, in float lumStd, 
	in float3 normal, in float linearDepth, out float3 filtered, out float weightSum)
{
	const int16_t2 renderDim = int16_t2(g_frame.RenderWidth, g_frame.RenderHeight);
	
	// Eq (1) in paper
	// center pixel has weight 1
	float3 weightedColor = integratedColor * k_kernel2D[k_radius][k_radius];
	weightSum = k_kernel2D[k_radius][k_radius];
				
	// kernel widths 3x3 to 65x65
	// step 1 --> 3x3
	// step 2 --> 5x5
	// step 4 --> 9x9
	// step 8 --> 17x17
	// step 16 --> 33x33
	// step 32 --> 65x65
	const int16_t2 waveletScale = int16_t2(g_local.Step, g_local.Step);
						
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	RWTexture2D<uint4> g_integratedTemporalCache = ResourceDescriptorHeap[g_local.IntegratedTemporalCacheDescHeapIdx];

	const float2 uv = (DTid + 0.5f) / renderDim;
	const float3 pos = WorldPosFromTexturePos(uv, linearDepth, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv, g_frame.CurrCameraJitter);
	
	[unroll]
	for (int16_t y = 0; y < k_kernelWidth; y++)
	{
		[unroll]
		for (int16_t x = 0; x < k_kernelWidth; x++)
		{
			if (x == k_radius && y == k_radius)
				continue;
				
			int16_t2 offset = (int16_t2(x, y) - k_radius) * waveletScale;
			int16_t2 sampleAddr = DTid + offset;
				
			if (IsInRange(sampleAddr, renderDim))
			{
				const float sampleDepth = ComputeLinearDepthReverseZ(g_currDepth[sampleAddr], g_frame.CameraNear);
				const float2 sampleUV = (sampleAddr + 0.5f) / renderDim;
				const float w_z = GeometryWeight(sampleDepth, sampleUV, normal, pos);
					
				const float3 sampleNormal = DecodeUnitNormalFromHalf2(g_currNormal[sampleAddr].xy);
				const float w_n = NormalWeight(normal, sampleNormal);
					
				const uint2 integratedVals = g_integratedTemporalCache[sampleAddr].xy;
				const float3 sampleColor = float3(f16tof32(integratedVals.x >> 16), f16tof32(integratedVals.y), f16tof32(integratedVals.y >> 16));
				const float sampleLum = f16tof32(integratedVals.x);

				const float w_l = LuminanceWeight(integratedLum, sampleLum, lumStd, offset);

				const float weight = w_z * w_n * k_kernel2D[y][x];
				//const float weight = w_z * w_n * k_kernel2D[y][x];
				weightedColor += weight * sampleColor;
				weightSum += weight;
			}
		}
	}
	
	filtered = weightedColor / weightSum;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(WAVELET_TRANSFORM_THREAD_GROUP_SIZE_X, WAVELET_TRANSFORM_THREAD_GROUP_SIZE_Y, WAVELET_TRANSFORM_THREAD_GROUP_SIZE_Z)]
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

	if (!IsInRange(swizzledDTid, uint2(g_frame.RenderWidth, g_frame.RenderHeight)))
		return;
	
	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float isSurfaceMarker = g_baseColor[DTid.xy].w;

	// skip if this pixel belongs to sky
	if (isSurfaceMarker < MIN_ALPHA_CUTOFF)
		return;

	RWTexture2D<half> g_lumVar = ResourceDescriptorHeap[g_local.LumVarianceDescHeapIdx];
	const float lumVar = g_lumVar[swizzledDTid];
	const float lumStd = sqrt(lumVar);
	
//	if (lumVar < g_local.MinVarianceToFilter)
//		return;
	
	// integrated data
	RWTexture2D<uint4> g_temporalCache = ResourceDescriptorHeap[g_local.IntegratedTemporalCacheDescHeapIdx];
	uint2 integratedVals = g_temporalCache[swizzledDTid].xy;

	const float3 color = float3(f16tof32(integratedVals.x >> 16), f16tof32(integratedVals.y), f16tof32(integratedVals.y >> 16));
	const float lum = f16tof32(integratedVals.x);
	
	// current frame's normals
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = DecodeUnitNormalFromHalf2(g_currNormal[swizzledDTid].xy);
		
	// current frame's depth
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float linearDepth = ComputeLinearDepthReverseZ(g_currDepth[swizzledDTid], g_frame.CameraNear);

	float3 filtered;
	float weightSum;
	Filter(swizzledDTid, color, lum, lumStd, normal, linearDepth, filtered, weightSum);
	
	if (weightSum > 1e-6)
	{
		//lum = LuminanceFromLinearRGB(filtered);
		uint2 ret;
		ret.x = (f32tof16(filtered.r) << 16) | f32tof16(lum);
		ret.y = (f32tof16(filtered.b) << 16) | f32tof16(filtered.g);
		g_temporalCache[swizzledDTid].xy = ret;
		
		// smooth out increasingly smaller variations. if an edge survives one iteration, it 
		// will survive all the subsequent iterations.
		g_lumVar[swizzledDTid] = half(lumVar * weightSum * weightSum);
	}
}