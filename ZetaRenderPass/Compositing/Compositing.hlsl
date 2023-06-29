#include "Compositing_Common.h"
#include "../Common/BRDF.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../../ZetaCore/Core/Material.h"
#include "../Common/RT.hlsli"
#include "../Common/VolumetricLighting.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbCompositing> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Helper Functions
//--------------------------------------------------------------------------------------

float3 SunDirectLighting(uint2 DTid, float3 baseColor, float metalness, float3 posW, float3 normal,
	inout BRDF::SurfaceInteraction surface)
{
#if 0
	Texture2D<uint> g_sunShadowMask = ResourceDescriptorHeap[g_local.SunShadowDescHeapIdx];
	
	uint groupX = DTid.x >= 8 ? DTid.x >> 3 : 0;
	uint groupY = DTid.y >= 4 ? DTid.y >> 2 : 0;
	uint laneIdx = (DTid.y & (4 - 1)) * 8 + (DTid.x & (8 - 1));
	
	uint groupMask = g_sunShadowMask[uint2(groupX, groupY)];
	uint isUnoccluded = groupMask & (1u << laneIdx);

	if (!isUnoccluded)
		return 0.0.xxx;
#endif	

	Texture2D<half2> g_sunShadowTemporalCache = ResourceDescriptorHeap[g_local.SunShadowDescHeapIdx];
	float shadowVal = g_sunShadowTemporalCache[DTid.xy].x;
		
	surface.InitComplete(-g_frame.SunDir, baseColor, metalness, normal);
	float3 f = BRDF::ComputeSurfaceBRDF(surface);
	
	const float3 sigma_t_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
	const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
	const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;

	posW.y += g_frame.PlanetRadius;
	
	float t = Volumetric::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, posW, -g_frame.SunDir);
	float3 tr = Volumetric::EstimateTransmittance(g_frame.PlanetRadius, posW, -g_frame.SunDir, t,
		sigma_t_rayleigh, sigma_t_mie, sigma_t_ozone, 8);

	float3 L_i = (tr * f) * g_frame.SunIlluminance;
	L_i *= shadowVal;

	return L_i;
}

float GeometryTest(float sampleLinearDepth, float2 sampleUV, float3 centerNormal, float3 centerPos, float centerLinearDepth)
{
	float3 samplePos = Math::Transform::WorldPosFromUV(sampleUV,
		sampleLinearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	float planeDist = dot(centerNormal, samplePos - centerPos);
	float weight = abs(planeDist) <= 0.01 * centerLinearDepth;
	
	return weight;
}

// Ref: P. Kozlowski and T. Cheblokov, "ReLAX: A Denoiser Tailored to Work with the ReSTIR Algorithm," GTC, 2021.
float3 FilterFirefly(Texture2D<float4> g_input, float3 currColor, int2 DTid, int2 GTid, float linearDepth, float3 normal, float3 posW)
{
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];

	float minLum = FLT_MAX;
	float maxLum = 0.0;
	float3 minColor = currColor;
	float3 maxColor = currColor;
	float currLum = Math::Color::LuminanceFromLinearRGB(currColor);
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 rcpRenderDim = 1.0f / float2(g_frame.RenderWidth, g_frame.RenderHeight);
	
	[unroll]
	for (int i = -1; i <= 1; i++)
	{
		[unroll]
		for (int j = -1; j <= 1; j++)
		{
			if (i == 0 && j == 0)
				continue;
			
			int2 addr = int2(DTid.x + j, DTid.y + i);
			if (any(addr) < 0 || any(addr >= renderDim))
				continue;
			
			const float neighborLinearDepth = Math::Transform::LinearDepthFromNDC(g_depth[addr], g_frame.CameraNear);
			if (neighborLinearDepth == FLT_MAX)
				continue;
			
			float2 neighborUV = (addr + 0.5) * rcpRenderDim;
			if (!GeometryTest(neighborLinearDepth, neighborUV, normal, posW, linearDepth))
				continue;
			
			float3 neighborColor = g_input[addr].rgb;
			float neighborLum = Math::Color::LuminanceFromLinearRGB(neighborColor);

			if (neighborLum < minLum)
			{
				minLum = neighborLum;
				minColor = neighborColor;
			}
			else if (neighborLum > maxLum)
			{
				maxLum = neighborLum;
				maxColor = neighborColor;
			}
		}
	}
	
	float3 ret = currLum < minLum ? minColor : (currLum > maxLum ? maxColor : currColor);
	return ret;
}

