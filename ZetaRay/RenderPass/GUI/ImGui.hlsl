#include "../Common/StaticTextureSamplers.hlsli"
#include "GuiPass_Common.h"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbGuiPass> g_local : register(b0);

//--------------------------------------------------------------------------------------
// Layouts
//--------------------------------------------------------------------------------------

struct VS_INPUT
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD;
    float4 col : COLOR;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR;
    float2 uv  : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

PS_INPUT mainVS(VS_INPUT input)
{
    PS_INPUT output;
	output.pos = mul(g_local.WVP, float4(input.pos.xy, 0.f, 1.f));
	output.col = input.col;
	output.uv = input.uv;
    return output;
}

float4 mainPS(PS_INPUT input) : SV_Target
{
	Texture2D<float4> texture0 = ResourceDescriptorHeap[g_local.FontTex];
	float4 out_col = input.col * texture0.Sample(g_samImgUi, input.uv);
	return out_col;
}