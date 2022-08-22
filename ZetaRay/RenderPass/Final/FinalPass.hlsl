#include "FinalPass_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/GBuffers.hlsli"
#include "../Common/Material.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/SH.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbFinalPass> g_local : register(b1);
StructuredBuffer<float> g_avgLum : register(t0);

//--------------------------------------------------------------------------------------
// Helper structs
//--------------------------------------------------------------------------------------

struct VSOut
{
	float4 PosSS : SV_Position;
	float2 TexCoord : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

// ACESFitted() and related functions are from following repo (under MIT license):
// https://github.com/TheRealMJP/BakingLab

// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
static const float3x3 ACESInputMat =
{
	{ 0.59719, 0.35458, 0.04823 },
	{ 0.07600, 0.90834, 0.01566 },
	{ 0.02840, 0.13383, 0.83777 }
};

// ODT_SAT => XYZ => D60_2_D65 => sRGB
static const float3x3 ACESOutputMat =
{
	{ 1.60475, -0.53108, -0.07367 },
	{ -0.10208, 1.10813, -0.00605 },
	{ -0.00327, -0.07276, 1.07602 }
};

float3 RRTAndODTFit(float3 v)
{
	float3 a = v * (v + 0.0245786f) - 0.000090537f;
	float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
	return a / b;
}

float3 ACESFitted(float3 color)
{
	color = mul(ACESInputMat, color);

    // Apply RRT and ODT
	color = RRTAndODTFit(color);

	color = mul(ACESOutputMat, color);

    // Clamp to [0, 1]
	color = saturate(color);

	return color;
}

float ComputeAutoExposureFromAverage(float averageLuminance)
{
//	const float averageLuminance = exp(averageLogLuminance);
	const float S = 100.0f; // ISO arithmetic speed
	const float K = 12.5f;
	const float exposureIso100 = log2((averageLuminance * S) / K);
	const float q = 0.65f;
	const float luminanceMax = (78.0f / (q * S)) * pow(2.0f, exposureIso100);
	return 1 / luminanceMax;
}

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------

VSOut mainVS(uint vertexID : SV_VertexID)
{
	VSOut vsout;

	vsout.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
	vsout.PosSS = float4(vsout.TexCoord.x * 2 - 1, -vsout.TexCoord.y * 2 + 1, 0, 1);

	return vsout;
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------

float4 mainPS(VSOut psin) : SV_Target
{
	const float2 uv = psin.PosSS.xy / float2(g_frame.DisplayWidth, g_frame.DisplayHeight);
	
	// either output of TAA or composited lighting
	Texture2D<half4> g_in = ResourceDescriptorHeap[g_local.InputDescHeapIdx];
	half3 color = g_in.SampleLevel(g_samPointClamp, uv, 0).xyz;
	
	if (g_local.DoTonemapping)
	{
		float avgLum = max(g_avgLum[0], 1e-5f);
		float exposure = ComputeAutoExposureFromAverage(avgLum);
//		float linearExposure = (g_local.KeyValue / avgLum);
		float3 exposedColor = color * exposure;
		float3 toneMapped = LinearTosRGB(ACESFitted(exposedColor) * 1.8f);
//		float3 toneMapped = ToneMapFilmicALU(exposedColor);
		color = (half3) toneMapped;
	}
		
	if (g_local.DisplayDepth)
	{
		GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
		float z = g_depth.SampleLevel(g_samPointClamp, uv, 0);
		color = half(z);
	}
	else if (g_local.DisplayNormals)
	{
		GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
		half2 encodedNormal = g_normal.SampleLevel(g_samPointClamp, uv, 0);
		color = (half3) DecodeUnitNormalFromHalf2(encodedNormal.xy);
		color = abs(color);
		//float3 geometricNormal = DecodeUnitNormalFromHalf2(encodedNormal.zw);		
	}
	else if (g_local.DisplayBaseColor)
	{
		GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
			GBUFFER_OFFSET::BASE_COLOR];
		color = g_baseColor.SampleLevel(g_samPointClamp, uv, 0).xyz;
	}
	else if (g_local.DisplayMetalnessRoughness)
	{
		GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
			GBUFFER_OFFSET::METALLIC_ROUGHNESS];
		half2 mr = g_metallicRoughness.SampleLevel(g_samPointClamp, uv, 0);
		color = half3(0.0f, mr);
	}
	else if (g_local.DisplayMotionVec)
	{
		GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
			GBUFFER_OFFSET::MOTION_VECTOR];
		half2 mv = g_motionVector.SampleLevel(g_samPointClamp, uv, 0);
		color = half3(mv, 0.0f);
	}	
	else if (g_local.DisplayIndirectDiffuse)
	{
		Texture2D<uint4> g_indirectLo = ResourceDescriptorHeap[g_local.IndirectDiffuseLoDescHeapIdx];
		uint4 Lo = g_indirectLo[psin.PosSS.xy].xyzw;
	
		// plan
		float4 lumaSH = float4(f16tof32(Lo.x), f16tof32(Lo.x >> 16), f16tof32(Lo.y), f16tof32(Lo.y >> 16));
		float Co = f16tof32(Lo.z);
		float Cg = f16tof32(Lo.z >> 16);

		GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
		float3 normal = DecodeUnitNormalFromHalf2(g_normal[psin.PosSS.xy]);

		// convolve incoming radiance with max(0, cos(theta))
		// Convolution becomes dot product in SH basis. We have the MC estimate
		// of SH coeffs for incoming radiance, and SH coeffs for max(0, cos(theta)) are 
		// known in closed form
		float irradY = 0.0f;
		irradY += lumaSH.x * COS_THETA_SH_COEFFS[0];
		irradY += lumaSH.y * normal.y * COS_THETA_SH_COEFFS[1];
		irradY += lumaSH.z * normal.z * COS_THETA_SH_COEFFS[1];
		irradY += lumaSH.w * normal.x * COS_THETA_SH_COEFFS[1];
		
		irradY = max(0.0f, irradY);
		
		//float modifier = SHBasis00 * irradY * rcp(lumaSH.x) * rcp(asfloat(Lo.w));
		//float2 CoCg = float2(Co, Cg) * saturate(modifier);
		//color = (half3) YCoCgToRGB(float3(irradY, CoCg));
		color = half3(irradY.xxx);
		
		// orig
		//color = YCoCgToRGB(half3(asfloat16(uint16_t(Lo.x)), asfloat16(uint16_t(Lo.y)), asfloat16(uint16_t(Lo.z))));
		//color = (half3) YCoCgToRGB(float3(f16tof32(Lo.x), f16tof32(Lo.y), f16tof32(Lo.z)));
		//color = half3(f16tof32(Lo.x), f16tof32(Lo.y), f16tof32(Lo.z));
	}	
	else if (g_local.DisplaySvgfSpatialVariance)
	{
		Texture2D<half> g_var = ResourceDescriptorHeap[g_local.SVGFSpatialVarDescHeapIdx];
		half var = g_var.SampleLevel(g_samPointClamp, uv, 0);
		color = var.xxx;
	}
	else if (g_local.DisplaySvgfTemporalCache)
	{
		Texture2D<uint4> g_temporalCache = ResourceDescriptorHeap[g_local.SVGFTemporalCacheDescHeapIdx];
		uint3 temporal = g_temporalCache[psin.PosSS.xy].xyz;
		
		/*
		GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
		float3 normal = DecodeUnitNormalFromHalf2(g_normal[psin.PosSS.xy]);

		float4 lumaSH = float4(f16tof32(temporal.x), f16tof32(temporal.x >> 16), f16tof32(temporal.y), f16tof32(temporal.y >> 16));
		float Co = f16tof32(temporal.z);
		float Cg = f16tof32(temporal.z >> 16);
		
		float irradY = 0.0f;
		irradY += lumaSH.x * COS_THETA_SH_COEFFS[0];
		irradY += lumaSH.y * normal.y * COS_THETA_SH_COEFFS[1];
		irradY += lumaSH.z * normal.z * COS_THETA_SH_COEFFS[1];
		irradY += lumaSH.w * normal.x * COS_THETA_SH_COEFFS[1];
		
		irradY = max(0.0f, irradY);
		
		//color = (half3) YCoCgToRGB(float3(irradY, Co, Cg)) * 1e-6;
		*/
		color = half3(asfloat(temporal.z).xxx);
	}
		
	return float4(color, 1.0f);
}