float CoC(float linearDepth)
{
	float f = g_local.FocalLength / 1000.0f; // convert from mm to meters
	float numerator = f * f * abs(linearDepth - g_local.FocusDepth);
	float denom = g_local.FStop * linearDepth * (g_local.FocusDepth - f);
	return abs(numerator / denom) * 1000;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(COMPOSITING_THREAD_GROUP_DIM_X, COMPOSITING_THREAD_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint Gidx : SV_GroupIndex)
{
	if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
		return;

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	
	if (depth == 0.0)
		return;

	float3 color = 0.0.xxx;
	
	const float3 posW = Math::Transform::WorldPosFromScreenSpace(DTid.xy,
		uint2(g_frame.RenderWidth, g_frame.RenderHeight),
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	const float3 wo = normalize(g_frame.CameraPos - posW);
	
	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	float3 baseColor = g_baseColor[DTid.xy].rgb;

	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_normal[DTid.xy]);

	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float2 mr = g_metalnessRoughness[DTid.xy];

	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::InitPartial(normal, mr.y, wo);

	GBUFFER_EMISSIVE_COLOR g_emissiveColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::EMISSIVE_COLOR];
	float3 L_e = g_emissiveColor[DTid.xy].rgb;
	color += L_e;

	if (g_local.SunLighting)
		color += SunDirectLighting(DTid.xy, baseColor, mr.x, posW, normal, surface);

	if (g_local.SkyLighting)
	{
		Texture2D<float4> g_directDenoised = ResourceDescriptorHeap[g_local.DirectDNSRCacheDescHeapIdx];
		float3 skyLo = g_directDenoised[DTid.xy].rgb;
		
		if (g_local.FireflySuppression)
			skyLo = FilterFirefly(g_directDenoised, skyLo, DTid.xy, GTid.xy, linearDepth, normal, posW);
		
		color += skyLo;
	}

	const float3 diffuseReflectance = baseColor * ONE_OVER_PI;
	const float diffuseReflectanceLum = Math::Color::LuminanceFromLinearRGB(diffuseReflectance);
	
	const bool includeIndDiff = g_local.DiffuseIndirect && mr.x < MIN_METALNESS_METAL;
	const bool includeIndSpec = g_local.SpecularIndirect && mr.y < g_local.RoughnessCutoff;
	
	if (includeIndDiff)
	{
		Texture2D<float4> g_diffuseTemporalCache = ResourceDescriptorHeap[g_local.DiffuseDNSRCacheDescHeapIdx];
		float3 L_indDiff = g_diffuseTemporalCache[DTid.xy].rgb;
		L_indDiff *= diffuseReflectance;
		
		if (includeIndSpec)
			L_indDiff *= 0.5f;
		
		color += L_indDiff;
	}
	
	if (includeIndSpec)
	{
		Texture2D<float4> g_specularTemporalCache = ResourceDescriptorHeap[g_local.SpecularDNSRCacheDescHeapIdx];
		float3 L_indSpec = g_specularTemporalCache[DTid.xy].rgb;

		// check against diffuse can lead to discontinuities, but diffuse is 
		// a more low-frequency signal, so sudden changes should be less common
		if (includeIndDiff && diffuseReflectanceLum > 5e-4)
			L_indSpec *= 0.5;
		
		color += L_indSpec;
	}
	
	if (g_local.AccumulateInscattering)
	{
		if (linearDepth > 1e-4f)
		{
			float2 posTS = (DTid.xy + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);
			float p = pow(max(linearDepth - g_local.VoxelGridNearZ, 0.0f) / (g_local.VoxelGridFarZ - g_local.VoxelGridNearZ), 1.0f / g_local.DepthMappingExp);
		
			//float p = linearDepth / g_local.VoxelGridDepth;
			//p /= exp(-1.0 + p);
		
			Texture3D<half4> g_voxelGrid = ResourceDescriptorHeap[g_local.InscatteringDescHeapIdx];
			float3 posCube = float3(posTS, p);
			//float3 uvw = float3(posCube.xy, p);
			//const float3 uvw = float3(posTS, 12 / 32.0);
		
			half3 inscattering = g_voxelGrid.SampleLevel(g_samLinearClamp, posCube, 0.0f).rgb;
			color += inscattering;
		}
	}

	const float coc = CoC(linearDepth);
	
	RWTexture2D<float4> g_hdrLightAccum = ResourceDescriptorHeap[g_local.CompositedUAVDescHeapIdx];
	g_hdrLightAccum[DTid.xy] = float4(color, coc);
}