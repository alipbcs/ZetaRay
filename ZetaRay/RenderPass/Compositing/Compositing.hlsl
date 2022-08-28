#include "Compositing_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/Material.h"
#include "../Common/Sampler.hlsli"
#include "../Common/RT.hlsli"

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
	const uint2 textureDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	if (!IsInRange(DTid.xy, textureDim))
		return;

	RWTexture2D<float4> g_hdrLightAccum = ResourceDescriptorHeap[g_local.HDRLightAccumDescHeapIdx];
	float3 color = g_hdrLightAccum[DTid.xy].rgb;
	
	// Radiance reflected back to eye (w_o) due to one-bounce indirect lighting (L_ind) is:
	//		L_ind = int_w L_i (p, w_i) * n.w_i * BRDF(p, w_i, w_o) dw
	//		where,
	//			P is the surface point
	//			L_i is radiance that's been scattered once before reaching P having started from some light source
	//
	// MC_estimate(L_ind) = L_ind * n.w_i * BRDF(P, w_i, w_o) / Pdf(w_i)
	// 
	// Assuming a Lambertian surface, cosine-weighted hemisphere sampling can be used to draw sample 
	// directions w_i. For this distribution, PDF(w_i) = n.w_i / Pi 
	//
	// Plugging in above and since Lambertian BRDF is diffuseReflectance / Pi, we have:
	// MC_estimate(L_ind) = L_ind * diffuseReflectance / PI * n.w_i / Pdf(w_i)
	//					  = L_ind * diffuseReflectance

	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	float4 baseColor = g_baseColor[DTid.xy];

	if (baseColor.w < MIN_ALPHA_CUTOFF)
	{
		g_hdrLightAccum[DTid.xy] = float4(color, 1.0f);
		return;
	}
	
	if (g_local.UseDenoised)
	{
		// (hopefully) denoised L_ind
		Texture2D<uint4> g_denoisedLind = ResourceDescriptorHeap[g_local.DenoisedLindDescHeapIdx];
		uint3 integratedVals = g_denoisedLind[DTid.xy].xyz;
		float3 L_i = float3(f16tof32(integratedVals.x >> 16), f16tof32(integratedVals.y), f16tof32(integratedVals.y >> 16));

		// TODO account for Fresnel
		const float3 diffuseReflectance = baseColor.rgb;
		float3 L_o = L_i * diffuseReflectance;
		
		color += L_o;
	}
	
	if (g_local.AccumulateInscattering)
	{
		GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
		float linearDepth = g_depth[DTid.xy];

		if (linearDepth > 1e-4f)
		{
			float2 posTS = (DTid.xy + 0.5f) / textureDim;
			linearDepth = ComputeLinearDepthReverseZ(linearDepth, g_frame.CameraNear);
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
