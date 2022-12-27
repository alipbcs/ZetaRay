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

// Ref: https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
float3 ACESFilm(float3 x)
{
	float a = 2.51f;
	float b = 0.03f;
	float c = 2.43f;
	float d = 0.59f;
	float e = 0.14f;
	return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Ref: https://github.com/EmbarkStudios/kajiya/blob/main/crates/lib/rust-shaders/src/tonemap.rs
float TonemapCurve(float v)
{
	return 1.0 - exp(-v);
}

float3 TonemapCurve(float3 v)
{
	return float3(TonemapCurve(v.r), TonemapCurve(v.g), TonemapCurve(v.b));
}

float3 NeutralTonemap(float3 linearColor)
{
	float3 ycbcr = Math::Color::LinearToYCbCr(linearColor);

	float bt = TonemapCurve(length(ycbcr.yz) * 2.4);
	float desat = max((bt - 0.7) * 0.8, 0.0);
	desat *= desat;

	float3 desat_col = lerp(linearColor, ycbcr.xxx, desat);

	float tm_luma = TonemapCurve(ycbcr.x);
	float3 tm0 = linearColor * max(0.0, tm_luma / max(1e-5, Math::Color::LuminanceFromLinearRGB(linearColor)));
	float final_mult = 0.97;
	float3 tm1 = TonemapCurve(desat_col);

	return lerp(tm0, tm1, bt * bt) * final_mult;
}

// Ref: https://gist.github.com/da-molchanov/85b95ab3f20fa4eb5624f9b3b959a0ee
// Copyright 2021 Dmitry Molchanov and Julia Molchanova
// This code is licensed under the MIT license
float3 UE4Filmic(float3 linearColor)
{
	// This multiplier corresponds to "ExposureCompensation=1" and disabled auto exposure
	static const float ExposureMultiplier = 1.4;

	static const float3x3 PRE_TONEMAPPING_TRANSFORM =
	{
		0.575961650, 0.344143820, 0.079952030,
		0.070806820, 0.827392350, 0.101774690,
		0.028035252, 0.131523770, 0.840242300
	};
	static const float3x3 EXPOSED_PRE_TONEMAPPING_TRANSFORM = ExposureMultiplier * PRE_TONEMAPPING_TRANSFORM;
	static const float3x3 POST_TONEMAPPING_TRANSFORM =
	{
		1.666954300, -0.601741150, -0.065202855,
		-0.106835220, 1.237778600, -0.130948950,
		-0.004142626, -0.087411870, 1.091555000
	};

	// Transform color spaces, perform blue correction and pre desaturation
	float3 WorkingColor = mul(EXPOSED_PRE_TONEMAPPING_TRANSFORM, linearColor);

	// Apply tonemapping curve
	// Narkowicz 2016, "ACES Filmic Tone Mapping Curve"
	// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
	static const float a = 2.51;
	static const float b = 0.03;
	static const float c = 2.43;
	static const float d = 0.59;
	static const float e = 0.14;
	WorkingColor = saturate((WorkingColor * (a * WorkingColor + b)) / (WorkingColor * (c * WorkingColor + d) + e));

	// Transform color spaces, apply blue correction and post desaturation
	return mul(POST_TONEMAPPING_TRANSFORM, WorkingColor);
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
	
	Texture2D<float2> g_exposure = ResourceDescriptorHeap[g_local.ExposureDescHeapIdx];
	float exposure = g_exposure[int2(0, 0)].x;		
	float3 exposedColor = color * exposure;
	
	if (g_local.Tonemapper == (int) Tonemapper::ACES_FILMIC)
		color = ACESFilm(exposedColor);
	else if (g_local.Tonemapper == (int) Tonemapper::UE4_FILMIC)
		color = UE4Filmic(exposedColor);
	else if (g_local.Tonemapper == (int) Tonemapper::NEUTRAL)
		color = NeutralTonemap(exposedColor);
		
	if (g_local.DisplayOption == (int) DisplayOption::DEPTH)
	{
		GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
		float z = g_depth.SampleLevel(g_samPointClamp, uv, 0);
		color = z;
	}
	else if (g_local.DisplayOption == (int) DisplayOption::NORMAL)
	{
		GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
		half2 encodedNormal = g_normal.SampleLevel(g_samPointClamp, uv, 0);
		color = Math::Encoding::DecodeUnitNormal(encodedNormal.xy);
		color = abs(color);	
	}
	else if (g_local.DisplayOption == (int) DisplayOption::BASE_COLOR)
	{
		GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
			GBUFFER_OFFSET::BASE_COLOR];
		color = g_baseColor.SampleLevel(g_samPointClamp, uv, 0).xyz;
	}
	else if (g_local.DisplayOption == (int) DisplayOption::METALNESS_ROUGHNESS)
	{
		GBUFFER_METALNESS_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
			GBUFFER_OFFSET::METALNESS_ROUGHNESS];
		half2 mr = g_metallicRoughness.SampleLevel(g_samPointClamp, uv, 0);
		color = float3(0.0f, mr);
	}
	else if (g_local.DisplayOption == (int) DisplayOption::STAD_TEMPORAL_CACHE)
	{
		Texture2D<float4> g_temporalCache = ResourceDescriptorHeap[g_local.DenoiserTemporalCacheDescHeapIdx];	
		float4 integratedVals = g_temporalCache.SampleLevel(g_samPointClamp, uv, 0.0f);
		color = integratedVals.rgb;
		color /= (color + 1.0);
		
		if (g_local.VisualizeOcclusion)
			color = color * 0.1 + float3(1.0 - integratedVals.w / 32.0f, 0, 0);
	}
	else if (g_local.DisplayOption == (int) DisplayOption::ReSTIR_GI_TEMPORAL_RESERVOIR)
	{
		Reservoir r = PartialReadInputReservoir(int2(psin.PosSS.xy), g_local.TemporalReservoir_A_DescHeapIdx,
				g_local.TemporalReservoir_B_DescHeapIdx);

		color = r.Li * r.GetW() * ONE_DIV_PI;
			
		if (g_local.VisualizeOcclusion)
			color = color * 0.1 + float3(r.M <= 1, 0, 0);
	}
	else if (g_local.DisplayOption == (int) DisplayOption::ReSTIR_GI_SPATIAL_RESERVOIR)
	{
		Reservoir r = PartialReadInputReservoir(int2(psin.PosSS.xy), g_local.SpatialReservoir_A_DescHeapIdx,
				g_local.SpatialReservoir_B_DescHeapIdx);

		color = r.Li * r.GetW() * ONE_DIV_PI;
		
		if(g_local.VisualizeOcclusion)
			color = color * 0.1 + float3(r.M == 1, 0, 0);
	}
	
	return float4(color, 1.0f);
}