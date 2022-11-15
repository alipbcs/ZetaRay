#include "../Common/Common.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/FrameConstants.h"
#include "TAA_Common.h"
#include "../Common/LightSourceFuncs.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbTAA> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

// Ref: "Physically Based Rendering" 3rd Ed. Chapter 7
float Mitchell1D(in float x, in float B, in float C)
{
    x = abs(2.0f * x);
	const float oneDivSix = 1.0f / 6.0f;
	
    if (x > 1)
		return ((-B - 6.0f * C) * x * x * x + (6.0f * B + 30.0f * C) * x * x +
				(-12.0f * B - 48.0f * C) * x + (8.0f * B + 24.0f * C)) * oneDivSix;
    else
		return ((12.0f - 9.0f * B - 6.0f * C) * x * x * x +
                (-18.0f + 12.0f * B + 6.0f * C) * x * x +
                (6.0f - 2.0f * B)) * oneDivSix;
}

// Ref: "Temporal Reprojection Anti-Aliasing"
// https://github.com/playdeadgames/temporal
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
void main( uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID )
{
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	if (!Common::IsWithinBounds(DTid.xy, renderDim))
		return;
	
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];

	RWTexture2D<half4> g_antiAliased = ResourceDescriptorHeap[g_local.CurrOutputDescHeapIdx];	
	Texture2D<half4> g_currSignal = ResourceDescriptorHeap[g_local.InputDescHeapIdx];
	
	half3 currColor = g_currSignal[DTid.xy].rgb;
		
	if (!g_local.TemporalIsValid || depth == 0.0)
	{
		g_antiAliased[DTid.xy] = half4(currColor, 1.0h);
		return;
	}
	
	float3 reconstructed = float3(0.0f, 0.0f, 0.0f);
	float weightSum = 0.0f;
	float closestDepth = FLT_MAX;
	int2 closestDepthAddress;
	float3 firstMoment = float3(0.0f, 0.0f, 0.0f);
	float3 secondMoment = float3(0.0f, 0.0f, 0.0f);

	// compute neighborhood's AABB
	[loop]
	for (int i = -1; i < 2; i++)
	{
		for (int j = -1; j < 2; j++)
		{
			int2 neighborGTID = int2(GTid.x + 1 + j, GTid.y + 1 + i);
			half3 neighborColor = g_currSignal[DTid.xy + int2(i, j)].rgb;;
			
			// Pixel subsamples are jittered every frame; it's been recommended to reconstruct current 
			// pixel value from the 3x3 neighborhood for better stability (i.e. assumes 3x3 neighborhood are subpixel values).
			// Ref: "High-Quality Temporal Supersampling"
			float weight = Mitchell1D(i, 0.33f, 0.33f) * Mitchell1D(j, 0.33f, 0.33f);
			reconstructed += neighborColor * weight;
			weightSum += weight;
			
			firstMoment += neighborColor;
			secondMoment += neighborColor * neighborColor;
			
			// motion vector signal might be aliased— prefilter it by selecting the motion vector
			// of the neighborhood pixel that is closest to the camera.
			// Ref: "Filmic SMAA Sharp Morphological and Temporal Antialiasing"
//			float neighborDepth = g_depth[DTid.xy + int2(i, j)];
//			if (neighborDepth > closestDepth)
//			{
//				closestDepth = neighborDepth;
//				closestDepthAddress = int2(i, j);
//			}
		}
	}
	
	reconstructed /= weightSum;
	float currLum = Common::LuminanceFromLinearRGB(reconstructed);
	
	// sample history using motion vector
	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
//	const half2 motionVec = g_motionVector[DTid.xy + closestDepthAddress];
	const float2 motionVec = g_motionVector[DTid.xy];
	
	// motion vector is relative to texture space
	float2 currUV = float2(DTid.x + 0.5f, DTid.y + 0.5f) / renderDim;
	float2 prevUV = currUV - motionVec;
	
	// no history sample was available
	if (any(abs(prevUV) - prevUV))
	{
		g_antiAliased[DTid.xy] = half4(reconstructed, 0.0h);
		return;
	}
	
	Texture2D<float4> g_prevColor = ResourceDescriptorHeap[g_local.PrevOutputDescHeapIdx];
	
	float3 prevSample;
	if (g_local.CatmullRomFiltering)
		prevSample = Common::SampleTextureCatmullRom_5Tap(g_prevColor, g_samLinearClamp, prevUV, renderDim);
	else
		prevSample = g_prevColor.SampleLevel(g_samLinearClamp, prevUV, 0).xyz;

	// helps with reducing fireflies
	// Ref: https://graphicrants.blogspot.com/2013/12/tone-mapping.html
	float tonemapWeight = rcp(1.0f + Common::LuminanceFromLinearRGB(prevSample));
	prevSample *= tonemapWeight;

	// clip history sample towards neighborhood AABB's center
	float3 mean = firstMoment / 9;
	// apply Bessel's correction to get an unbiased sample variance
	float3 std = sqrt(abs(secondMoment - firstMoment * firstMoment) / (9 - 1));
	
	// form a confidene-interval for the distrubution of color around the current pixel
	// note that clipping was performed against the tonemapped color, so "g_inPrevColor" must've been tonemapped
	float3 clippedPrevSample = ClipAABB(saturate(mean - std), saturate(mean + std), prevSample);
	
	float currWeight = rcp(1.0f + currLum);
	currWeight *= g_local.BlendWeight;
	float prevWeight = (1.0f - g_local.BlendWeight) * tonemapWeight;

	float3 result = (currWeight * reconstructed + (1.0f - g_local.BlendWeight) * clippedPrevSample) / max(currWeight + prevWeight, 1e-5);
//	float3 result = g_local.BlendWeight * reconstructed + (1.0f - g_local.BlendWeight) * clippedPrevSample;
	
	g_antiAliased[DTid.xy] = half4(half3(result), 1.0h);
}