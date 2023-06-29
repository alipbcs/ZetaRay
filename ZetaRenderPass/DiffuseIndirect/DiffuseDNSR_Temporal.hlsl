#include "Reservoir_Diffuse.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/BRDF.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"

#define DISOCCLUSION_TEST_RELATIVE_DELTA_BILINEAR 0.005f
#define DISOCCLUSION_TEST_RELATIVE_DELTA_CR 0.01f
#define DISOCCLUSION_TEST_NORMAL_EXP 8

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbDiffuseDNSRTemporal> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

float4 GeometricTest(float4 prevDepths, float2 prevUVs[4], float3 currNormal, float3 currPos, float linearDepth)
{
	float3 prevPos[4];
	prevPos[0] = Math::Transform::WorldPosFromUV(prevUVs[0], prevDepths.x, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	prevPos[1] = Math::Transform::WorldPosFromUV(prevUVs[1], prevDepths.y, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	prevPos[2] = Math::Transform::WorldPosFromUV(prevUVs[2], prevDepths.z, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	prevPos[3] = Math::Transform::WorldPosFromUV(prevUVs[3], prevDepths.w, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	
	float4 planeDist = float4(dot(currNormal, prevPos[0] - currPos),
		dot(currNormal, prevPos[1] - currPos),
		dot(currNormal, prevPos[2] - currPos),
		dot(currNormal, prevPos[3] - currPos));
	
	float4 weights = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA_BILINEAR * linearDepth;
	
	return weights;
}

float GeometricTest(float prevDepth, float2 prevUV, float3 currNormal, float3 currPos, float linearDepth)
{
	float3 prevPos = Math::Transform::WorldPosFromUV(prevUV, prevDepth, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	
	float planeDist = dot(currNormal, prevPos - currPos);
	float weight = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA_CR * linearDepth;
	
	return weight;
}

float4 NormalTest(float3 prevNormals[4], float3 currNormal)
{
	float4 weights = float4(dot(prevNormals[0], currNormal),
	                        dot(prevNormals[1], currNormal),
	                        dot(prevNormals[2], currNormal),
	                        dot(prevNormals[3], currNormal));

	weights = saturate(1 * weights);
	weights = pow(weights, DISOCCLUSION_TEST_NORMAL_EXP);
	
	return weights;
}

void SampleTemporalCache_Bilinear(uint2 DTid, float3 currPos, float3 currNormal, float linearDepth, float2 currUV, float2 prevUV,
	inout float tspp, out float3 color)
{
	// offset of history pos from surrounding pixels
	//	p0-----------p1
	//	|-------------|
	//	|--prev-------|
	//	|-------------|
	//	p2-----------p3
	const float2 screenDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 f = prevUV * screenDim;
	const float2 topLeft = floor(f - 0.5f); // e.g if p0 is at (20.5, 30.5), then topLeft would be (20, 30)
	const float2 offset = f - (topLeft + 0.5f);
	const float2 topLeftTexelUV = (topLeft + 0.5f) / screenDim;
		
	// previous frame's normals
	GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];

	// w (0, 0)		z (1,0)
	// x (0, 1)		y (1, 1)
	const half4 prevNormalsXEncoded = g_prevNormal.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	const half4 prevNormalsYEncoded = g_prevNormal.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
	float3 prevNormals[4];
			
	prevNormals[0] = Math::Encoding::DecodeUnitNormal(half2(prevNormalsXEncoded.x, prevNormalsYEncoded.x));
	prevNormals[1] = Math::Encoding::DecodeUnitNormal(half2(prevNormalsXEncoded.y, prevNormalsYEncoded.y));
	prevNormals[2] = Math::Encoding::DecodeUnitNormal(half2(prevNormalsXEncoded.z, prevNormalsYEncoded.z));
	prevNormals[3] = Math::Encoding::DecodeUnitNormal(half2(prevNormalsXEncoded.w, prevNormalsYEncoded.w));
		
	const float4 normalWeights = NormalTest(prevNormals, currNormal);
	
	// previous frame's depth
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float4 prevDepths = g_prevDepth.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	prevDepths = Math::Transform::LinearDepthFromNDC(prevDepths, g_frame.CameraNear);
	
	float2 prevUVs[4];
	prevUVs[0] = topLeftTexelUV;
	prevUVs[1] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 0.0f);
	prevUVs[2] = topLeftTexelUV + float2(0.0f, 1.0f / g_frame.RenderHeight);
	prevUVs[3] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 1.0f / g_frame.RenderHeight);
	const float4 geoWeights = GeometricTest(prevDepths, prevUVs, currNormal, currPos, linearDepth);
	
	// weight must be zero for out-of-bound samples
	const float4 isInBounds = float4(Math::IsWithinBoundsExc(topLeft, screenDim),
									 Math::IsWithinBoundsExc(topLeft + float2(1, 0), screenDim),
									 Math::IsWithinBoundsExc(topLeft + float2(0, 1), screenDim),
									 Math::IsWithinBoundsExc(topLeft + float2(1, 1), screenDim));

	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);
	
	float4 weights = geoWeights * normalWeights * bilinearWeights * isInBounds;
	//float4 weights = geoWeights * bilinearWeights * isInBounds;
	// zero out samples with very low weights to avoid bright spots
	weights *= weights > 1e-3f;
	const float weightSum = dot(1.0f, weights);

	if (1e-4f < weightSum)
	{
		// uniformly distribute the weight over the valid samples
		weights *= rcp(weightSum);

		// tspp
		Texture2D<float4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheDescHeapIdx];
		uint4 histTspp = (uint4) g_prevTemporalCache.GatherAlpha(g_samPointClamp, topLeftTexelUV).wzxy;
		
		// as tspp is an integer, make sure it's at least 1, otherwise tspp would remain at zero forever
		//uint maxTspp = max(max(histTspp.x, histTspp.y), max(histTspp.z, histTspp.w));
		//tspp = min(round(dot(histTspp, weights)), maxTspp + 1);
		histTspp = max(1, histTspp);
		tspp = round(dot(histTspp, weights));
		
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

bool SampleTemporalCache_CatmullRom(uint2 DTid, float3 currPos, float3 currNormal, float linearDepth, float2 currUV, float2 prevUV,
	inout float tspp, out float3 color)
{
	const float2 screenDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	
	float2 samplePos = prevUV * screenDim;
	float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
	float2 f = samplePos - texPos1;
	float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
	float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
	float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
	float2 w3 = f * f * (-0.5f + 0.5f * f);

	float2 w12 = w1 + w2;
	float2 offset12 = w2 / (w1 + w2);

	float2 texPos0 = texPos1 - 1;
	float2 texPos3 = texPos1 + 2;
	float2 texPos12 = texPos1 + offset12;

	texPos0 /= screenDim;
	texPos3 /= screenDim;
	texPos12 /= screenDim;

	float2 prevUVs[5];
	prevUVs[0] = float2(texPos12.x, texPos0.y);
	prevUVs[1] = float2(texPos0.x, texPos12.y);
	prevUVs[2] = float2(texPos12.x, texPos12.y);
	prevUVs[3] = float2(texPos3.x, texPos12.y);
	prevUVs[4] = float2(texPos12.x, texPos3.y);

	// previous frame's depth
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float prevDepths[5];
		
	[unroll]
	for (int i = 0; i < 5; i++)
	{
		float prevDepth = g_prevDepth.SampleLevel(g_samLinearClamp, prevUVs[i], 0.0f);
		prevDepths[i] = Math::Transform::LinearDepthFromNDC(prevDepth, g_frame.CameraNear);
	}
	
	float weights[5];
	
	[unroll]
	for (int j = 0; j < 5; j++)
	{
		float isInBounds = all(prevUVs[j] <= 1.0.xx) && all(prevUVs[j] >= 0.0f);
		float geoWeight = GeometricTest(prevDepths[j], prevUVs[j], currNormal, currPos, linearDepth);
		weights[j] = isInBounds * geoWeight;
	}

	bool allValid = weights[0] > 1e-3f;
	
	[unroll]
	for (int k = 1; k < 5; k++)
		allValid = allValid && (weights[k] > 1e-3f);
	
	if (allValid)
	{
		Texture2D<float4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheDescHeapIdx];
		
		float4 results[5];
		results[0] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[0], 0.0f) * w12.x * w0.y;

		results[1] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[1], 0.0f) * w0.x * w12.y;
		results[2] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[2], 0.0f) * w12.x * w12.y;
		results[3] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[3], 0.0f) * w3.x * w12.y;

		results[4] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[4], 0.0f) * w12.x * w3.y;

		tspp = results[0].w * weights[0] +
			results[1].w * weights[1] +
			results[2].w * weights[2] +
			results[3].w * weights[3] +
			results[4].w * weights[4];
		
		tspp = max(1, tspp);
		color = results[0].rgb + results[1].rgb + results[2].rgb + results[3].rgb + results[4].rgb;
		
		return true;
	}
	
	return false;
}

