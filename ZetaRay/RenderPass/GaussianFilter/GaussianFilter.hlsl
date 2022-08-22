#include "GaussianFilter_Common.h"
#include "../Common/Common.hlsli"

//--------------------------------------------------------------------------------------
// Kernel weights for 3x3 Gaussian
//--------------------------------------------------------------------------------------

static const int Radius = 1;
static const float Kernel1D[2 * Radius + 1] = { 0.27901f, 0.44198f, 0.27901f };

// NumToLoadPerRowOrColumn = 8 + 2 * R pixel values need to be loaded for the filtering
// of each row or column where 10 <= NumToLoadPerRowOrColumn <= 16. Note that ALL
// 16 threads participate in the final computation though
static const int NumRowsForVerticalPass = GAUSSAIN_FILT_THREAD_GROUP_SIZE_X + Radius * 2;

groupshared float3 g_shared[NumRowsForVerticalPass][GAUSSAIN_FILT_THREAD_GROUP_SIZE_X];

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbGaussianFilter> g_local : register(b0, space0);
static const int2 GroupDim = int2(GAUSSAIN_FILT_THREAD_GROUP_SIZE_X, GAUSSAIN_FILT_THREAD_GROUP_SIZE_Y);

//--------------------------------------------------------------------------------------
// Horizontal convolution
//--------------------------------------------------------------------------------------

void HorizontalPass(uint3 Gid, uint Gidx)
{
	int2 blockFirstPixel = Gid.xy * GroupDim;
	blockFirstPixel -= Radius;

	Texture2D<float4> g_signal = ResourceDescriptorHeap[g_local.InputDescHeapIdx];

	// reshape original 8x8 into 4x16 so that each wave corresponds to one image row
	const uint2 Gidx4x16 = uint2(Gidx & (16 - 1), Gidx >> 4);
	
    // Horizontally filter 4 rows of 16 threads. Next iterations filters
    // next 4 rows and so on. In total, this 8x8 block horizontally filteres
	// R(=#Rows)x8 pixels where 10 <= R <= 16, corresponding to 3x3 to 9x9 kernels.
	// In this way, Radius more rows above and Radius more rows below the 
	// original 8x8 block are loaded and
	// Rows = 2 * Radius + 8		e.g. for 3x3 filter, we filter 10x8 pixels, where 1
	//								extra row below and one extra row above the 8x8 are fitered.
    [unroll]
	for (int step = 0; step < 4; step++)
	{
		int2 offsetAddr = Gidx4x16 + int2(0, step * 4);
		uint2 pixelAddr = blockFirstPixel + offsetAddr;

		// R(=#Rows) <= x <= 16 don't need to participate in filtering
		if (offsetAddr.y >= NumRowsForVerticalPass)
			break;
        
		float3 val = float3(0.0f, 0.0f, 0.0f);
		
		// avoid out-of-bound texture reads. This thread still participates in the
		// kernel computation though
		if (Gidx4x16.x < NumRowsForVerticalPass && IsInBounds(pixelAddr, uint2(g_local.InputWidth, g_local.InputHeight)))
			val = g_signal[pixelAddr].xyz;
    
		// wave could span more than one row
		const uint rowToWaveAdjustment = (WaveGetLaneIndex() >> 4) * 16;		// nearest smallest multiple of 16
        
		// 16 threads in each row operate on image rows of 8 values. Filtering pixel in row i, column j where 
		// 0 <= i <= NumRowsForVerticalPass and 0 <= j <= 8 consists of summing over 2 * Radius + 1 values. Split 
		// the sum between 1st and 2nd 8 threads in each row. Pixel summation to thread mapping is such that 
		// pixel[i][j] is filtered by threads with 4x16 thread IDs of (i % 4, j) for first half (j < 8)
		// and (i % 4, j - 8 + Radius) for 2nd half (j >= 8).
		float3 filtered = float3(0.0f, 0.0f, 0.0f);

		// sum over the non-center kernel values
	    for (int i = 0; i < Radius; i++)
	    {
			float3 neighborVal = Gidx4x16.x < GroupDim.x ? WaveReadLaneAt(val, Gidx4x16.x + i + rowToWaveAdjustment) :
														   // + 1 is to skip over center value
														   WaveReadLaneAt(val, (int)Gidx4x16.x - GroupDim.x + Radius + 1 + i + rowToWaveAdjustment);
            
			filtered += neighborVal * Kernel1D[i];
		}

        // add in the contribution from the kernel's center value
		uint laneIdx = Gidx4x16.x + Radius + rowToWaveAdjustment;
		float3 centerVal = Gidx4x16.x < GroupDim.x ? WaveReadLaneAt(val, laneIdx) : float3(0.0f, 0.0f, 0.0f);
		filtered += centerVal * Kernel1D[Radius];
        
		if (Gidx4x16.x < GroupDim.x)
		{
			// sum the first & second half's sums
			filtered += WaveReadLaneAt(filtered, rowToWaveAdjustment + Gidx4x16.x + GroupDim.x);
			g_shared[offsetAddr.y][Gidx4x16.x] = filtered;
		}
	}
}

//--------------------------------------------------------------------------------------
// Vertical convolution
//--------------------------------------------------------------------------------------

void VerticalPass(uint3 DTid, uint3 GTid)
{
	float3 filtered = float3(0.0f, 0.0f, 0.0f);
	
	// for corresponding 8x8 pixel block, g_shared contains Rx8 horizontally filtered
	// values such that original 8x8 spans from row E to row 8 + E. This way, all the extra
	// values needed for vertical pass are already there
	[unroll]
	for (int i = 0; i < 2 * Radius + 1; i++)
	{
		filtered += g_shared[GTid.y + i][GTid.x] * Kernel1D[i];
	}

	RWTexture2D<float3> g_filtered = ResourceDescriptorHeap[g_local.OutputDescHeapIdx];

	g_filtered[DTid.xy] = filtered;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(GAUSSAIN_FILT_THREAD_GROUP_SIZE_X, GAUSSAIN_FILT_THREAD_GROUP_SIZE_Y, GAUSSAIN_FILT_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
	HorizontalPass(Gid, Gidx);
	GroupMemoryBarrierWithGroupSync();
	VerticalPass(DTid, GTid);
}