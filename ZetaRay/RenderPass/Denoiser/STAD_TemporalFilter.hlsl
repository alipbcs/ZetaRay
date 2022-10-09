#include "STAD_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/Material.h"
#include "../Common/StaticTextureSamplers.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbSTADTemporalFilter> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);
StructuredBuffer<uint> g_owenScrambledSobolSeq : register(t0, space0);
StructuredBuffer<uint> g_scramblingTile : register(t1, space0);
StructuredBuffer<uint> g_rankingTile : register(t2, space0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

float4 ComputeGeometricConsistency(float4 prevDepths, float2 prevUVs[4], float3 currNormal, float3 currPos)
{
	float3 prevPos[4];
	prevPos[0] = Common::WorldPosFromUV(prevUVs[0], prevDepths.x, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv, g_frame.PrevCameraJitter);
	prevPos[1] = Common::WorldPosFromUV(prevUVs[1], prevDepths.y, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv, g_frame.PrevCameraJitter);
	prevPos[2] = Common::WorldPosFromUV(prevUVs[2], prevDepths.z, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv, g_frame.PrevCameraJitter);
	prevPos[3] = Common::WorldPosFromUV(prevUVs[3], prevDepths.w, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv, g_frame.PrevCameraJitter);
	
	float4 planeDist = float4(dot(currNormal, prevPos[0] - currPos),
		dot(currNormal, prevPos[1] - currPos),
		dot(currNormal, prevPos[2] - currPos),
		dot(currNormal, prevPos[3] - currPos));
	
//	float4 weights = saturate(1 - abs(planeDist) / g_local.MaxPlaneDist);
	float4 weights = planeDist <= g_local.MaxPlaneDist;
	
	return weights;
}

float4 ComputeNormalConsistency(float3 prevNormals[4], float3 currNormal)
{
	float4 weights = float4(dot(prevNormals[0], currNormal),
	                        dot(prevNormals[1], currNormal),
	                        dot(prevNormals[2], currNormal),
	                        dot(prevNormals[3], currNormal));

	// adjust tolerance of difference; scale > 1 causes tests to be less stringent and vice versa
	weights = saturate(g_local.BilinearNormalScale * weights);
	weights = pow(weights, g_local.BilinearNormalExp);
	
	return weights;
}

// resample history using a 2x2 bilinear filter with custom weights
void SampleTemporalCache(in uint3 DTid, inout uint tspp, out float3 color)
{
	// pixel position for this thread
	// reminder: pixel positions are 0.5, 1.5, 2.5, ...
	const float2 screenDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 currUV = (DTid.xy + 0.5f) / screenDim;

	// compute pixel position corresponding to current pixel in previous frame (in texture space)
	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
	const half2 motionVec = g_motionVector[DTid.xy];
	const float2 prevUV = currUV - motionVec;

	if (any(abs(prevUV) - prevUV))
		return;
	
	// offset of prevPixelPos from surrounding pixels
	//	p0-----------p1
	//	|-------------|
	//	|--prev-------|
	//	|-------------|
	//	p2-----------p3
	const float2 f = prevUV * screenDim;
	const float2 topLeft = floor(f - 0.5f); // e.g if p0 is at (20.5, 30.5), then topLeft would be (20, 30)
	const float2 offset = f - (topLeft + 0.5f);
	const float2 topLeftTexelUV = (topLeft + 0.5f) / screenDim;
		
	/*
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
		
	const float4 normalWeights = ComputeNormalConsistency(prevNormals, currNormal);
	*/
	
	// current frame's normals
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 currNormal = Common::DecodeUnitNormalFromHalf2(g_currNormal[DTid.xy].xy);

	// previous frame's depth
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float4 prevDepths = g_prevDepth.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	prevDepths = Common::ComputeLinearDepthReverseZ(prevDepths, g_frame.CameraNear);

	// current frame's depth
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float currLinearDepth = Common::ComputeLinearDepthReverseZ(g_currDepth[DTid.xy], g_frame.CameraNear);

	const float3 currPos = Common::WorldPosFromUV(currUV, currLinearDepth, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.CurrViewInv, g_frame.CurrCameraJitter);
	
	float2 prevUVs[4];
	prevUVs[0] = topLeftTexelUV;
	prevUVs[1] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 0.0f);
	prevUVs[2] = topLeftTexelUV + float2(0.0f, 1.0f / g_frame.RenderHeight);
	prevUVs[3] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 1.0f / g_frame.RenderHeight);
	const float4 geoWeights = ComputeGeometricConsistency(prevDepths, prevUVs, currNormal, currPos);
	
	// weight must be zero for out-of-bound samples
	const float4 isInBounds = float4(Common::IsWithinBounds(topLeft, screenDim),
									 Common::IsWithinBounds(topLeft + float2(1, 0), screenDim),
									 Common::IsWithinBounds(topLeft + float2(0, 1), screenDim),
									 Common::IsWithinBounds(topLeft + float2(1, 1), screenDim));

	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);
	
	//float4 weights = geoWeights * normalWeights * bilinearWeights * isInBounds;
	float4 weights = geoWeights * bilinearWeights * isInBounds;
	const float weightSum = dot(1.0f, weights);

	if (1e-6f < weightSum)
	{
		// uniformly distribute the weight over the consistent samples
		weights *= rcp(weightSum);

		// tspp
		Texture2D<float4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheDescHeapIdx];
		uint4 histTspp = (uint4) g_prevTemporalCache.GatherAlpha(g_samPointClamp, topLeftTexelUV).wzxy;
		
		// as tspp is an integer, make sure it's at least 1, otherwise tspp would remain at zero forever
		uint maxTspp = max(max(histTspp.x, histTspp.y), max(histTspp.z, histTspp.w));
		histTspp = max(1, histTspp);
		tspp = min(round(dot(histTspp, weights)), maxTspp + 1);
		
		if (tspp > 0)
		{
			// color
			float3 colorHistSamples[4];
			const float4 histR = g_prevTemporalCache.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
			const float4 histG = g_prevTemporalCache.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
			const float4 histB = g_prevTemporalCache.GatherBlue(g_samPointClamp, topLeftTexelUV).wzxy;
			
			colorHistSamples[0] = float3(histR.x, histG.x, histB.x);
			colorHistSamples[1] = float3(histR.y, histG.y, histB.y);
			colorHistSamples[2] = float3(histR.z, histG.z, histB.z);
			colorHistSamples[3] = float3(histR.w, histG.w, histB.w);

			color = colorHistSamples[0] * weights[0] +
					colorHistSamples[1] * weights[1] +
					colorHistSamples[2] * weights[2] +
					colorHistSamples[3] * weights[3];
		}		
	}
}

