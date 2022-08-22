#include "Compositing_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/Material.h"
#include "../Common/Sampler.hlsli"
#include "../Common/RT.hlsli"
#include "../Common/SH.hlsli"

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
	// MC_estimate(L_ind) = L_ind * diffuseReflectance * n.w_i / Pdf(w_i) * Pi
	//					  = L_ind * diffuseReflectance

	if (g_local.UseDenoised)
	{
		// (hopefully) denoised L_ind
		GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
			GBUFFER_OFFSET::BASE_COLOR];
		const float3 diffuseReflectance = g_baseColor[DTid.xy].xyz;
		
		/*
		Texture2D<uint4> g_denoisedLind = ResourceDescriptorHeap[g_local.DenoisedLindDescHeapIdx];
		uint2 integratedVals = g_denoisedLind[DTid.xy].xy;
		float3 L_o = float3(f16tof32(integratedVals.x >> 16), f16tof32(integratedVals.y), f16tof32(integratedVals.y >> 16));
	
		const float3 diffuseReflectance = baseColor.rgb;
		float3 L_ind_mc_est = L_o * diffuseReflectance;
		
		color += L_ind_mc_est * g_frame.SunIlluminance;
		*/

		Texture2D<uint4> g_denoisedLind = ResourceDescriptorHeap[g_local.DenoisedLindDescHeapIdx];
		uint3 integratedVals = g_denoisedLind[DTid.xy].xyz;
		
		// plan
		float4 lumaSH = float4(f16tof32(integratedVals.x), f16tof32(integratedVals.x >> 16), 
			f16tof32(integratedVals.y), f16tof32(integratedVals.y >> 16));
		float Co = f16tof32(integratedVals.z);
		float Cg = f16tof32(integratedVals.z >> 16);

		GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
		float3 normal = DecodeUnitNormalFromHalf2(g_normal[DTid.xy]);

		// convolve incoming radiance with max(0, cos(theta))
		// Convolution becomes dot product in SH basis. We have the MC estimate
		// of SH coeffs for incoming radiance, and SH coeffs for max(0, cos(theta)) are 
		// known in closed form
		float irradY = 0.0f;
		irradY += lumaSH.x * COS_THETA_SH_COEFFS[0];
//		irradY += lumaSH.y * normal.y * COS_THETA_SH_COEFFS[1];
//		irradY += lumaSH.z * normal.z * COS_THETA_SH_COEFFS[1];
//		irradY += lumaSH.w * normal.x * COS_THETA_SH_COEFFS[1];
		
		irradY = max(0.0f, irradY);
		color = (half3) YCoCgToRGB(float3(irradY, Co, Cg)) * diffuseReflectance *  g_frame.SunIlluminance;
	}
	
	if(g_local.AccumulateInscattering)
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
			color += inscattering * 1.5f;
		}
	}
	
	g_hdrLightAccum[DTid.xy] = float4(color, 1.0f);
}
