#include "../Common/Common.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/LightSourceFuncs.hlsli"
#include "../Common/VolumetricLighting.hlsli"
#include "Sky_Common.h"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbSky> g_local : register(b0, space0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1, space0);

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

#define NON_LINEAR_LATITUDE 1

[numthreads(SKY_VIEW_LUT_THREAD_GROUP_SIZE_X, SKY_VIEW_LUT_THREAD_GROUP_SIZE_Y, SKY_VIEW_LUT_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID)
{
	const uint2 textureDim = uint2(g_local.LutWidth, g_local.LutHeight);
	if (DTid.x >= g_local.LutWidth || DTid.y >= g_local.LutHeight)
		return;
	
	// reverse the direction to uv conversion in sky dome
	float phi = ((float) DTid.x / (float) g_local.LutWidth);
	phi *= TWO_PI;
	phi -= PI;
	
	float v = ((float) DTid.y / (float) g_local.LutHeight);

#if NON_LINEAR_LATITUDE == 1
	// horizon (theta ~ PI / 2) has higher frequency data comparitively. Subsequently, low sampleing
	// rate used for generating this LUT (128 for latitude) can't capture those features, which leads 
	// to arifacts. To compensate, apply a non-linear transformations that maps more texels to be  
	// around the horizon. The reverse mapping has to be applied when this LUT is sampled back.
	// Ref: Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"
	float s = v >= 0.5f ? 1.0f : -1.0f;
	float a = v - 0.5f;
	float theta = a * a * TWO_PI * s + PI_DIV_2;
#else
	float theta = v * PI;
#endif

	// x = sin(theta) * cos(phi)
	// y = cos(theta)
	// z = sin(theta) * sin(phi)
	float3 w = float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
	
	const float3 sigma_s_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
	const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
	const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;
	
	// place the camera slighly above the ground to avoid artifacts
	float3 rayOrigin = float3(0.0f, g_frame.PlanetRadius + 0.2f, 0.0f);

	// in-scattered lighting
	float3 Ls = Volumetric::EstimateLs(g_frame.PlanetRadius, rayOrigin, w, g_frame.SunDir, g_frame.AtmosphereAltitude, g_frame.g,
			sigma_s_rayleigh, g_frame.MieSigmaS, sigma_t_mie, sigma_t_ozone, 32);
	Ls *= g_frame.SunIlluminance;
	
	RWTexture2D<float4> g_out = ResourceDescriptorHeap[g_local.LutDescHeapIdx];
	
	// R11G11B10 doesn't have a sign bit
	g_out[DTid.xy].xyz = max(0.0f, Ls);
}
