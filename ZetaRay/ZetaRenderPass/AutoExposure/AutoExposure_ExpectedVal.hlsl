#include "AutoExposure_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbAutoExposureHist> g_local : register(b1);
RWByteAddressBuffer g_hist : register(u0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

float ComputeAutoExposure(float avgLum)
{
	const float S = 100.0f;
	const float K = 12.5f;
	const float EV100 = log2((avgLum * S) / K);
	const float q = 0.65f;
	const float luminanceMax = (78.0f / (q * S)) * pow(2.0f, EV100);
	return 1 / luminanceMax;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

static const int MinWaveSize = 16;
static const int MaxNumWaves = HIST_BIN_COUNT / MinWaveSize;
groupshared float g_waveSum[MaxNumWaves];
groupshared uint g_waveSampleCount[MaxNumWaves];

[numthreads(HIST_BIN_COUNT, 1, 1)]
void main(uint Gidx : SV_GroupIndex)
{
	// exclude the first (invalid) & last (too bright) bins
	const bool isFirstOrLastBin = (Gidx == 0) || (Gidx == (HIST_BIN_COUNT - 1));
	const int binSize = isFirstOrLastBin ? 0 : g_hist.Load(Gidx * sizeof(uint));
	const int numLanesInWave = WaveGetLaneCount();
	const int wave = Gidx / numLanesInWave;
	const int numWavesInGroup = HIST_BIN_COUNT / numLanesInWave;	// HIST_BIN_COUNT is always divisible by wave size
	const int numExcludedSamples = g_hist.Load(0) + g_hist.Load((HIST_BIN_COUNT - 1) * sizeof(uint));
	const int numSamples = g_frame.RenderWidth * g_frame.RenderHeight - numExcludedSamples;

	// do a prefix sum for the whole group to calculate the percentile up to each bin
	uint binPercentile = WavePrefixSum(binSize) + binSize;
	
	if (WaveGetLaneIndex() == WaveGetLaneCount() - 1)
		g_waveSampleCount[wave] = binPercentile;
	
	GroupMemoryBarrierWithGroupSync();

	// add in the samples from previous waves
	for (int w = 0; w < wave; w++)
		binPercentile += g_waveSampleCount[w];

	const uint lowerPercentileNumSamples = (uint) (numSamples * g_local.LowerPercentile);
	const uint upperPercentileNumSamples = (uint) (numSamples * g_local.UpperPercentile);

	// exclude bins that don't fall in the intended range
	bool skip = (binPercentile < lowerPercentileNumSamples) || (binPercentile > upperPercentileNumSamples);
	skip = skip || isFirstOrLastBin;
	
	// skipped bins don't contribute to average
	float val = skip ? 0 : binSize * (Gidx - 1 + 0.5f) / HIST_BIN_COUNT;
	val = WaveActiveSum(val);
	
	if (WaveIsFirstLane())
		g_waveSum[wave] = val;
	
	GroupMemoryBarrierWithGroupSync();
	
	// sum across the waves
	float expectedVal = Gidx < numWavesInGroup ? g_waveSum[Gidx] : 0.0;
	// assuming min wave size of 16, there are at most 16 values to sum together, so one WaveActiveSum is enough
	expectedVal = WaveActiveSum(expectedVal);
	expectedVal /= max(numSamples, 1);
	
	if (Gidx == 0)
	{
		// undo the mapping
		float result = pow(expectedVal, 1.0 / g_local.LumMapExp);
		// bring it to the given luminance range
		result = result * g_local.LumRange + g_local.MinLum;
		
		RWTexture2D<float2> g_out = ResourceDescriptorHeap[g_local.ExposureDescHeapIdx];
		float prev = g_out[int2(0, 0)].y;
		
		if (prev < 1e8f)
		{
			float rate = 1.0f;
			result = prev + (result - prev) * (1 - exp(-g_frame.dt * 1000.0f * g_local.AdaptationRate));
		}
			
		float exposure = ComputeAutoExposure(result);
		g_out[int2(0, 0)] = float2(exposure, result);
	}
}