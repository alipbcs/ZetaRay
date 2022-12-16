#include "FinalPass_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/GBuffers.hlsli"
#include "../../ZetaCore/Core/Material.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../IndirectDiffuse/Reservoir.hlsli"

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
	
	Texture2D<half4> g_in = ResourceDescriptorHeap[g_local.InputDescHeapIdx];
	float3 color = g_in.SampleLevel(g_samPointClamp, uv, 0).xyz;
	//float3 color = g_in[psin.PosSS.xy].xyz;
	
	if (g_local.DoTonemapping)
	{
		float avgLum = max(g_avgLum[0], 1e-5f);
		float exposure = ComputeAutoExposureFromAverage(avgLum);
//		float linearExposure = (g_local.KeyValue / avgLum);
		float3 exposedColor = color * exposure;
		float3 toneMapped = Math::Color::LinearTosRGB(ACESFitted(exposedColor) * 1.8f);
//		float3 toneMapped = ToneMapFilmicALU(exposedColor);
		color = toneMapped;
	}
		
	if (g_local.DisplayDepth)
	{
		GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
		float z = g_depth.SampleLevel(g_samPointClamp, uv, 0);
		color = z;
	}
	else if (g_local.DisplayNormal)
	{
		GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
		half2 encodedNormal = g_normal.SampleLevel(g_samPointClamp, uv, 0);
		color = Math::Encoding::DecodeUnitNormalFromHalf2(encodedNormal.xy);
		color = abs(color);	
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
		color = float3(0.0f, mr);
	}
	else if (g_local.DisplayStadTemporalCache)
	{
		Texture2D<float4> g_temporalCache = ResourceDescriptorHeap[g_local.DenoiserTemporalCacheDescHeapIdx];
		
		float4 integratedVals = g_temporalCache.SampleLevel(g_samPointClamp, uv, 0.0f);
		color = integratedVals.rgb;
		color /= (color + 1.0);
		
		if (g_local.VisualizeOcclusion)
			color = color * 0.1 + float3(1.0 - integratedVals.w / 32.0f, 0, 0);
	}
	else if (g_local.DisplayReSTIR_GI_TemporalReservoir)
	{
		Reservoir r = PartialReadInputReservoir(int2(psin.PosSS.xy), g_local.TemporalReservoir_A_DescHeapIdx,
				g_local.TemporalReservoir_B_DescHeapIdx);

		color = r.Li * r.GetW() * ONE_DIV_PI;
			
		if (g_local.VisualizeOcclusion)
			color = color * 0.1 + float3(r.M <= 1, 0, 0);
	}
	else if (g_local.DisplayReSTIR_GI_SpatialReservoir)
	{
		Reservoir r = PartialReadInputReservoir(int2(psin.PosSS.xy), g_local.SpatialReservoir_A_DescHeapIdx,
				g_local.SpatialReservoir_B_DescHeapIdx);

		color = r.Li * r.GetW() * ONE_DIV_PI;
		
		if(g_local.VisualizeOcclusion)
			color = color * 0.1 + float3(r.M == 1, 0, 0);
	}
	
	return float4(color, 1.0f);
}