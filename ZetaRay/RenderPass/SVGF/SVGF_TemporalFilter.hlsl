// Implementation of SVGF was inspired by the following sample from Microsoft (under MIT License)
// https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/Samples/Desktop/D3D12Raytracing/src/D3D12RaytracingRealTimeDenoisedAmbientOcclusion

// Essentially, noise is due to the variance of the MC estimate that results from low sampling-rate. By 
// taking advantage of the temporal locality of franes, we can increase the sampling-rate over time. In high 
// frame-rates (e.g. 60 FPS), results are decent.
//
// To that end, a temporal cache needs to be maintained. It should include all the data 
// necessary to perform the integration:
//  - Temporal Samples Per-Pixel (tspp): to keep track of whether history samples up to date at this point in 
//  time. It should be set to zero whenever the history becomes stale
//  - Color: this is the output of first round of atrous wavelet transform filter from the previous frame. By using
//  the filtered signal rather than the raw values, we're trading off bias for noise
//  - Luminance and Luminance^2: These are used to estimate the variance of color luminance over time. The idea 
//  is that variance correlates with noise; a wider spatial filter is used when the input is more noisy.
//  When tspp is low (e.g. disocclusion), this estimate isn't reliable, so variance is 
//  also estimated spatially.

#include "SVGF_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/Material.h"
#include "../Common/StaticTextureSamplers.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbTemporalFilter> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

// computes similarity (consistency) between history and current sample
float4 ComputeNormalConsistency(float3 prevNormals[4], float3 currNormal)
{
	float4 weights = float4(dot(prevNormals[0], currNormal),
	                        dot(prevNormals[1], currNormal),
	                        dot(prevNormals[2], currNormal),
	                        dot(prevNormals[3], currNormal));

	// adjust tolerance of difference; scale > 1 causes tests to be less stringent and vice versa
	weights *= saturate(g_local.BilinearNormalScale * weights);
	weights = pow(weights, g_local.BilinearNormalExp);
	
	return weights;
}

float4 ComputeDepthConsistency(float4 prevDepths, float currDepth, float2 dzdx_dzdy)
{
	// 1st order taylor series approximation (i.e. tangent plane)
	float threshold = abs(dzdx_dzdy.x) + abs(dzdx_dzdy.y);
	threshold = max(threshold, 1e-7f);
	threshold *= g_local.BilinearDepthScale;
	
	// differences in range 0 < |prev - curr| < (ddx + ddy) * DepthScale are consistent
	//float4 weights = min(NextFloat32Up(threshold) / NextFloat32Up(abs(currDepth - prevDepths)), 1.0f);
	float4 weights = min(threshold / max(abs(currDepth - prevDepths), 1e-7f), 1.0f);
	weights *= (weights >= g_local.BilinearDepthCutoff);
	
	return weights;
}

// resample history using a 2x2 bilinear filter with custom weights
float4 SampleTemporalCache(in uint3 DTid, out uint tspp, out float4 irradY_SH, out float irradCo, out float irradCg,
	out float lum, out float lumSq)
{
	// pixel position for this thread
	// reminder: pixel positions are 0.5, 1.5, 2.5, ...
	const float2 screenDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 currPosTS = (DTid.xy + 0.5f) / screenDim;

	// compute pixel position corresponding to current pixel in previous frame (in texture space)
	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
	const half2 motionVec = g_motionVector[DTid.xy];
	const float2 prevPosTS = currPosTS - motionVec;

	// offset of prevPixelPos from the 4 surrounding pixels
	//	p0-----------p1
	//	|-------------|
	//	|--prev-------|
	//	|-------------|
	//	p2-----------p3
	const float2 f = prevPosTS * screenDim;
	const float2 topLeft = floor(f - 0.5f); // e.g if p0 is at (20.5, 30.5), then topLeft would be (20, 30)
	const float2 offset = f - (topLeft + 0.5f);	
	const float2 topLeftTexelUV = (topLeft + 0.5f) / screenDim;
	
	// previous frame's normals
	GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];

	// w (0, 0)		z (1,0)
	// x (0, 1)		y (1, 1)
	const float4 prevNormalsXEncoded = g_prevNormal.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	const float4 prevNormalsYEncoded = g_prevNormal.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
	float3 prevNormals[4];
			
	prevNormals[0] = DecodeUnitNormalFromHalf2(float2(prevNormalsXEncoded.x, prevNormalsYEncoded.x));
	prevNormals[1] = DecodeUnitNormalFromHalf2(float2(prevNormalsXEncoded.y, prevNormalsYEncoded.y));
	prevNormals[2] = DecodeUnitNormalFromHalf2(float2(prevNormalsXEncoded.z, prevNormalsYEncoded.z));
	prevNormals[3] = DecodeUnitNormalFromHalf2(float2(prevNormalsXEncoded.w, prevNormalsYEncoded.w));
	
