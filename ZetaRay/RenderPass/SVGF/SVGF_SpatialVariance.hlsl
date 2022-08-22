// Spatial estimate of color variance

#include "SVGF_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/Material.h"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbSpatialVar> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

groupshared float2 g_lumMoments[16][SPATIAL_VAR_THREAD_GROUP_SIZE_X];
groupshared half g_numValidVals[16][SPATIAL_VAR_THREAD_GROUP_SIZE_X];
groupshared half g_depthCache[16][SPATIAL_VAR_THREAD_GROUP_SIZE_X];
static const int2 GroupDim = int2(SPATIAL_VAR_THREAD_GROUP_SIZE_X, SPATIAL_VAR_THREAD_GROUP_SIZE_Y);

// NumToLoadPerRowOrColumn = 8 + 2 * R pixel values need to be loaded to filter
// each row or column where 10 <= NumToLoadPerRowOrColumn <= 16. Note that all
// 16 threads participate in the computation
static const uint NumToLoadPerRowOrColumn = SPATIAL_VAR_THREAD_GROUP_SIZE_Y + 2 * g_local.Radius;

float DepthTest(float centerDepth, float neighborDepth, int neighborDist)
{
	const float depthDiff = abs(centerDepth - neighborDepth);
	const float tol = 0.05f + 0.01f * abs(g_local.Radius - neighborDist);
	return (depthDiff <= tol) * (centerDepth < FLT_MAX);
}

void HorizontalPass(uint3 Gid, uint Gidx)
{
	const int2 screenDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);
	const int2 blockFirstPixel = Gid.xy * GroupDim - g_local.Radius;
	
	// reshape original 8x8 group into 4x16 so that each 16 threads work on one image row
	const uint2 Gidx4x16 = uint2(Gidx & (16 - 1), Gidx >> 4);
		
	Texture2D<half4> g_indirectLiRayT = ResourceDescriptorHeap[g_local.IndirectLiRayTDescHeapIdx];
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];

    // Horizontally filter 4 rows of 16 pixels. Next iterations filters
    // next 4 rows and so on. In total, this 8x8 block horizontally touches on
	// R(=#Rows) x 8 pixels where 10 <= R <= 16, corresponding to 3x3 to 9x9 kernels.
	// In this way, Radius more rows above and Radius more rows below the 
	// original 8x8 block are loaded where
	// Rows = 2 * Radius + 8		e.g. for 3x3 filter, we filter 10x8 pixels, where 1
	//								extra row below and one extra row above the 8x8 are fitered.
	[unroll]
	for (int step = 0; step < 4; step++)
	{
		const int2 offsetAddr = Gidx4x16 + int2(0, step * 4);
		const int2 pixelAddr = blockFirstPixel + offsetAddr;

		// these image rows are not needed for filtering
		if (NumToLoadPerRowOrColumn <= offsetAddr.y)
			break;
		
		float lum = 0.0f;
		float lumSq = 0.0f;
		float depth = FLT_MAX;
		const bool isInScreenBounds = IsInRange(pixelAddr, screenDim);
		
		// avoid out-of-bound texture reads
		if (Gidx4x16.x < NumToLoadPerRowOrColumn && isInScreenBounds)
		{
			depth = ComputeLinearDepthReverseZ(g_depth[pixelAddr], g_frame.CameraNear);
			
			float3 color = g_indirectLiRayT[pixelAddr].rgb;
			lum = LuminanceFromLinearRGB(color);
			lumSq = lum * lum;
		}
				
		// #image rows spanned by each wave = WaveSize / 16
		const uint rowToWaveAdjustment = (WaveGetLaneIndex() >> 4) * 16; // biggest multiple of 16 smaller than lane-index

		// depth corresponding to pixel from 8x8 block
		const float centerDepth = WaveReadLaneAt(depth, min(Gidx4x16.x + g_local.Radius, WaveGetLaneCount() - 1));
		
		// 16 threads in each row operate on image rows of 8 values. Filtering each pixel consists of summing 
		// over 2 * Radius + 1 values. This computation is split the sum between 1st and 2nd 8 threads in each 
		// row
		float lumSum = Gidx4x16.x < SPATIAL_VAR_THREAD_GROUP_SIZE_X ? lum : 0.0f;
		float lumSqSum = Gidx4x16.x < SPATIAL_VAR_THREAD_GROUP_SIZE_X ? lumSq : 0.0f;
		float numValues = ((Gidx4x16.x < SPATIAL_VAR_THREAD_GROUP_SIZE_X) && isInScreenBounds) ? 1.0f : 0.0f;
		numValues *= DepthTest(centerDepth, depth, g_local.Radius);
		
		for (int i = 0; i < g_local.Radius; i++)
		{
			const int neighborAddr = Gidx4x16.x < SPATIAL_VAR_THREAD_GROUP_SIZE_X ?
											1 + Gidx4x16.x + i + rowToWaveAdjustment :
											// +1 is to skip over loaded values
											1 + Gidx4x16.x - SPATIAL_VAR_THREAD_GROUP_SIZE_X + g_local.Radius + i + rowToWaveAdjustment;

			const float neighborDepth = WaveReadLaneAt(depth, neighborAddr);
			const float depthWeight = DepthTest(centerDepth, neighborDepth, i);
			//float depthWeight = abs(depth - WaveReadLaneAt(depth, neighborAddr)) < (0.05f + 0.001f * abs(g_local.Radius - i)) * depth;

			lumSum += WaveReadLaneAt(lum, neighborAddr) * depthWeight;
			lumSqSum += WaveReadLaneAt(lumSq, neighborAddr) * depthWeight;
			numValues += depthWeight;
		}

		const uint secondHalfLaneIdx = min(rowToWaveAdjustment + Gidx4x16.x + SPATIAL_VAR_THREAD_GROUP_SIZE_X, WaveGetLaneCount() - 1);
		lumSum += WaveReadLaneAt(lumSum, secondHalfLaneIdx);
		lumSqSum += WaveReadLaneAt(lumSqSum, secondHalfLaneIdx);
		numValues += WaveReadLaneAt(numValues, secondHalfLaneIdx);

		// cache depth values that are needed in the vertical pass
		const float depth8x8 = WaveReadLaneAt(depth,
			min(Gidx4x16.x + g_local.Radius + rowToWaveAdjustment, WaveGetLaneCount() - 1));

		// add the two sub-computations (row's first and second halves)
		if (Gidx4x16.x < SPATIAL_VAR_THREAD_GROUP_SIZE_X)
		{
			float2 lumMoments;
			lumMoments.x = lumSum;
			lumMoments.y = lumSqSum;
			
			g_lumMoments[offsetAddr.y][Gidx4x16.x] = lumMoments;
			g_numValidVals[offsetAddr.y][Gidx4x16.x] = half(numValues);
			g_depthCache[offsetAddr.y][Gidx4x16.x] = half(depth8x8);
		}
	}
}

