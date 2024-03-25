#include "AutoExposure_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"

#define SKIP_OUTSIDE_PERCENTILE_RANGE 0

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

#if SKIP_OUTSIDE_PERCENTILE_RANGE == 1
groupshared uint g_waveSampleCount[MaxNumWaves];
#endif

[numthreads(HIST_BIN_COUNT, 1, 1)]
void main(uint Gidx : SV_GroupIndex)
{
    // Exclude the first (invalid) bin
    const bool isFirstBin = (Gidx == 0);
    const uint binSize = isFirstBin ? 0 : g_hist.Load(Gidx * sizeof(uint));
    const uint numLanesInWave = WaveGetLaneCount();
    const uint wave = Gidx / numLanesInWave;
    const uint numWavesInGroup = HIST_BIN_COUNT / numLanesInWave;	// HIST_BIN_COUNT is always divisible by wave size
    const uint numExcludedSamples = g_hist.Load(0);
    const uint numSamples = g_frame.RenderWidth * g_frame.RenderHeight - numExcludedSamples;

    // Prefix sum for the whole group to calculate the percentiles up to each bin
#if SKIP_OUTSIDE_PERCENTILE_RANGE == 1
    uint binPercentile = WavePrefixSum(binSize) + binSize;

    if (WaveGetLaneIndex() == WaveGetLaneCount() - 1)
        g_waveSampleCount[wave] = binPercentile;
#endif

    GroupMemoryBarrierWithGroupSync();

    // Add in the samples from previous waves
#if SKIP_OUTSIDE_PERCENTILE_RANGE == 1
    for (int w = 0; w < wave; w++)
        binPercentile += g_waveSampleCount[w];

    const uint lowerPercentileNumSamples = (uint)(numSamples * g_local.LowerPercentile);
    const uint upperPercentileNumSamples = (uint)(numSamples * g_local.UpperPercentile);

    // Exclude bins that don't fall in the intended range
    bool skip = (binPercentile < lowerPercentileNumSamples) || (binPercentile > upperPercentileNumSamples);
    skip = skip || isFirstBin;
#else
    bool skip = isFirstBin;
#endif

    // Skipped bins don't contribute to average
    float val = skip ? 0 : binSize * (Gidx - 1 + 0.5f) / HIST_BIN_COUNT;
    val = WaveActiveSum(val);

    if (WaveIsFirstLane())
        g_waveSum[wave] = val;

    GroupMemoryBarrierWithGroupSync();

    // Sum across the waves
    float mean = Gidx < numWavesInGroup ? g_waveSum[Gidx] : 0.0;
    // Assuming min wave size of at least 16, there are at most 16 values to sum together, 
    // so one WaveActiveSum is enough
    mean = WaveActiveSum(mean);
    mean /= max(numSamples, 1);

    if (Gidx == 0)
    {
        // Do the inverse mapping
        float result = pow(mean, 1.0 / g_local.LumMapExp);
        result = result * g_local.LumRange + g_local.MinLum;

        RWTexture2D<float2> g_out = ResourceDescriptorHeap[g_local.ExposureDescHeapIdx];
        float prev = g_out[int2(0, 0)].y;

        if (prev < 1e8f)
            result = prev + (result - prev) * (1 - exp(-g_frame.dt * 1000.0f * g_local.AdaptationRate));

        float exposure = ComputeAutoExposure(result);
        g_out[int2(0, 0)] = float2(exposure, result);
    }
}