#include "Compositing_Common.h"
#include "../Common/BRDF.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/Material.h"
#include "../Common/RT.hlsli"
#include "../IndirectDiffuse/Reservoir.hlsli"
#include "../Common/VolumetricLighting.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbCompositing> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------

float3 SunDirectLighting(uint2 DTid, float3 baseColor, float3 normal, float2 mr, float3 posW, float3 wo)
{
	Texture2D<uint> g_sunShadowMask = ResourceDescriptorHeap[g_local.SunShadowDescHeapIdx];
	
	uint groupX = DTid.x >= 8 ? DTid.x >> 3 : 0;
	uint groupY = DTid.y >= 4 ? DTid.y >> 2 : 0;
	uint laneIdx = (DTid.y & (4 - 1)) * 8 + (DTid.x & (8 - 1));
	
	uint groupMask = g_sunShadowMask[uint2(groupX, groupY)];
	uint isUnoccluded = groupMask & (1u << laneIdx);
	
	if (!isUnoccluded)
		return 0.0.xxx;
	
	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::InitPartial(normal,
		mr.y, mr.x, wo, baseColor.rgb);
	
	surface.InitComplete(-g_frame.SunDir);
	
	float3 f = BRDF::ComputeSurfaceBRDF(surface);

	const float3 sigma_t_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
	const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
	const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;

	posW.y += g_frame.PlanetRadius;
	
	float t = Volumetric::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, posW, -g_frame.SunDir);
	float3 tr = Volumetric::EstimateTransmittance(g_frame.PlanetRadius, posW, -g_frame.SunDir, t,
		sigma_t_rayleigh, sigma_t_mie, sigma_t_ozone, 8);

	float3 L_i = (tr * f) * g_frame.SunIlluminance;

	GBUFFER_EMISSIVE_COLOR g_emissiveColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::EMISSIVE_COLOR];
	half3 L_e = g_emissiveColor[DTid].rgb;
	L_i += L_e;

	return L_i;
}

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint Gidx : SV_GroupIndex)
{
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	if (!Math::IsWithinBoundsExc(DTid.xy, renderDim))
		return;

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];

	if(depth == 0.0)
		return;

	float3 color = 0.0.xxx;
	
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
		
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
	const float3 normal = Math::Encoding::DecodeUnitNormalFromHalf2(g_normal[DTid.xy]);

	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	const half2 mr = g_metallicRoughness[DTid.xy];
	
	if (!g_local.DisplayIndirectDiffuseOnly)
		color += SunDirectLighting(DTid.xy, baseColor, normal, mr, posW, wo);
	
	if(!g_local.DisplayDirectLightingOnly)
	{	
		Reservoir r = PartialReadInputReservoir(DTid.xy, g_local.InputReservoir_A_DescHeapIdx,
				g_local.InputReservoir_B_DescHeapIdx);

		const float3 wi = normalize(r.SamplePos - posW);
		float3 diffuseReflectance = baseColor * (1.0f - mr.x);
	
		// TODO account for Fresnel during denoising
		/*
			float3 F0 = lerp(0.04f.xxx, baseColor, mr.x);
			float3 wh = normalize(wi + wo);
			float whdotwo = saturate(dot(wh, wo)); // == hdotwi
			float3 F = BRDF::FresnelSchlick(F0, whdotwo);
			float3 f = (1.0f.xxx - F) * diffuseReflectance * ONE_DIV_PI;
		*/

		float3 f = diffuseReflectance * ONE_DIV_PI;
		float3 integratedLiXndotwi = r.Li * r.GetW();
	
		if (!g_local.UseRawIndirectDiffuse)
		{
			Texture2D<half4> g_temporalCache = ResourceDescriptorHeap[g_local.DenoiserTemporalCacheDescHeapIdx];
			half3 integratedVals = g_temporalCache[DTid.xy].rgb;
			integratedLiXndotwi = integratedVals;
		}
	
		color += integratedLiXndotwi * f;
		//color = f * 10;
	}
	
	if (g_local.AccumulateInscattering)
	{
		if (linearDepth > 1e-4f)
		{
			float2 posTS = (DTid.xy + 0.5f) / renderDim;
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
	
	RWTexture2D<float4> g_hdrLightAccum = ResourceDescriptorHeap[g_local.HDRLightAccumDescHeapIdx];
	g_hdrLightAccum[DTid.xy] = float4(color, 1.0f);
}
