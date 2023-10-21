#include "Display_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/Common.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/GBuffers.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"

#define GOLDEN_ANGLE 2.39996323

#define MIN_WAVE_SIZE 16
static const uint MaxNumWaves = (DOF_GATHER_THREAD_GROUP_DIM_X * DOF_GATHER_THREAD_GROUP_DIM_Y) / MIN_WAVE_SIZE;

groupshared float g_maxLum[MaxNumWaves];

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbDoF_Gather> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper Functions
//--------------------------------------------------------------------------------------

// Ref: https://blog.voxagon.se/2018/05/04/bokeh-depth-of-field-in-single-pass.html
float3 depthOfField(float3 color, uint2 DTid, float depth, float coc)
{
	float total = 1.0;
	float radius = g_local.RadiusScale;
	
	Texture2D<float4> g_composited = ResourceDescriptorHeap[g_local.CompositedSrvDescHeapIdx];
	Texture2D<float> g_coc = ResourceDescriptorHeap[g_local.CoCSrvDescHeapIdx];
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];

	float2 pixelSize = float2(1.0f / g_frame.DisplayWidth, 1.0f / g_frame.DisplayHeight);
	float2 uv = (DTid.xy + 0.5) * pixelSize;
	
	for (float ang = 0.0; radius < g_local.MaxBlurRadius * coc; ang += GOLDEN_ANGLE)
	{
		float2 sampleUV = uv + float2(cos(ang), sin(ang)) * radius * pixelSize;
		float3 sampleColor = g_composited.SampleLevel(g_samLinearClamp, sampleUV, 0.0f).rgb;
		float sampleCoC = g_coc.SampleLevel(g_samLinearClamp, sampleUV, 0.0f);

#if 0		
		float sampleDepth = g_depth.SampleLevel(g_samLinearClamp, sampleUV, 0.0f);
		if (sampleDepth < depth)
			coc = clamp(coc, 0.0, coc * g_local.MaxBlurRadius * 2);
#endif
		
		float m = smoothstep(radius - 0.5, radius + 0.5, sampleCoC * g_local.MaxBlurRadius);
		color += lerp(color / total, sampleColor, m);
		total += 1.0;
		radius += g_local.RadiusScale / radius;
	}
	
	return color /= total;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(DOF_GATHER_THREAD_GROUP_DIM_X, DOF_GATHER_THREAD_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex)
{
	if (DTid.x >= g_frame.DisplayWidth || DTid.y >= g_frame.DisplayHeight)
		return;

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];

	Texture2D<float4> g_composited = ResourceDescriptorHeap[g_local.CompositedSrvDescHeapIdx];
	Texture2D<float> g_coc = ResourceDescriptorHeap[g_local.CoCSrvDescHeapIdx];
	const float3 composited = g_composited[DTid.xy].rgb;
	const float coc = g_coc[DTid.xy];
	
	float3 color = depthOfField(composited, DTid.xy, depth, coc);
	const float lum = Math::Color::LuminanceFromLinearRGB(color.rgb);
	const float waveMaxLum = WaveActiveMax(lum);
	const uint numWaves = (DOF_GATHER_THREAD_GROUP_DIM_X * DOF_GATHER_THREAD_GROUP_DIM_Y) / WaveGetLaneCount();
	const uint wave = Gidx / WaveGetLaneCount();
	
	if (WaveIsFirstLane())
		g_maxLum[wave] = waveMaxLum;
	
	GroupMemoryBarrierWithGroupSync();
	
	float groupMaxLum = wave < numWaves ? g_maxLum[wave] : 0.0f;
	groupMaxLum = WaveActiveMax(groupMaxLum);
	
	// TODO replace with average lum over input image
	if (g_local.IsGaussianFilterEnabled && groupMaxLum >= g_local.MinLumToFilter)
		color *= -1;
	
	RWTexture2D<float4> g_gather = ResourceDescriptorHeap[g_local.OutputUavDescHeapIdx];
	g_gather[DTid.xy].rgb = color;
}