void Integrate(uint2 DTid, float3 pos, float3 normal, inout uint tspp, inout float3 color)
{
	DiffuseReservoir r = RGI_Diff_Util::PartialReadInputReservoir(DTid, g_local.InputReservoir_A_DescHeapIdx,
				g_local.InputReservoir_B_DescHeapIdx);

	const float3 wi = normalize(r.SamplePos - pos);
	
	const float3 noisySignal = r.Li * r.GetW() * saturate(dot(wi, normal));

	// TODO come up with a better way to incorporate reservoir data into denoiser -- the following
	// approach prevents convergence for more complicated geometry
#if 0
	// TODO improve, temporal lag is still noticeable
	float tsppAdjustment = saturate(r.M / MAX_TEMPORAL_M);
	tsppAdjustment *= tsppAdjustment;
	tspp = round(float(tspp) * tsppAdjustment);
#endif
	
	// use linear weights rather than exponential weights, which comparatively give a higher weight 
	// to initial samples right after disocclusion
	const float accumulationSpeed = 1.0f / (1.0f + tspp);

	// accumulate
	color = lerp(color, noisySignal, accumulationSpeed);

	// don't accumulate more than MaxTspp temporal samples (temporal lag vs noise tradeoff)
	tspp = min(tspp + 1, g_local.MaxTspp);
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(DiffuseDNSR_TEMPORAL_THREAD_GROUP_SIZE_X, DiffuseDNSR_TEMPORAL_THREAD_GROUP_SIZE_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	if (!Math::IsWithinBoundsExc(DTid.xy, renderDim))
		return;
	
	float tspp = 0;
	float3 color = 0.0f.xxx;

	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_currDepth[DTid.xy];

	// skip sky pixels
	if (depth == 0.0)
		return;

	// skip metallic surfaces
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	float metalness = g_metalnessRoughness[DTid.xy].x;
	if (metalness >= MIN_METALNESS_METAL)
		return;
	
	// current frame's normals
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 currNormal = Math::Encoding::DecodeUnitNormal(g_currNormal[DTid.xy].xy);

	// current frame's depth
	const float currLinearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	const float2 currUV = (DTid.xy + 0.5f) / renderDim;

	const float3 currPos = Math::Transform::WorldPosFromUV(currUV, currLinearDepth, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	// compute pixel position corresponding to current pixel in previous frame (in texture space)
	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
	const float2 motionVec = g_motionVector[DTid.xy];
	const float2 prevUV = currUV - motionVec;
	const bool motionVecValid = all(prevUV >= 0.0f) && all(prevUV <= 1.0f);

	if (g_local.IsTemporalCacheValid && motionVecValid)
	{
		// try to use Catmull-Rom filtering first
		bool success = SampleTemporalCache_CatmullRom(DTid.xy, currPos, currNormal, currLinearDepth, currUV, prevUV, tspp, color);
		
		// if it failed, then resample history using a bilinear filter with custom weights
		if (!success)
			SampleTemporalCache_Bilinear(DTid.xy, currPos, currNormal, currLinearDepth, currUV, prevUV, tspp, color);
	}

	// integrate history and current frame
	Integrate(DTid.xy, currPos, currNormal, tspp, color);

	RWTexture2D<float4> g_nextTemporalCache = ResourceDescriptorHeap[g_local.CurrTemporalCacheDescHeapIdx];
	g_nextTemporalCache[DTid.xy] = float4(color, tspp);
}