#include "../Common/FrameConstants.h"
#include "../Common/LightSourceFuncs.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/BRDF.hlsli"
#include "../Common/RT.hlsli"
#include "../Common/VolumetricLighting.hlsli"

#define USE_SOFT_SHADOWS 0

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0, space0);
RaytracingAccelerationStructure g_sceneBVH : register(t0, space0);
StructuredBuffer<uint> g_owenScrambledSobolSeq : register(t1, space0);
StructuredBuffer<uint> g_scramblingTile : register(t2, space0);
StructuredBuffer<uint> g_rankingTile : register(t3, space0);
//StructuredBuffer<AnalyticalLightSource> g_analyticalLights : register(t1, space0);

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

bool EvaluateVisibility(float3 pos, float3 wi, float3 normal)
{
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
			 RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
			 RAY_FLAG_CULL_NON_OPAQUE> rayQuery;
	
	if (dot(normal, wi) < 0)
		wi *= -1.0f;
	
//	float3 adjustedRayOrigin = pos;
//	float3 adjustedRayOrigin = OffsetRayRTG(pos, geometricNormal);
	float3 adjustedRayOrigin = pos + normal * 5e-3f;
	//float3 adjustedRayOrigin = pos + normal * 1.0f;
//	float3 adjustedRayOrigin = posW + geometricNormal * g_frame.RayOffset;

//	const float3 adjustedRayOrigin = OffsetRayOrigin(pos, geometricNormal, wi);
	
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
	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	half4 baseColor = g_baseColor[psin.PosSS.xy];

	// not on the scene surface
	const float EPS = 0.0001f;
	clip(baseColor.w - MIN_ALPHA_CUTOFF - EPS);

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float linearDepth = g_depth[psin.PosSS.xy];
	linearDepth = ComputeLinearDepthReverseZ(linearDepth, g_frame.CameraNear);
	
	const uint2 textureDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	float3 posW = WorldPosFromScreenSpacePos(psin.PosSS.xy, 
		textureDim, 
		linearDepth, 
		g_frame.TanHalfFOV, 
		g_frame.AspectRatio, 
		g_frame.CurrViewInv, 
		g_frame.CurrCameraJitter);
	float3 wo = normalize(g_frame.CameraPos - posW);

	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	half2 encodedNormals = g_normal[psin.PosSS.xy];
	//float3 geometricNormal = normalize(DecodeUnitNormalFromHalf2(encodedNormals.zw));

	float3 wi = -g_frame.SunDir;
	float pdf = 1.0f;
	
#if USE_SOFT_SHADOWS
	const uint sampleIdx = g_frame.FrameNum & 31;
	const float u0 = samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			psin.PosSS.x, psin.PosSS.y, sampleIdx, 0);
	const float u1 = samplerBlueNoiseErrorDistribution(g_owenScrambledSobolSeq, g_rankingTile, g_scramblingTile,
			psin.PosSS.x, psin.PosSS.y, sampleIdx, 1);

	float3 wiLocal = UniformSampleCone(float2(u0, u1), g_frame.SunCosAngularRadius, pdf);
	
	// build rotation quaternion for transforming from sun dir to y = (0, 1, 0)
	float3 snCrossY = float3(-wi.z, 0.0f, wi.x);
	float4 q = float4(snCrossY, 1.0f + dot(wi, float3(0.0f, 1.0f, 0.0f)));
	// transform wiLocal from local space to world space
	float4 qReverse = q * float4(-1.0f, -1.0f, -1.0f, 1.0f);
	wi = RotateVector(wiLocal, qReverse);
#endif	
	
	const float3 T = ddx(posW);
	const float3 B = ddy(posW);
	const float3 geometricNormal = normalize(cross(T, B));
	
	if (!EvaluateVisibility(posW.xyz, wi, geometricNormal))
		return float4(0.0f, 0.0f, 0.0f, 0.0f);

	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	const half2 mr = g_metallicRoughness[psin.PosSS.xy];
	
	const float3 shadingNormal = DecodeUnitNormalFromHalf2(encodedNormals.xy);

	SurfaceInteraction surface = ComputePartialSurfaceInteraction(shadingNormal,
		mr.y, mr.x, wo, baseColor.rgb);
	
	CompleteSurfaceInteraction(-g_frame.SunDir, surface);
	float3 f = ComputeSurfaceBRDF(surface);

	const float3 sigma_t_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
	const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
	const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;

	posW.y += g_frame.PlanetRadius;
	
	float t = IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, posW, -g_frame.SunDir);
	float3 tr = EstimateTransmittance(g_frame.PlanetRadius, posW , -g_frame.SunDir, t, 
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
