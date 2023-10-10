#include "Display_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/GBuffers.hlsli"
#include "../../ZetaCore/Core/Material.h"
#include "../Common/StaticTextureSamplers.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbDisplayPass> g_local : register(b1);

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

// Ref: https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl

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

// Ref: https://github.com/h3r2tic/tony-mc-mapface
float3 tony_mc_mapface(float3 stimulus)
{
    // Apply a non-linear transform that the LUT is encoded with.
	const float3 encoded = stimulus / (stimulus + 1.0);

    // Align the encoded range to texel centers.
	const float LUT_DIMS = 48.0;
	const float3 uv = encoded * ((LUT_DIMS - 1.0) / LUT_DIMS) + 0.5 / LUT_DIMS;

    // Note: for OpenGL, do `uv.y = 1.0 - uv.y`
	Texture3D<float3> g_lut = ResourceDescriptorHeap[g_local.LUTDescHeapIdx];
	return g_lut.SampleLevel(g_samLinearClamp, uv, 0);
}

// Ref: https://iolite-engine.com/blog_posts/minimal_agx_implementation
float3 agxDefaultContrastApprox(float3 x) 
{
	float3 x2 = x * x;
	float3 x4 = x2 * x2;
	
	return + 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4
		- 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 agxInset(float3 val) 
{
	const float3x3 agx_mat = float3x3(
		0.842479062253094, 0.0423282422610123, 0.0423756549057051,
		0.0784335999999992,  0.878468636469772,  0.0784336,
		0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    
	const float min_ev = -12.47393f;
	const float max_ev = 4.026069f;

	// Input transform (inset)
	val = mul(agx_mat, val);
  
	// Log2 space encoding
	val = clamp(log2(val), min_ev, max_ev);
	val = (val - min_ev) / (max_ev - min_ev);
  
	// Apply sigmoid function approximation
	val = agxDefaultContrastApprox(val);

	return val;
}

float3 agxEotf(float3 val) {
	const float3x3 agx_mat_inv = float3x3(
		1.19687900512017, -0.0528968517574562, -0.0529716355144438,
		-0.0980208811401368, 1.15190312990417, -0.0980434501171241,
		-0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    
	// Inverse input transform (outset)
	val = mul(agx_mat_inv, val);
  
	// sRGB IEC 61966-2-1 2.2 Exponent Reference EOTF Display
	//val = pow(val, float3(2.2));

	return val;
}

float3 AgX(float3 value)
{
	float3 tonemapped = agxInset(value);
	tonemapped = agxEotf(tonemapped);
	
	return tonemapped;
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
	
	Texture2D<half4> g_composited = ResourceDescriptorHeap[g_local.InputDescHeapIdx];
	//float4 composited = g_composited[psin.PosSS.xy].rgba;
	float3 composited = g_composited.SampleLevel(g_samPointClamp, uv, 0).rgb;	
	float3 display = composited;
	
	if(g_local.AutoExposure)
	{
		Texture2D<float2> g_exposure = ResourceDescriptorHeap[g_local.ExposureDescHeapIdx];
		const float exposure = g_exposure[int2(0, 0)].x;
		const float3 exposedColor = composited * exposure;
		display = exposedColor;
	}
	
	if (g_local.Tonemapper == (int) Tonemapper::ACES_FITTED)
		display = ACESFitted(display);
	else if (g_local.Tonemapper == (int) Tonemapper::NEUTRAL)
		display = tony_mc_mapface(display);
	else if (g_local.Tonemapper == (int) Tonemapper::AgX)
		display = AgX(display);

	float3 desaturation = Math::Color::LuminanceFromLinearRGB(display);
	display = lerp(desaturation, display, g_local.Saturation);
		
	if (g_local.DisplayOption == (int) DisplayOption::DEPTH)
	{
		GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
		float z = g_depth.SampleLevel(g_samPointClamp, uv, 0);
		display = z.xxx;
	}
	else if (g_local.DisplayOption == (int) DisplayOption::NORMAL)
	{
		GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
		float2 encodedNormal = g_normal.SampleLevel(g_samPointClamp, uv, 0);
		display = Math::Encoding::DecodeUnitVector(encodedNormal.xy);
		display = display * 0.5 + 0.5;
	}
	else if (g_local.DisplayOption == (int) DisplayOption::BASE_COLOR)
	{
		GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
			GBUFFER_OFFSET::BASE_COLOR];
		display = g_baseColor.SampleLevel(g_samPointClamp, uv, 0).xyz;
	}
	else if (g_local.DisplayOption == (int) DisplayOption::METALNESS_ROUGHNESS)
	{
		GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
			GBUFFER_OFFSET::METALLIC_ROUGHNESS];
		float2 mr = g_metallicRoughness.SampleLevel(g_samPointClamp, uv, 0);

		bool isMetallic = GBuffer::DecodeMetallic(mr.x);
		mr.x = isMetallic;

		display = float3(mr, 0.0f);
	}
	else if (g_local.DisplayOption == (int) DisplayOption::EMISSIVE)
	{
		GBUFFER_EMISSIVE_COLOR g_emissiveColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
			GBUFFER_OFFSET::EMISSIVE_COLOR];
		GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
			GBUFFER_OFFSET::BASE_COLOR];
		display =  g_baseColor.SampleLevel(g_samPointClamp, uv, 0).xyz * 0.05 + g_emissiveColor.SampleLevel(g_samPointClamp, uv, 0).rgb;
	}
	else if (g_local.DisplayOption == (int) DisplayOption::CURVATURE)
	{
		GBUFFER_CURVATURE g_curvature = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
			GBUFFER_OFFSET::CURVATURE];
		display = g_curvature.SampleLevel(g_samPointClamp, uv, 0).rrr;
	}
//	else if (g_local.DisplayOption == (int) DisplayOption::EXPOSURE_HEATMAP)
//	{
//		float l1 = Math::Color::LuminanceFromLinearRGB(display);
//		float l2 = Math::Color::LuminanceFromLinearRGB(exposedColor);
//		display = HeatmapColor(l2 / max(l1, 1e-6));
//	}
	
	return float4(display, 1.0f);
}