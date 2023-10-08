#include "../Common/Common.hlsli"
#include "../Common/Math.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/BRDF.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../../ZetaCore/Core/Material.h"
#include "GBuffer_Common.h"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbGBuffer> g_local : register(b1);
StructuredBuffer<MeshInstance> g_meshes : register(t0);
StructuredBuffer<Vertex> g_sceneVertices : register(t1);
ByteAddressBuffer g_sceneIndices : register(t2);
ByteAddressBuffer g_materials : register(t3);

//--------------------------------------------------------------------------------------
// Helper structs
//--------------------------------------------------------------------------------------

struct VSOut
{
	float4 PosSS : SV_Position;
	float3 PosH : POSH;
	float3 PosHPrev : POSH_PREV;
	float3 NormalW : NORMAL;
	float2 TexUV : TEXUV;
	float3 TangentW : TANGENT;
	nointerpolation uint MatID : MATERIALID;
};

struct PS_OUT
{
	float3 BaseColor : SV_Target0;
	float2 Normal : SV_Target1;
	float2 MetallicRoughness : SV_Target2;
	float2 MotionVec : SV_Target3;
	float3 Emissive : SV_Target4;
	float Curvature : SV_Target5;
};

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

PS_OUT PackGBuffer(float3 baseColor, float3 emissive, float3 sn, float metalness, float roughness,
	float2 motionVec, float localCurvature)
{
	PS_OUT psout;

	psout.BaseColor = baseColor;
	psout.Normal.xy = Math::Encoding::EncodeUnitVector(sn);
	psout.MotionVec = clamp(motionVec, -1, 1);
	psout.MetallicRoughness = float2(metalness, roughness);
	psout.Emissive = max(0, emissive);
	psout.Curvature = localCurvature;
	
	return psout;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

VSOut mainVS(uint vtxID : SV_VertexID)
{
	MeshInstance mesh = g_meshes[g_local.MeshIdxinBuff];
	Vertex vtx = g_sceneVertices[mesh.BaseVtxOffset + vtxID];

	VSOut vsout;
		
	float3 posW = mul(mesh.CurrWorld, float4(vtx.PosL, 1.0f));
	float4 posH = mul(g_frame.CurrViewProj, float4(posW, 1.0f));

	float4 prevPosH = float4(mul(mesh.PrevWorld, float4(vtx.PosL, 1.0f)), 1.0f);
	prevPosH = mul(g_frame.PrevViewProj, prevPosH);

	// W (4x3)
	// W^T (3x4) = g_instance.CurrWorld
	// (W^-1)^T = (W^T)^-1   (assuming W is invertible)
	// (W^T)^-1 = (g_instance.CurrWorld)^-1
	float3x3 worldInvT = Math::Inverse(((float3x3) mesh.CurrWorld));
	
	float3 n = Math::Encoding::DecodeSNorm3(vtx.NormalL);
	float3 t = Math::Encoding::DecodeSNorm3(vtx.TangentU);
	
	vsout.PosSS = posH;
	vsout.PosH = posH.xyw;
	vsout.PosHPrev = prevPosH.xyw;
	vsout.NormalW = mul(n, worldInvT);
	vsout.TexUV = vtx.TexUV;
	vsout.TangentW = mul((float3x3) mesh.CurrWorld, t);
	vsout.MatID = mesh.IdxInMatBuff;

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

PS_OUT mainPS(VSOut psin)
{
	const uint byteOffset = psin.MatID * sizeof(Material);
	const Material mat = g_materials.Load<Material>(byteOffset);
		
	float4 baseColor = Math::Color::UnpackRGBA(mat.BaseColorFactor);
	float4 emissiveColorNormalScale = Math::Color::UnpackRGBA(mat.EmissiveFactorNormalScale);
	float3 shadingNormal = psin.NormalW;
	float2 metalnessAlphaCuttoff = Math::Color::UnpackRG(mat.MetallicFactorAlphaCuttoff);
	float roughness = mat.RoughnessFactor;

	if (mat.BaseColorTexture != -1)
	{
		BASE_COLOR_MAP g_baseCol = ResourceDescriptorHeap[g_frame.BaseColorMapsDescHeapOffset + mat.BaseColorTexture];
		float4 baseColorSample = g_baseCol.SampleBias(g_samAnisotropicWrap, psin.TexUV, g_frame.MipBias);
		baseColor *= baseColorSample;
	
		clip(baseColor.w - metalnessAlphaCuttoff.y);
	}
	// [hack]
	else if (dot(baseColor.rgb, 1) == 0)
	{
		baseColor.rgb = half3(GetCheckerboardColor(psin.TexUV * 300.0f));
	}

	// avoid normal mapping if tangent = (0, 0, 0), which results in NaN
	if (mat.NormalTexture != -1 && abs(dot(psin.TangentW, psin.TangentW)) > 1e-6)
	{
		NORMAL_MAP g_normalMap = ResourceDescriptorHeap[g_frame.NormalMapsDescHeapOffset + mat.NormalTexture];
		float2 bump2 = g_normalMap.SampleBias(g_samAnisotropicWrap, psin.TexUV, g_frame.MipBias);
	
		shadingNormal = Math::Transform::TangentSpaceToWorldSpace(bump2, psin.TangentW, psin.NormalW, emissiveColorNormalScale.w);
	}
	
	if (mat.MetallicRoughnessTexture != -1)
	{
		uint offset = g_frame.MetallicRoughnessMapsDescHeapOffset + mat.MetallicRoughnessTexture;
		
		METALLIC_ROUGHNESS_MAP g_metallicRoughnessMap = ResourceDescriptorHeap[offset];
		float2 mr = g_metallicRoughnessMap.SampleBias(g_samAnisotropicWrap, psin.TexUV, g_frame.MipBias);

		metalnessAlphaCuttoff.x *= mr.x;
		roughness *= mr.y;
	}

	uint16_t emissiveTex = mat.GetEmissiveTex();
	float emissiveStrength = mat.GetEmissiveStrength();

	if (emissiveTex != -1)
	{
		EMISSIVE_MAP g_emissiveMap = ResourceDescriptorHeap[g_frame.EmissiveMapsDescHeapOffset + emissiveTex];
		emissiveColorNormalScale.rgb *= g_emissiveMap.SampleBias(g_samAnisotropicWrap, psin.TexUV, g_frame.MipBias).xyz;
	}

	emissiveColorNormalScale.rgb *= emissiveStrength;

	float2 prevPosNDC = psin.PosHPrev.xy / psin.PosHPrev.z;
	float2 currPosNDC = psin.PosH.xy / psin.PosH.z;
	float2 prevUV = Math::Transform::UVFromNDC(prevPosNDC);
	float2 currUV = Math::Transform::UVFromNDC(currPosNDC);
	// undo camera jitter
	prevUV -= g_frame.PrevCameraJitter / float2(g_frame.RenderWidth, g_frame.RenderHeight);
	currUV -= g_frame.CurrCameraJitter / float2(g_frame.RenderWidth, g_frame.RenderHeight);
	float2 motionVec = currUV - prevUV;
	
	float3 posV = float3(currPosNDC, psin.PosH.z);
	posV.x *= g_frame.TanHalfFOV * g_frame.AspectRatio * psin.PosH.z;
	posV.y *= g_frame.TanHalfFOV * psin.PosH.z;
	float3 posW = mul(g_frame.CurrViewInv, float4(posV, 1.0f));

	// eq. (31) in Ray Tracing Gems 1, ch. 20
	float3 dNdX = ddx(shadingNormal);
	float3 dNdY = ddy(shadingNormal);
	float phi = length(dNdX + dNdY);
	float s = sign(dot(ddx(posW), dNdX) + dot(ddy(posW), dNdY));
	float k = 2.0f * phi * s;

//	float3 T = ddx(psin.PosW);
//	float3 B = ddy(psin.PosW);
//	float3 geometricNormal = normalize(cross(T, B));

	if (mat.IsDoubleSided())
	{
		const float3 normalV = mul((float3x3) g_frame.CurrView, shadingNormal);
	
		if (dot(normalV, -posV) < 0)
			shadingNormal *= -1;
	}
	
	metalnessAlphaCuttoff.x = GBuffer::EncodeMetallic(metalnessAlphaCuttoff.x, mat.BaseColorTexture, emissiveColorNormalScale.rgb);
	
	PS_OUT psout = PackGBuffer(baseColor.rgb,
							emissiveColorNormalScale.rgb,
							shadingNormal,
							metalnessAlphaCuttoff.x,
							roughness,
	                        motionVec, 
							k);

	return psout;
}
