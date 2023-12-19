#include "Compositing_Common.h"
#include "../Common/BRDF.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../../ZetaCore/Core/Material.h"
#include "../Common/RT.hlsli"
#include "../Common/Volumetric.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbCompositing> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Helper Functions
//--------------------------------------------------------------------------------------

float3 SunDirectLighting(uint2 DTid, float3 baseColor, float metallic, float3 posW, float3 normal,
	inout BRDF::SurfaceInteraction surface)
{
	Texture2D<half> g_sunShadowTemporalCache = ResourceDescriptorHeap[g_local.SunShadowDescHeapIdx];
	float shadowVal = g_sunShadowTemporalCache[DTid].x;

	surface.SetWi(-g_frame.SunDir, normal);
	float3 f = BRDF::SurfaceBRDF(surface);
	
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

float3 SkyColor(uint2 DTid)
{
	float3 w = RT::GeneratePinholeCameraRay(DTid, float2(g_frame.RenderWidth, g_frame.RenderHeight), 
		g_frame.AspectRatio, g_frame.TanHalfFOV, g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, g_frame.CurrView[2].xyz);
	
	float3 rayOrigin = float3(0, 1e-1, 0);
	rayOrigin.y += g_frame.PlanetRadius;

	float3 wTemp = w;
	// cos(a - b) = cos a cos b + sin a sin b
	wTemp.y = wTemp.y * g_frame.SunCosAngularRadius + sqrt(1 - w.y * w.y) * g_frame.SunSinAngularRadius;

	float t;
	bool intersectedPlanet = Volumetric::IntersectRayPlanet(g_frame.PlanetRadius, rayOrigin, wTemp, t);

	// a disk that's supposed to be the sun
	if (dot(-w, g_frame.SunDir) >= g_frame.SunCosAngularRadius && !intersectedPlanet)
	{
		return g_frame.SunIlluminance;
	}
	// sample the sky texture
	else
	{
		float2 thetaPhi = Math::SphericalFromCartesian(w);
		
		const float u = thetaPhi.y * ONE_OVER_2_PI;
		float v = thetaPhi.x * ONE_OVER_PI;
		
		float s = thetaPhi.x >= PI_OVER_2 ? 1.0f : -1.0f;
		v = (thetaPhi.x - PI_OVER_2) * 0.5f;
		v = 0.5f + s * sqrt(abs(v) * ONE_OVER_PI);

		Texture2D<half3> g_envMap = ResourceDescriptorHeap[g_frame.EnvMapDescHeapOffset];
		return g_envMap.SampleLevel(g_samLinearClamp, float2(u, v), 0.0f);		
	}
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
	const float linearDepth = g_depth[DTid.xy];

	RWTexture2D<float4> g_hdrLightAccum = ResourceDescriptorHeap[g_local.CompositedUAVDescHeapIdx];
	
	if (linearDepth == FLT_MAX)
	{
		g_hdrLightAccum[DTid.xy].rgb = SkyColor(DTid.xy);
		return;
	}

	float3 color = 0.0.xxx;
	
	const float3 posW = Math::Transform::WorldPosFromScreenSpace(DTid.xy,
		float2(g_frame.RenderWidth, g_frame.RenderHeight),
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv,
		g_frame.CurrCameraJitter);
	
	const float3 wo = normalize(g_frame.CameraPos - posW);
	
	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	float3 baseColor = g_baseColor[DTid.xy].rgb;

	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitVector(g_normal[DTid.xy]);

	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	const float2 mr = g_metallicRoughness[DTid.xy];

	bool isMetallic;
	bool isEmissive;
	GBuffer::DecodeMetallicEmissive(mr.x, isMetallic, isEmissive);
	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::Init(normal, wo, isMetallic, mr.y, baseColor);

	if(!isEmissive)
	{
		if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::SUN_DI))
			color += SunDirectLighting(DTid.xy, baseColor, isMetallic, posW, normal, surface);

		if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::SKY_DI) && g_local.SkyDIDenoisedDescHeapIdx != 0)
		{
			Texture2D<float4> g_directDenoised = ResourceDescriptorHeap[g_local.SkyDIDenoisedDescHeapIdx];
			float3 L_s = g_directDenoised[DTid.xy].rgb;
		
			color += L_s / (g_frame.Accumulate && g_frame.CameraStatic ? g_frame.NumFramesCameraStatic : 1);
		}

		if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::EMISSIVE_DI) && g_local.EmissiveDIDenoisedDescHeapIdx != 0)
		{
			Texture2D<half4> g_emissive = ResourceDescriptorHeap[g_local.EmissiveDIDenoisedDescHeapIdx];
			float3 L_e = g_emissive[DTid.xy].rgb;

			color += L_e / (g_frame.Accumulate && g_frame.CameraStatic ? g_frame.NumFramesCameraStatic : 1);
		}
	}
	else
	{
		GBUFFER_EMISSIVE_COLOR g_emissiveColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
			GBUFFER_OFFSET::EMISSIVE_COLOR];
		float3 L_e = g_emissiveColor[DTid.xy].rgb;
		color = L_e;
	}
	
	if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::INSCATTERING))
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

	//g_hdrLightAccum[DTid.xy].rgb = baseColor;
	g_hdrLightAccum[DTid.xy].rgb = color;
}