//	[unroll]
//	for (int i = 0; i < 4; i++)
//		prevNormals[i] = DecodeUnitNormalFromHalf2(half2(prevNormalsXEncoded[i], prevNormalsYEncoded[i]));
		
	// current frame's normals
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 currNormal = DecodeUnitNormalFromHalf2(g_currNormal[DTid.xy].xy);
	const float4 normalWeights = ComputeNormalConsistency(prevNormals, currNormal);

	// previous frame's depth
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float4 prevDepths = g_prevDepth.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	prevDepths = ComputeLinearDepthReverseZ(prevDepths, g_frame.CameraNear);

	// current frame's depth
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float currDepth = ComputeLinearDepthReverseZ(g_currDepth[DTid.xy], g_frame.CameraNear);

	Texture2D<float2> g_linearDepthGrad = ResourceDescriptorHeap[g_local.LinearDepthGradDescHeapIdx];
	const float2 dzdx_dzdy = g_linearDepthGrad[DTid.xy];
	const float4 depthWeights = ComputeDepthConsistency(prevDepths, currDepth, dzdx_dzdy);
	
	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);
		
	// weight must be zero for out-of-bound samples
	const float4 isInBounds = float4(IsInRange(topLeft, screenDim),
									 IsInRange(topLeft + float2(1, 0), screenDim),
									 IsInRange(topLeft + float2(0, 1), screenDim),
									 IsInRange(topLeft + float2(1, 1), screenDim));

	float4 weights = normalWeights * depthWeights * bilinearWeights * isInBounds;	
	const float weightSum = dot(1.0f, weights);
	
	if (g_local.MinConsistentWeight < weightSum)
	{
		// uniformly distribute the weight over the consistent samples
		weights *= rcp(weightSum);
		
		// tspp
		Texture2D<uint> g_prevTspp = ResourceDescriptorHeap[g_local.PrevTemporalCacheTSPPDescHeapIdx];
		uint4 histTspp = g_prevTspp.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
		
		// as tspp is an integer, make sure it's at least 1, otherwise tspp would remain at zero forever
		histTspp = max(1, histTspp);
		tspp = round(dot(histTspp, weights));

		if(tspp > 0)
		{
			// don't accumulate more than MaxTspp temporal samples (temporal lag <-> blur tradeoff)
			tspp = min(tspp + 1, g_local.MaxTspp);

			Texture2D<uint4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheDescHeapIdx];
			const uint4 histR = g_prevTemporalCache.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
			const uint4 histG = g_prevTemporalCache.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
			
			// irradiance, luma in SH1
			float4 histIrradY_SH[4];
			histIrradY_SH[0] = float4(f16tof32(histR.x), f16tof32(histR.x >> 16), f16tof32(histG.x), f16tof32(histG.x >> 16));
			histIrradY_SH[1] = float4(f16tof32(histR.y), f16tof32(histR.y >> 16), f16tof32(histG.y), f16tof32(histG.y >> 16));
			histIrradY_SH[2] = float4(f16tof32(histR.z), f16tof32(histR.z >> 16), f16tof32(histG.z), f16tof32(histG.z >> 16));
			histIrradY_SH[3] = float4(f16tof32(histR.w), f16tof32(histR.w >> 16), f16tof32(histG.w), f16tof32(histG.w >> 16));
			
			irradY_SH = histIrradY_SH[0] * weights[0] +
					histIrradY_SH[1] * weights[1] +
					histIrradY_SH[2] * weights[2] +
					histIrradY_SH[3] * weights[3];
			
			// irradiance, CoCg 
			const uint4 histB = g_prevTemporalCache.GatherBlue(g_samPointClamp, topLeftTexelUV).wzxy;
			const float4 histCo = f16tof32(histB);
			const float4 histCg = f16tof32(histB >> 16);
			
			irradCo = dot(weights, histCo);
			irradCg = dot(weights, histCg);
			
			// lum
			const uint4 histA = g_prevTemporalCache.GatherAlpha(g_samPointClamp, topLeftTexelUV).wzxy;
			const float4 lumHist = f16tof32(histA);
			lum = dot(weights, lumHist);
			
			// lum^2
			const float4 lumSqHist = f16tof32(histA >> 16);
			lumSq = dot(weights, lumSqHist);
		}
	}
	
	return weights;
}

