// Refs:
// 1. L. Yang, S. Liu, and M. Salvi, "A Survey of Temporal Antialiasing Techniques," Computer Graphics Forum, 2020.
// 2. https://github.com/TheRealMJP/MSAAFilter
// 3. https://alextardif.com/TAA.html
// 4. https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/

#include "TAA_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/Common.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/FrameConstants.h"

#define DEPTH_DILATION 1
#define CATMULL_ROM_FILTERING 1

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbTAA> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

// Ref: M. Pharr, W. Jakob, and G. Humphreys, Physically Based Rendering, Morgan Kaufmann, 2016.
float Mitchell1D(in float x, in float B, in float C)
{
	x = abs(2.0f * x);
	const float oneDivSix = 1.0f / 6.0f;
	
	if (x > 1)
	{
		return ((-B - 6.0f * C) * x * x * x + (6.0f * B + 30.0f * C) * x * x +
				(-12.0f * B - 48.0f * C) * x + (8.0f * B + 24.0f * C)) * oneDivSix;
	}
	else
	{
		return ((12.0f - 9.0f * B - 6.0f * C) * x * x * x +
                (-18.0f + 12.0f * B + 6.0f * C) * x * x +
                (6.0f - 2.0f * B)) * oneDivSix;
	}
}

// Ref: https://github.com/playdeadgames/temporal
float3 ClipAABB(float3 aabbMin, float3 aabbMax, float3 histSample)
{
	float3 center = 0.5f * (aabbMax + aabbMin);
	float3 extents = 0.5f * (aabbMax - aabbMin);

	float3 rayToCenter = histSample - center;
	float3 rayToCenterUnit = rayToCenter.xyz / extents;
	rayToCenterUnit = abs(rayToCenterUnit);
	float rayToCenterUnitMax = max(rayToCenterUnit.x, max(rayToCenterUnit.y, rayToCenterUnit.z));

	if (rayToCenterUnitMax > 1.0)
		return center + rayToCenter / rayToCenterUnitMax;
	else
		return histSample; // point inside aabb
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(TAA_THREAD_GROUP_SIZE_X, TAA_THREAD_GROUP_SIZE_Y, TAA_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID)
{
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	if (!Math::IsWithinBoundsExc(DTid.xy, renderDim))
		return;
	
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];

	RWTexture2D<half4> g_antiAliased = ResourceDescriptorHeap[g_local.CurrOutputDescHeapIdx];
	Texture2D<half4> g_currSignal = ResourceDescriptorHeap[g_local.InputDescHeapIdx];
	
	const half3 currColor = g_currSignal[DTid.xy].rgb;
		
	if (!g_local.TemporalIsValid || depth == 0.0)
	{
		g_antiAliased[DTid.xy] = half4(currColor, 1.0);
		return;
	}
	
	float weightSum = Mitchell1D(0, 0.33f, 0.33f) * Mitchell1D(0, 0.33f, 0.33f);
	float3 reconstructed = currColor * weightSum;
	float3 firstMoment = currColor;
	float3 secondMoment = currColor * currColor;

#if DEPTH_DILATION	
	float closestDepth = depth;
	int2 closestDepthAddress = 0.0.xx;
#endif
	
	// compute neighborhood's AABB
	int numNeighbors = 1;
	
	[unroll]
	for (int i = -1; i < 2; i++)
	{
		for (int j = -1; j < 2; j++)
		{
			if(i == 0 && j == 0)
				continue;
			
			int2 neighborAddrr = DTid.xy + int2(i, j);
			if (!Math::IsWithinBoundsExc(neighborAddrr, int2(g_frame.RenderWidth, g_frame.RenderHeight)))
				continue;
			
			float3 neighborColor = max(g_currSignal[neighborAddrr].rgb, 0.0.xxx);
			
			float weight = Mitchell1D(i, 0.33f, 0.33f) * Mitchell1D(j, 0.33f, 0.33f);
			weight *= 1.0 / (1.0 + Math::Color::LuminanceFromLinearRGB(neighborColor));
			
			reconstructed += neighborColor * weight;
			weightSum += weight;
			
			firstMoment += neighborColor;
			secondMoment += neighborColor * neighborColor;
			
			// motion vector signal might be aliased -- prefilter it by selecting the motion vector
			// of the neighborhood pixel that is closest to the camera.
		#if DEPTH_DILATION
			float neighborDepth = g_depth[neighborAddrr];
			if (neighborDepth > closestDepth)
			{
				closestDepth = neighborDepth;
				closestDepthAddress = int2(i, j);
			}
		#endif
			
			numNeighbors += 1;
		}
	}
	
	reconstructed /= max(weightSum, 1e-5);
	
	// sample history using motion vector
	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];

#if DEPTH_DILATION
	const half2 motionVec = g_motionVector[DTid.xy + closestDepthAddress];
#else
	const float2 motionVec = g_motionVector[DTid.xy];
#endif
	
	// motion vector is relative to texture space
	const float2 currUV = (DTid.xy + 0.5f) / renderDim;
	const float2 prevUV = currUV - motionVec;
	
	// no history sample was available
	if (!Math::IsWithinBoundsInc(prevUV, 1.0f.xx))
	{
		g_antiAliased[DTid.xy] = half4(reconstructed, 0.0h);
		return;
	}

	Texture2D<half4> g_prevColor = ResourceDescriptorHeap[g_local.PrevOutputDescHeapIdx];
	
#if CATMULL_ROM_FILTERING
	float3 history = Common::SampleTextureCatmullRom_5Tap(g_prevColor, g_samLinearClamp, prevUV, renderDim);
#else
	float3 history = g_prevColor.SampleLevel(g_samLinearClamp, prevUV, 0).xyz;
#endif

	// clip history sample towards neighborhood AABB's center
	const float3 mean = firstMoment / numNeighbors;
	float3 std = abs(secondMoment - (firstMoment * firstMoment) / numNeighbors);
	// apply Bessel's correction to get an unbiased sample variance
	std /= (numNeighbors - 1.0f);
	std = sqrt(std);
	
	// form a confidene interval for the distrubution of color around the current pixel
	const float3 clippedHistory = ClipAABB(mean - std, mean + std, history);
	
	// inverse-luminance filtering
	const float currWeight = saturate(g_local.BlendWeight * rcp(1.0f + Math::Color::LuminanceFromLinearRGB(reconstructed)));
	const float histWeight = saturate((1.0f - g_local.BlendWeight) * rcp(1.0f + Math::Color::LuminanceFromLinearRGB(clippedHistory)));
	
//	float3 result = (currWeight * reconstructed + (1.0f - g_local.BlendWeight) * clippedPrevSample) / max(currWeight + prevWeight, 1e-4);
	float3 result = (currWeight * reconstructed + histWeight * clippedHistory) / (currWeight + histWeight);
//	float3 result = g_local.BlendWeight * reconstructed + (1.0f - g_local.BlendWeight) * clippedPrevSample;
	
	// TODO on rare occasions, result can be NaN. figure out what's causing it
	// temporay solution in the meantime -- NaN propagation is avoided at least
	result = isnan(result.x) ? reconstructed : result;
	
	g_antiAliased[DTid.xy] = half4(result, 1);
}