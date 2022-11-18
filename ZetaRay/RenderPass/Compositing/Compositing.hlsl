#include "Compositing_Common.h"
#include "../Common/BRDF.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/Material.h"
#include "../Common/Sampling.hlsli"
#include "../Common/RT.hlsli"
#include "../IndirectDiffuse/Reservoir.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbCompositing> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint Gidx : SV_GroupIndex)
{
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	if (!Math::IsWithinBoundsExc(DTid.xy, renderDim))
		return;

	RWTexture2D<float4> g_hdrLightAccum = ResourceDescriptorHeap[g_local.HDRLightAccumDescHeapIdx];
	float3 color = g_hdrLightAccum[DTid.xy].rgb;
	
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];

	if(depth == 0.0)
	{
		g_hdrLightAccum[DTid.xy] = float4(color, 1.0f);
		return;
	}
			
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
		
	const float3 posW = Math::Transform::WorldPosFromScreenSpace(DTid.xy,
		uint2(g_frame.RenderWidth, g_frame.RenderHeight),
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	Reservoir r = PartialReadInputReservoir(DTid.xy, g_local.InputReservoir_A_DescHeapIdx,
				g_local.InputReservoir_B_DescHeapIdx);

	const float3 wo = normalize(g_frame.CameraPos - posW);
	const float3 wi = normalize(r.SamplePos - posW);
	
	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	float3 baseColor = g_baseColor[DTid.xy].rgb;

	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormalFromHalf2(g_normal[DTid.xy]);

	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	const half2 mr = g_metallicRoughness[DTid.xy];
	
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
	
	if (g_local.StadDenoiser)
	{
		Texture2D<half4> g_temporalCache = ResourceDescriptorHeap[g_local.DenoiserTemporalCacheDescHeapIdx];
		half3 integratedVals = g_temporalCache[DTid.xy].rgb;
		integratedLiXndotwi = integratedVals;
	}
	
	color += integratedLiXndotwi * f;
	//color = f * 10;
	
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
	
	g_hdrLightAccum[DTid.xy] = float4(color, 1.0f);
}
