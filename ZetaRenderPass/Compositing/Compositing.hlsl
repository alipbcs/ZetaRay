#include "Compositing_Common.h"
#include "../Common/BRDF.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../../ZetaCore/Core/Material.h"
#include "../Common/RT.hlsli"
#include "../IndirectDiffuse/Reservoir_Diffuse.hlsli"
#include "../IndirectSpecular/Reservoir_Specular.hlsli"
#include "../Common/VolumetricLighting.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbCompositing> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Helper Functions
//--------------------------------------------------------------------------------------

float3 SunDirectLighting(uint2 DTid, float3 baseColor, float3 normal, float2 mr, float3 posW, float3 wo)
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
		
	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::InitPartial(normal, mr.y, wo);	
	surface.InitComplete(-g_frame.SunDir, baseColor.rgb, mr.x);
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

	GBUFFER_EMISSIVE_COLOR g_emissiveColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::EMISSIVE_COLOR];
	float3 L_e = g_emissiveColor[DTid].rgb;
	L_i += L_e;

	return L_i;
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

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint Gidx : SV_GroupIndex)
{
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	if (!Math::IsWithinBoundsExc(DTid.xy, renderDim))
		return;

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];

	if (depth == 0.0)
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
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_normal[DTid.xy]);

	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float2 mr = g_metalnessRoughness[DTid.xy];

	if (g_local.DirectLighting)
		color += SunDirectLighting(DTid.xy, baseColor, normal, mr, posW, wo);
	
	if (g_local.IndirectDiffuse && mr.x <= MAX_METALNESS)
	{
		float3 f = baseColor * ONE_DIV_PI;
	
		Texture2D<half4> g_diffuseTemporalCache = ResourceDescriptorHeap[g_local.DiffuseDNSRCacheDescHeapIdx];
		float3 integratedVals = g_diffuseTemporalCache[DTid.xy].rgb;
		color += integratedVals * f;
		//color = f * 10;
	}
	
	if (g_local.IndirectSpecular && mr.y < g_local.RoughnessCutoff)
	{
		Texture2D<half4> g_specularTemporalCache = ResourceDescriptorHeap[g_local.SpecularDNSRCacheDescHeapIdx];
		float3 integratedVals = g_specularTemporalCache[DTid.xy].rgb;
		color += integratedVals;
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

	const float coc = CoC(linearDepth);
	
	RWTexture2D<float4> g_hdrLightAccum = ResourceDescriptorHeap[g_local.CompositedUAVDescHeapIdx];
	g_hdrLightAccum[DTid.xy] = float4(color, coc);
}