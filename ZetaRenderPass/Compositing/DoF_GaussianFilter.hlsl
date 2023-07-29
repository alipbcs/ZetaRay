// Ref: https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12Raytracing/src/D3D12RaytracingRealTimeDenoisedAmbientOcclusion/RTAO/Shaders/Denoising/DepthAwareSeparableGaussianFilter3x3CS_AnyToAnyWaveReadLaneAt.hlsl

#include "Compositing_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/GBuffers.hlsli"

//--------------------------------------------------------------------------------------
// Gloabal Values
//--------------------------------------------------------------------------------------

static const int Radius = 1;
static const float Kernel[2 * Radius + 1] = { 0.27901f, 0.44198f, 0.27901f };

// NumToLoadPerRowOrColumn = 8 + 2 * R pixel values need to be loaded for the filtering
// of each row or column where 10 <= NumToLoadPerRowOrColumn <= 16. Note that ALL
// 16 threads participate in the final computation though
static const int2 GroupDim = int2(COMPOSITING_THREAD_GROUP_DIM_X, COMPOSITING_THREAD_GROUP_DIM_Y);
static const int NumToLoadPerRowOrColumn = 8 + Radius * 2;

groupshared float3 g_horizontallyFiltered[NumToLoadPerRowOrColumn][8];

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbGaussianFilter> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Horizontal convolution
//--------------------------------------------------------------------------------------

void HorizontalPass(uint3 Gid, uint Gidx)
{
	const int2 screenDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);
	const int2 blockFirstPixel = int2(Gid.xy) * GroupDim - Radius;

	// reshape original 8x8 group into 4x16 so that every 16 threads work on one image row
	const uint2 Gidx4x16 = uint2(Gidx & (16 - 1), Gidx >> 4);

	Texture2D<float4> g_gather = ResourceDescriptorHeap[g_local.GatherSrvDescHeapIdx];

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
        
		float3 val = 0.0f.xxx;
		
		// avoid out-of-bound texture reads
		if (Gidx4x16.x < 16 && Math::IsWithinBounds(pixelAddr, screenDim))
			val = abs(g_gather[pixelAddr].rgb);
	
		// wave could span more than one row
		const uint rowToWaveAdjustment = (WaveGetLaneIndex() >> 4) * 16; // biggest multiple of 16 smaller than lane-index
	        
		// 16 threads in each row operate on image rows of 8 values. Filtering each pixel consists of summing 
		// over 2 * Radius + 1 values. This computation is split between the first and second 8 threads in each 
		// row
		float3 filtered = Gidx4x16.x < GroupDim.x ? val * Kernel[0] : 0.0f;

		// convolution sum
		for (int i = 0; i < Radius; i++)
		{
			const int neighborAddr = Gidx4x16.x < GroupDim.x ? 
											// +1 is to skip over first pixel
											1 + Gidx4x16.x + i + rowToWaveAdjustment :
											1 + Gidx4x16.x - GroupDim.x + Radius + i + rowToWaveAdjustment;

			const float3 neighborVal = WaveReadLaneAt(val, neighborAddr);          
			const int kernelIdx = Gidx4x16.x < GroupDim.x ? i + 1 : Radius + i + 1;
			filtered += neighborVal * Kernel[kernelIdx];
		}
		
		// sum the first & second sums
		const uint secondHalfLaneIdx = min(rowToWaveAdjustment + Gidx4x16.x + GroupDim.x, WaveGetLaneCount() - 1);
		filtered += WaveReadLaneAt(filtered, secondHalfLaneIdx);
		
		if (Gidx4x16.x < GroupDim.x)
		{			
			g_horizontallyFiltered[offsetAddr.y][Gidx4x16.x] = filtered;
		}
	}
}

//--------------------------------------------------------------------------------------
// Vertical convolution
//--------------------------------------------------------------------------------------

float3 VerticalPass(uint3 DTid, uint3 GTid)
{
	float3 filtered = 0.0f;
	
	// for the corresponding 8x8 pixel block, g_horizontallyFiltered contains Rx8 horizontally 
	// filtered values such that the original 8x8 spans from row E to row 8 + E. This way, all 
	// the extra values needed for the vertical pass are already there
	[unroll]
	for (int r = 0; r < 2 * Radius + 1; r++)
	{
		filtered += g_horizontallyFiltered[GTid.y + r][GTid.x] * Kernel[r];
	}
	
	return filtered;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(GAUSSIAN_FILTER_THREAD_GROUP_DIM_X, GAUSSIAN_FILTER_THREAD_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
	Texture2D<float4> g_gather = ResourceDescriptorHeap[g_local.GatherSrvDescHeapIdx];
	float3 color = g_gather[DTid.xy].rgb;
	const bool needsFilter = any(color < 0);
	color = abs(color);
	
	HorizontalPass(Gid, Gidx);
	GroupMemoryBarrierWithGroupSync();
	
	RWTexture2D<float4> g_color = ResourceDescriptorHeap[g_local.FilteredUavDescHeapIdx];
	
	if (needsFilter)
	{
		float3 filtered = VerticalPass(DTid, GTid);
		g_color[DTid.xy].rgb = filtered;
	}
	else
		g_color[DTid.xy].rgb = color.rgb;
}