void VerticalPass(uint3 DTid, uint3 GTid)
{
	float lumSum = 0.0f;
	float lumSqSum = 0.0f;
	float numValues = 0.0f;
	
	const half depth = g_depthCache[GTid.y + g_local.Radius][GTid.x];
	
	for (int i = 0; i < 2 * g_local.Radius + 1; i++)
	{
		float2 lumMoments = g_lumMoments[GTid.y + i][GTid.x];
		float numNeighborValues = g_numValidVals[GTid.y + i][GTid.x];

		float neighborDepth = g_depthCache[GTid.y + i][GTid.x];
		float depthWeight = DepthTest(depth, neighborDepth, abs(i - g_local.Radius));

		lumSum += lumMoments.x * depthWeight;
		lumSqSum += lumMoments.y * depthWeight;
		numValues += numNeighborValues * depthWeight;
	}
	
	numValues = max(numValues, 1.0f);
	const float meanLum = lumSum / numValues;
	float varianceLum = (lumSqSum / numValues) - meanLum * meanLum;
	
	// apply Bessel's correction to obtain an unbiased estimate of variance
	// its square root is not an unbiased estimate of standard deviation though
	varianceLum *= (float) numValues / max(numValues - 1.0f, 1.0f);
	varianceLum = max(varianceLum, 0.0f);
	
	RWTexture2D<half> g_outLumVar = ResourceDescriptorHeap[g_local.SpatialLumVarDescHeapIdx];
	g_outLumVar[DTid.xy] = half(varianceLum);
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(SPATIAL_VAR_THREAD_GROUP_SIZE_X, SPATIAL_VAR_THREAD_GROUP_SIZE_Y, SPATIAL_VAR_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float isSurfaceMarker = g_baseColor[DTid.xy].w;

	// skip if this pixel belongs to sky
	const bool isSurface = isSurfaceMarker >= MIN_ALPHA_CUTOFF;
		
	HorizontalPass(Gid, Gidx);
	GroupMemoryBarrierWithGroupSync();
	
	if (isSurface)
		VerticalPass(DTid, GTid);
	else
	{
		RWTexture2D<half> g_outLumVar = ResourceDescriptorHeap[g_local.SpatialLumVarDescHeapIdx];
		g_outLumVar[DTid.xy] = 0.0f;
	}
}