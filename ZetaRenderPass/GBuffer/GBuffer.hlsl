#include "../Common/Common.hlsli"
#include "../Common/Math.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/FrameConstants.h"
#include "../../ZetaCore/Core/Material.h"
#include "GBuffer_Common.h"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbGBuffer> g_local : register(b1);
StructuredBuffer<MeshInstance> g_meshes : register(t0);
StructuredBuffer<Vertex> g_sceneVertices : register(t1);
StructuredBuffer<uint> g_sceneIndices : register(t2);
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
	half2 Normal : SV_Target1;
	float2 MetallicRoughness : SV_Target2;
	float2 MotionVec : SV_Target3;
	float3 Emissive : SV_Target4;
};

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

PS_OUT PackGBuffer(float3 baseColor, float3 emissive, float3 sn, float metalness, float roughness,
	float2 motionVec)
{
	PS_OUT psout;
	
	psout.BaseColor.rgb = baseColor;
	psout.Normal.xy = Math::Encoding::EncodeUnitNormal(sn);
	psout.MetallicRoughness = float2(metalness, roughness);
	psout.MotionVec = motionVec;
	
	if(dot(emissive, 1) != 0)
		psout.Emissive.rgb = emissive;

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
	
	vsout.PosSS = posH;
	vsout.PosH = posH.xyw;
	vsout.PosHPrev = prevPosH.xyw;
	vsout.NormalW = mul(vtx.NormalL, worldInvT);
	vsout.TexUV = vtx.TexUV;
	vsout.TangentW = mul((float3x3) mesh.CurrWorld, vtx.TangentU);
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
		
	float3 baseColor = mat.BaseColorFactor.rgb;
	float3 emissiveColor = mat.EmissiveFactor;
	float3 shadingNormal = psin.NormalW * mat.NormalScale;
	float metalness = mat.MetallicFactor;
	float roughness = mat.RoughnessFactor;

	if (mat.BaseColorTexture != -1)
	{
		BASE_COLOR_MAP g_baseCol = ResourceDescriptorHeap[g_frame.BaseColorMapsDescHeapOffset + mat.BaseColorTexture];
		float4 baseColorSample = g_baseCol.SampleBias(g_samAnisotropicWrap, psin.TexUV, g_frame.MipBias);
	
		clip(baseColorSample.w - mat.AlphaCuttoff);
	
		baseColor *= baseColorSample.rgb;
	}
	// [hack]
//	else if (dot(mat.BaseColorFactor.rgb, 1) == 0)
//	{
//		baseColor.xyz = half3(GetCheckerboardColor(psin.TexUV * 300.0f));
//		baseColor.w = 1.0h;
//	}

	// avoid normal mapping if tangent = (0, 0, 0), which results in NaN
	if (mat.NormalTexture != -1 && abs(dot(psin.TangentW, psin.TangentW)) > 1e-6)
	{
		NORMAL_MAP g_normalMap = ResourceDescriptorHeap[g_frame.NormalMapsDescHeapOffset + mat.NormalTexture];
		float2 bump2 = g_normalMap.SampleBias(g_samAnisotropicWrap, psin.TexUV, g_frame.MipBias);
	
		shadingNormal = Math::Transform::TangentSpaceToWorldSpace(bump2, psin.TangentW, psin.NormalW, mat.NormalScale);		
	}
	
	if (mat.MetalnessRoughnessTexture != -1)
	{
		uint offset = g_frame.MetalnessRoughnessMapsDescHeapOffset + mat.MetalnessRoughnessTexture;
		
		METALNESS_ROUGHNESS_MAP g_metallicRoughnessMap = ResourceDescriptorHeap[offset];
		float2 mr = g_metallicRoughnessMap.SampleBias(g_samAnisotropicWrap, psin.TexUV, g_frame.MipBias);

		metalness *= mr.x;
		roughness *= mr.y;
	}

	if (mat.EmissiveTexture != -1)
	{
		EMISSIVE_MAP g_emissiveMap = ResourceDescriptorHeap[g_frame.EmissiveMapsDescHeapOffset + mat.EmissiveTexture];
		emissiveColor *= g_emissiveMap.SampleBias(g_samAnisotropicWrap, psin.TexUV, g_frame.MipBias).xyz;
	}
	
	// undo camera jitter. since the jitter was applied relative to NDC space, NDC pos must be used
	float2 prevUnjitteredPosNDC = psin.PosHPrev.xy / psin.PosHPrev.z;
	prevUnjitteredPosNDC -= g_frame.PrevCameraJitter;

	float2 currUnjitteredPosNDC = psin.PosH.xy / psin.PosH.z;
	currUnjitteredPosNDC -= g_frame.CurrCameraJitter;

	// NDC to texture space position: [-1, 1] * [-1, 1] -> [0, 1] * [0, 1]
	float2 prevPosTS = Math::Transform::UVFromNDC(prevUnjitteredPosNDC);
	float2 currPosTS = Math::Transform::UVFromNDC(currUnjitteredPosNDC);
	float2 motionVecTS = currPosTS - prevPosTS;

	// eq. (31) in Ray Tracing Gems 1, ch. 20
	//float phi = length(ddx(shadingNormal) + ddy(shadingNormal));
	
//	float3 T = ddx(psin.PosW);
//	float3 B = ddy(psin.PosW);
//	float3 geometricNormal = normalize(cross(T, B));

	if (mat.IsDoubleSided())
	{
		const float3 normalV = mul((float3x3) g_frame.CurrView, shadingNormal);
		float3 posV = float3(currUnjitteredPosNDC, psin.PosH.z);
		posV.x *= g_frame.TanHalfFOV * g_frame.AspectRatio * psin.PosH.z;
		posV.y *= g_frame.TanHalfFOV * psin.PosH.z;
		
		if (dot(normalV, -posV) < 0)
			shadingNormal *= -1;
	}
	
	PS_OUT psout = PackGBuffer(baseColor,
							emissiveColor,
							shadingNormal,
							metalness,
							roughness,
	                        motionVecTS);

	return psout;
}
