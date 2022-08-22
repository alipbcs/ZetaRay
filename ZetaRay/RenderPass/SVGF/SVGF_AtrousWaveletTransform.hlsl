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

// inputDepth: linear depth of input
// sampleDepth: linear depth of sample
// dxdy: partial derivate of linear depth at input
//
// For a filter with radius R, we compute the contribution of samples
// at offsets (i, j) where -R < i,j < R and take the weighted average.
//
// Linear z is not linear in screen space. Therefore, for given scree-space offset q
// relative to z0, find linear z corresponding to z0 + q.
//	
// Assuming a local linear model at view space position P0:
//		1 / P.Z = 1 / P0.Z + q(1 / P1.Z - 1 / P0.Z)
//
// which means, for view space point P, P lies at a line connecting P0 and P1 where projection
// of P on projection window (at z = d), has offset q from projection of P.
//		
// With Z1 = Z0 + dz and q = pixel offset, we can compute P.z.
float DepthWeightFunction(float inputZ, float sampleZ, float2 dz, uint2 screenSpaceOffset)
{
	float weight;
	
	float2 Z1 = inputZ + dz;
	float2 Z = (1.0f / (1.0f / inputZ) + screenSpaceOffset * ((1.0f / Z1) - (1.0f / inputZ)));
	float2 t = Z - inputZ;
	t *= sign(screenSpaceOffset);
	
	// Linear approximation:
	// f(x) = f(a) + f'(a)(x - a)
	// f(x) - f(a) = f'(a)(x - a)
	// if differene is within tolerance, accept it:
	//		0 < |sample - input| < tolerance
	// use f(x) - f(a) as tolerance
	float tolerance = g_local.DepthSigma * dot(1, abs(t));
	float depthDifference = abs(sampleZ - inputZ);
	
	// anything in 0 < x < minfloatprecision becomes 0. Reduces banding.
	depthDifference = max(0.0f, NextFloat32Down(depthDifference));
	weight = exp(-depthDifference / NextFloat32Up(tolerance));
	
	// zero contribution for anything below cutoff
	weight = weight >= g_local.DepthWeightCutoff;
	
	return weight;
}

float NormalWeightFunction(float3 input, float3 sample)
{
	return pow(max(0.0f, dot(input, sample)), g_local.NormalSigma);
}

float LuminanceWeightFunction(float lum, float sampleLum, float lumStd, int2 offset)
{
	float lumDiff = abs(lum - sampleLum);
	float std = g_local.LumSigma * lumStd + 0.005f;
	
	// increase the tolerance for pixels that are further apart
	//std *= 1.0f / length(offset);

	// bilateral range filtering
	// filter more if variance is high (likely there's more noise)	
	float weight = exp(-lumDiff / std);
	
	return weight;
}

void Filter(in int16_t2 DTid, in float3 integratedColor, in float integratedLum, in float lumStd, 
	in float3 normal, in float linearDepth, in float2 dzdx_dzdy, out float3 filtered, out float weightSum)
{
	const int16_t2 renderDim = int16_t2(g_frame.RenderWidth, g_frame.RenderHeight);
	
	// Eq (1) in paper
	// center pixel has weight 1
	float3 weightedColor = integratedColor * Kernel2D[Radius][Radius];
	weightSum = Kernel2D[Radius][Radius];
				
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

	[unroll]
	for (int16_t y = 0; y < KernelWidth; y++)
	{
		[unroll]
		for (int16_t x = 0; x < KernelWidth; x++)
		{
			if (x == Radius && y == Radius)
				continue;
				
			int16_t2 offset = (int16_t2(x, y) - Radius) * waveletScale;
			int16_t2 sampleAddr = DTid + offset;
				
			if (IsInRange(sampleAddr, renderDim))
			{
				const float sampleDepth = ComputeLinearDepthReverseZ(g_currDepth[sampleAddr], g_frame.CameraNear);
				const float w_z = DepthWeightFunction(linearDepth, sampleDepth, dzdx_dzdy, sampleAddr);
					
				const float3 sampleNormal = DecodeUnitNormalFromHalf2(g_currNormal[sampleAddr].xy);
				const float w_n = NormalWeightFunction(normal, sampleNormal);
					
				const uint2 integratedVals = g_integratedTemporalCache[sampleAddr].xy;
				const float3 sampleColor = float3(f16tof32(integratedVals.x >> 16), f16tof32(integratedVals.y), f16tof32(integratedVals.y >> 16));
				const float sampleLum = f16tof32(integratedVals.x);

				const float w_l = LuminanceWeightFunction(integratedLum, sampleLum, lumStd, offset);

				//const float weight = w_z * w_n * w_l * Kernel2D[y][x];
				const float weight = w_l * w_n * Kernel2D[y][x];
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
	
	if (lumVar < g_local.MinVarianceToFilter)
		return;
	
	// integrated data
	RWTexture2D<uint4> g_integratedTemporalCache = ResourceDescriptorHeap[g_local.IntegratedTemporalCacheDescHeapIdx];
	uint2 integratedVals = g_integratedTemporalCache[swizzledDTid].xy;

	const float3 color = float3(f16tof32(integratedVals.x >> 16), f16tof32(integratedVals.y), f16tof32(integratedVals.y >> 16));
	const float lum = f16tof32(integratedVals.x);
	const float lumStd = sqrt(lumVar);
	
	// current frame's normals
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = DecodeUnitNormalFromHalf2(g_currNormal[swizzledDTid].xy);
		
	// current frame's depth
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float linearDepth = ComputeLinearDepthReverseZ(g_currDepth[swizzledDTid], g_frame.CameraNear);

	// partial derivatives of linear depth
	Texture2D<float2> g_linearDepthGrad = ResourceDescriptorHeap[g_local.LinearDepthGradDescHeapIdx];
	const float2 dzdx_dzdy = g_linearDepthGrad[swizzledDTid];

	float3 filtered;
	float weightSum;
	Filter(swizzledDTid, color, lum, lumStd, normal, linearDepth, dzdx_dzdy, filtered, weightSum);
	
	if (weightSum > 1e-6)
	{
		//lum = LuminanceFromLinearRGB(filtered);
		uint2 ret;
		ret.x = (f32tof16(filtered.r) << 16) | f32tof16(lum);
		ret.y = (f32tof16(filtered.b) << 16) | f32tof16(filtered.g);
		g_integratedTemporalCache[swizzledDTid].xy = ret;
		
		// smooth out increasingly smaller variations. if an edge survives one iteration, it 
		// will survive all the subsequent iterations.
		g_lumVar[swizzledDTid] = half(lumVar * weightSum * weightSum);
	}
}