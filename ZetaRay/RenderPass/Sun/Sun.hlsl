#include "../Common/FrameConstants.h"
#include "../Common/LightSourceFuncs.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/BRDF.hlsli"
#include "../Common/RT.hlsli"
#include "../Common/VolumetricLighting.hlsli"

#define RAY_OFFSET_VIEW_DIST_START 30.0
#define USE_SOFT_SHADOWS 0

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0, space0);
RaytracingAccelerationStructure g_sceneBVH : register(t0, space0);
StructuredBuffer<uint> g_owenScrambledSobolSeq : register(t1, space0);
StructuredBuffer<uint> g_scramblingTile : register(t2, space0);
StructuredBuffer<uint> g_rankingTile : register(t3, space0);

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

bool EvaluateVisibility(float3 pos, float3 wi, float3 normal, float viewZ)
{
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
			 RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
			 RAY_FLAG_CULL_NON_OPAQUE> rayQuery;
	
	//float3 adjustedRayOrigin = OffsetRayRTG(pos, normal);
	
	float offsetScale = viewZ / RAY_OFFSET_VIEW_DIST_START;
	float3 adjustedRayOrigin = pos + normal * 1e-2f * (1 + offsetScale * 2);

	RayDesc ray;
	ray.Origin = adjustedRayOrigin;
	ray.TMin = g_frame.RayOffset;
	ray.TMax = FLT_MAX;
	ray.Direction = wi;
	
	// initialize
	rayQuery.TraceRayInline(g_sceneBVH, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);
	
	// traversal
	rayQuery.Proceed();

	// light source is occluded
	if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		return false;
		
	return true;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

VSOut mainVS(uint vertexID : SV_VertexID)
{
	VSOut vsout;

	vsout.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
	vsout.PosSS = float4(vsout.TexCoord.x * 2 - 1, -vsout.TexCoord.y * 2 + 1, 0, 1);

	return vsout;
}

float4 mainPS(VSOut psin) : SV_Target
{
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[psin.PosSS.xy];
	// skip sky pixels
	clip(depth - 1e-6f);
	
	const float linearDepth = Common::ComputeLinearDepthReverseZ(depth, g_frame.CameraNear);
	
	const uint2 textureDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	float3 posW = Common::WorldPosFromScreenSpace(psin.PosSS.xy,
		textureDim, 
		linearDepth, 
		g_frame.TanHalfFOV, 
		g_frame.AspectRatio, 
		g_frame.CurrViewInv);
	float3 wo = normalize(g_frame.CameraPos - posW);

	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	half2 encodedNormals = g_normal[psin.PosSS.xy];

	float3 wi = -g_frame.SunDir;
	float pdf = 1.0f;
	
#if USE_SOFT_SHADOWS
	const uint sampleIdx = g_frame.FrameNum & 31;
	const float u0 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			psin.PosSS.x, psin.PosSS.y, sampleIdx, 2);
	const float u1 = Sampling::samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			psin.PosSS.x, psin.PosSS.y, sampleIdx, 3);

	float3 wiLocal = Sampling::UniformSampleCone(float2(u0, u1), g_frame.SunCosAngularRadius, pdf);
	float4 q = Common::QuaternionFromY(wi);
	// transform from local space to world space
	wi = Common::RotateVector(wiLocal, q);
#endif	
	
	const float3 T = ddx(posW);
	const float3 B = ddy(posW);
	const float3 geometricNormal = normalize(cross(T, B));
	
	if (!EvaluateVisibility(posW.xyz, wi, geometricNormal, linearDepth))
		return 0.0.xxxx;

	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	const half2 mr = g_metallicRoughness[psin.PosSS.xy];
	
	const float3 shadingNormal = Common::DecodeUnitNormalFromHalf2(encodedNormals.xy);

	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	half4 baseColor = g_baseColor[psin.PosSS.xy];

	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::InitPartial(shadingNormal,
		mr.y, mr.x, wo, baseColor.rgb);
	
	surface.InitComplete(-g_frame.SunDir);
	
	float3 f = BRDF::ComputeSurfaceBRDF(surface);

	const float3 sigma_t_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
	const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
	const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;

	posW.y += g_frame.PlanetRadius;
	
	float t = Volumetric::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, posW, -g_frame.SunDir);
	float3 tr = Volumetric::EstimateTransmittance(g_frame.PlanetRadius, posW, -g_frame.SunDir, t,
		sigma_t_rayleigh, sigma_t_mie, sigma_t_ozone, 8);

	float3 L_i = (tr * f) * g_frame.SunIlluminance;
	//L_i /= pdf;

	GBUFFER_EMISSIVE_COLOR g_emissiveColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::EMISSIVE_COLOR];
	half3 L_e = g_emissiveColor[psin.PosSS.xy].rgb;
	L_i += L_e;

	return float4(L_i, 1.0f);
//	return float4(0.42f, 0.0f, 0.0f, 1.0f);
//	return float4(tr, 1.0f);
//	return float4(shadowFactor, shadowFactor, shadowFactor, 1.0f);
}