void Integrate(in uint3 DTid, inout uint tspp, inout float3 color)
{
	Texture2D<float4> g_indirectLiRayT = ResourceDescriptorHeap[g_local.IndirectLiRayTDescHeapIdx];
	const float3 noisySignal = g_indirectLiRayT[DTid.xy].rgb;
	
	// use linear weights as opposed to exponential weights (used in the paper), which
	// comparatively give higher weight to initial samples (useful for right after disocclusion)
	const float accumulationSpeed = 1.0f / (1.0f + tspp);
	
	// don't accumulate more than MaxTspp temporal samples (temporal lag <-> noise tradeoff)
	tspp = min(tspp + 1, g_local.MaxTspp);

	// color
	color = lerp(color, noisySignal, accumulationSpeed);
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(STAD_TEMPORAL_PASS_THREAD_GROUP_SIZE_X, STAD_TEMPORAL_PASS_THREAD_GROUP_SIZE_Y, STAD_TEMPORAL_PASS_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	if (!Common::IsWithinBounds(DTid.xy, uint2(g_frame.RenderWidth, g_frame.RenderHeight)))
		return;
	
	uint tspp = 0;
	float3 color = 0.0f.xxx;

	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float isSurfaceMarker = g_baseColor[DTid.xy].w;

	// skip if this pixel belongs to sky
	if (isSurfaceMarker < MIN_ALPHA_CUTOFF)
		return;
	
	// sample temporal cache using a bilinear tap with custom weights
	if (g_local.IsTemporalCacheValid)
		SampleTemporalCache(DTid, tspp, color);

	// integrate history and current frame
	Integrate(DTid, tspp, color);

	RWTexture2D<float4> g_nextTemporalCache = ResourceDescriptorHeap[g_local.CurrTemporalCacheDescHeapIdx];
	g_nextTemporalCache[DTid.xy].xyzw = float4(color, tspp);
}