void Integrate(in uint3 DTid, inout uint tspp, inout float4 temporalIrradY_SH, inout float temporalIrradCo, inout float temporalIrradCg, 
	inout float temporalLum, inout float temporalLumSq)
{
	Texture2D<uint4> g_indirectLi = ResourceDescriptorHeap[g_local.IndirectLiRayTDescHeapIdx];
	const uint4 noisySignal = g_indirectLi[DTid.xy];
	const float currLum = asfloat(noisySignal.w);
	const float currLumSq = currLum * currLum;

	const float4 currY_Sh = float4(f32tof16(noisySignal.x), f32tof16(noisySignal.x >> 16), f32tof16(noisySignal.y), f32tof16(noisySignal.y >> 16));
	const float currCo = f32tof16(noisySignal.z);
	const float currCg = f32tof16(noisySignal.z >> 16);
	
	RWTexture2D<half> g_spatialLumVar = ResourceDescriptorHeap[g_local.SpatialLumVarDescHeapIdx];
	float lumVariance;
	
	if (tspp > 100)
	{
		// don't accumulate more than MaxTspp temporal samples (temporal lag <-> blur tradeoff)
		tspp = min(tspp + 1, g_local.MaxTspp);
		
		// use linear weights as opposed to exponential weights (used in the paper), which
		// comparatively give higher weight to initial samples (useful for right after disocclusion)
//		const float accumulationSpeed = clamp(1.0f / (1.0f + tspp), g_local.MinAccumulationSpeed, 1.0f);
		const float accumulationSpeed = 1.0f / (1.0f + tspp);
		
		// Y
		temporalIrradY_SH = lerp(temporalIrradY_SH, currY_Sh, accumulationSpeed);

		// Co
		temporalIrradCo = lerp(temporalIrradCo, currCo, accumulationSpeed);

		// Cg
		temporalIrradCg = lerp(temporalIrradCg, currCg, accumulationSpeed);
		
		// lum
		temporalLum = lerp(temporalLum, currLum, accumulationSpeed);
		
		// lum^2
		temporalLumSq = lerp(temporalLumSq, currLumSq, accumulationSpeed);
		
		// variance		
		// if tspp is too low, use the spatial estimate instead
		if (tspp > g_local.MinTsppToUseTemporalVar)
		{
			// var = E[X^2]- E[X]^2
			// E[X^2] = lum^2
			// E[X] = lum
			const float meanLum = temporalLum / tspp;
			float temporalVariance = (temporalLumSq / tspp) - meanLum * meanLum;
			temporalVariance *= (float) tspp / (tspp - 1.0f);
			temporalVariance = max(temporalVariance, 0.0f);
		
			lumVariance = temporalVariance;

			// TODO is this necessary?
			//lumVariance = max(lumVariance, g_local.MinLumVariance);

			// rewrtie var
			g_spatialLumVar[DTid.xy] = half(lumVariance);
		}
	}
	else
	{
		tspp = 1;
		temporalIrradY_SH = currY_Sh;
		temporalIrradCo = currCo;
		temporalIrradCg = currCg;
		temporalLum = currLum;
		temporalLumSq = currLumSq;
	}
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(TEMPORAL_FILTER_THREAD_GROUP_SIZE_X, TEMPORAL_FILTER_THREAD_GROUP_SIZE_Y, TEMPORAL_FILTER_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	if(!IsInRange(DTid.xy, uint2(g_frame.RenderWidth, g_frame.RenderHeight)))
		return;
	
	uint tspp = 0;
	float4 irradY_SH = 0.0f.xxxx;
	float irradCo = 0.0f;
	float iradCg = 0.0f;
	float lum = 0.0f;
	float lumSq = 0.0f;

	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float isSurfaceMarker = g_baseColor[DTid.xy].w;

	// skip if this pixel belongs to sky
	if (isSurfaceMarker < MIN_ALPHA_CUTOFF)
		return;
	
	float4 w = 0;
	// sample temporal cache using a bilinear tap with custom weights
	if (g_local.IsTemporalCacheValid)
		w = SampleTemporalCache(DTid, tspp, irradY_SH, irradCo, iradCg, lum, lumSq);

	// integrate history and current frame
	Integrate(DTid, tspp, irradY_SH, irradCo, iradCg, lum, lumSq);

	uint4 ret;
	ret.x = (f32tof16(irradY_SH.y) << 16) | f32tof16(irradY_SH.x);
	ret.y = (f32tof16(irradY_SH.w) << 16) | f32tof16(irradY_SH.z);
	ret.z = (f32tof16(iradCg) << 16) | f32tof16(irradCo);
	ret.w = (f32tof16(lumSq) << 16) | f32tof16(lum);
	ret.z = asuint(tspp / 32.0f);
	
	RWTexture2D<uint4> g_nextTemporalCache = ResourceDescriptorHeap[g_local.CurrTemporalCacheDescHeapIdx];
	g_nextTemporalCache[DTid.xy] = ret;
	
	RWTexture2D<uint> g_temporalCahceTSPP = ResourceDescriptorHeap[g_local.NextTemporalCacheTSPPDescHeapIdx];
	g_temporalCahceTSPP[DTid.xy] = tspp;
}