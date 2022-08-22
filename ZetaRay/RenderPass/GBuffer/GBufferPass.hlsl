#include "../Common/Common.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/Material.h"
#include "GBuffer_Common.h"

//--------------------------------------------------------------------------------------
// Helper structs
//--------------------------------------------------------------------------------------

struct VSIn
{
	float3 PosL : POSITION;
	float3 NormalL : NORMAL;
	float2 TexUV : TEXUV;
	float3 TangentU : TANGENT;
};

struct VSOut
{
	float4 PosSS : SV_Position;
	float4 PosH : POSH;
	float3 PosW : POSW;
	float4 PosHPrev : POS_PREV;
	float3 NormalW : NORMAL;
	float2 TexUV : TEXUV;
	float3 TangentW : TANGENT;
	nointerpolation uint MatID : MATERIALID;
};

struct GBUFFER_OUT
{
	half4 BaseColor : SV_Target0;
	half2 Normal : SV_Target1;
	half2 MetallicRoughness : SV_Target2;
	half2 MotionVec : SV_Target3;
	half4 EmissiveCurv : SV_Target4;
};

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<DrawCB> g_instance : register(b0, space0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1, space0);
StructuredBuffer<Material> g_materials : register(t0, space0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

GBUFFER_OUT PackGBuffer(half4 baseColor, half3 emissive, float3 sn, half metallic, half roughness,
	half2 motionVec, half surfaceSpreadAngle)
{
	GBUFFER_OUT psout;
	
	psout.BaseColor = baseColor;
	psout.EmissiveCurv = half4(emissive, surfaceSpreadAngle);
	psout.Normal.xy = EncodeUnitNormalAsHalf2(sn);
	//psout.Normal.zw = EncodeUnitNormalAsHalf2(gn);
	psout.MetallicRoughness = half2(metallic, roughness);
	psout.MotionVec = motionVec;

	return psout;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

VSOut mainVS(VSIn vsin)
{
	VSOut vsout;
		
	float3 posW = mul(g_instance.CurrWorld, float4(vsin.PosL, 1.0f));
	float4 posH = mul(float4(posW, 1.0f), g_frame.CurrViewProj);

	float4 prevPosH = float4(mul(g_instance.PrevWorld, float4(vsin.PosL, 1.0f)), 1.0f);
	prevPosH = mul(prevPosH, g_frame.PrevViewProj);
	
	vsout.PosW = posW;
	vsout.PosSS = posH;
	vsout.PosH = posH;
	vsout.PosHPrev = prevPosH;
	vsout.NormalW = mul(vsin.NormalL, (float3x3) g_instance.CurrWorldInvT);
	vsout.TexUV = vsin.TexUV;
	vsout.TangentW = mul(vsin.TangentU, (float3x3) g_instance.CurrWorld);
	vsout.MatID = g_instance.MatID;
	
	return vsout;
}

float2 IntegrateBump(float2 x)
{
	float2 f = floor(0.5f * x);
	return f + 2.0f * max((0.5f * x) - f - 0.5f, 0.0f);
}

float3 GetCheckerboardColor(float2 uv)
{
	float2 fw = max(abs(ddx(uv)), abs(ddy(uv)));
	float width = max(fw.x, fw.y);
	
	float2 p0 = uv - 0.5 * width;
	float2 p1 = uv + 0.5 * width;
	
	float2 i = (IntegrateBump(p1) - IntegrateBump(p0)) / width;
	float area2 = i.x + i.y - 2 * i.x * i.y;
	
	return (1 - area2) * float3(0.85f, 0.6f, 0.7f) + area2 * float3(0.034f, 0.015f, 0.048f);
}

GBUFFER_OUT mainPS(VSOut psin)
{
	Material mat = g_materials[psin.MatID];
		
	half4 baseColor = half4(mat.BaseColorFactor);
	half3 emissiveColor = half3(mat.EmissiveFactor);
	float3 shadingNormal = psin.NormalW * mat.NormalScale;
	half metallic = half(mat.MetallicFactor);
	half roughness = half(mat.RoughnessFactor);

	// minimum Alpha Cutoff is set to MIN_ALPHA_CUTOFF (== 0.01f)
	// so mat.BaseColorTexture.w < MIN_ALPHA_CUTOFF means this pixel does
	// not correspond to any surface on the scene

	if (mat.BaseColorTexture != -1)
	{
		BASE_COLOR_MAP g_baseCol = ResourceDescriptorHeap[g_frame.BaseColorMapsDescHeapOffset + mat.BaseColorTexture];
		half4 baseColorSample = g_baseCol.SampleBias(g_samAnisotropicWrap, psin.TexUV, g_frame.MipBias);
	
		clip(baseColorSample.w - mat.AlphaCuttoff);
	
		baseColor *= baseColorSample;
		baseColor.w = 1.0h;
	}
	else if (dot(mat.BaseColorFactor, 1) == 0)
	{
		baseColor.xyz = half3(GetCheckerboardColor(psin.TexUV * 100.0f));
		baseColor.w = 1.0h;
	}

	if (mat.NormalTexture != -1)
	{
		NORMAL_MAP g_normalMap = ResourceDescriptorHeap[g_frame.NormalMapsDescHeapOffset + mat.NormalTexture];
		float2 bumpNormal = g_normalMap.SampleBias(g_samAnisotropicWrap, psin.TexUV, g_frame.MipBias);
		//float3 bumpNormal = g_normalMap.Sample(g_samAnisotropicWrap, psin.TexUV);

		shadingNormal = TangentSpaceNormalToWorldSpace(bumpNormal, psin.TangentW, psin.NormalW, mat.NormalScale);
	}
	
	if (mat.MetallicRoughnessTexture != -1)
	{
		uint offset = g_frame.MetalnessRoughnessMapsDescHeapOffset + mat.MetallicRoughnessTexture;
		
		// green channel contains roughness values and blue channel contains metalness values
		METALNESS_ROUGHNESS_MAP g_metallicRoughnessMap = ResourceDescriptorHeap[offset];
		half2 mr = g_metallicRoughnessMap.SampleBias(g_samAnisotropicWrap, psin.TexUV, g_frame.MipBias).bg;

		metallic *= mr.x;
		roughness *= mr.y;
	}

	if (mat.EmissiveTexture != -1)
	{
		EMISSIVE_MAP g_emissiveMap = ResourceDescriptorHeap[g_frame.EmissiveMapsDescHeapOffset + mat.EmissiveTexture];
		emissiveColor *= g_emissiveMap.SampleBias(g_samAnisotropicWrap, psin.TexUV, g_frame.MipBias).xyz;
	}

	// undo camera jitter. since the jitter was applied relative to NDC-space, NDC-pos must be used
	float2 prevUnjitteredPosNDC = psin.PosHPrev.xy / psin.PosHPrev.w;
	prevUnjitteredPosNDC -= g_frame.PrevCameraJitter;

	float2 currUnjitteredPosNDC = psin.PosH.xy / psin.PosH.w;
	currUnjitteredPosNDC -= g_frame.CurrCameraJitter;

	// NDC to texture space position: [-1, 1] * [-1, 1] -> [0, 1] * [0, 1]
	float2 prevPosTS = TextureSpaceFromNDC(prevUnjitteredPosNDC);
	float2 currPosTS = TextureSpaceFromNDC(currUnjitteredPosNDC);
	float2 motionVecTS = currPosTS - prevPosTS;

//	float2 motionVecNDC = currUnjitteredPosNDC - prevUnjitteredPosNDC;
			
	// eq. (31) in Ray Tracing Gems 1, ch. 20
	float phi = length(ddx(shadingNormal) + ddy(shadingNormal));
	
//	float3 T = ddx(psin.PosW);
//	float3 B = ddy(psin.PosW);
//	float3 geometricNormal = normalize(cross(T, B));

	GBUFFER_OUT psout = PackGBuffer(baseColor,
									emissiveColor,
									shadingNormal,
									metallic,
	                                roughness,
	                                half2(motionVecTS),
									half(phi));

	
//	psout.Normal = float4(psin.NormalW * mat.NormalScale, psout.Albedo.a - mat.AlphaCuttoff);
//	psout.Normal = float3(0.5f * psin.NormalW * mat.NormalScale + 0.5f);
			
	return psout;
}
