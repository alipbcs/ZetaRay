// Refs:
// 1. D. Zhdan, "Fast Denoising with Self-Stabilizing Recurrent Blurs," GDC, 2020.
// 2. D. Zhdan, "ReBLUR: A Hierarchical Recurrent Denoiser," in Ray Tracing Gems 2, 2021.

#include "ReSTIR_GI_Specular_Common.h"
#include "Reservoir_Specular.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/BRDF.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/Common.hlsli"

#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.015f

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbDNSR> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

// output range is [0, +inf)
float Parallax(float3 currPos, float3 prevPos, float3 currCamPos, float3 prevCamPos)
{
	float3 v1 = normalize(currPos - currCamPos);
	float3 v2 = normalize(prevPos - prevCamPos);
	
	// theta is the angle between v1 & v2
	float cosTheta = saturate(dot(v1, v2));
	float sinTheta = sqrt(1.0f - cosTheta * cosTheta);
	
	float p = sinTheta / max(1e-6f, cosTheta);
	p = pow(p, 0.2f);
	p *= p >= 1e-4;
	
	return p;
}

float Reactivity(float roughness, float ndotwo, float parallax)
{
	// More weight given to recent samples when 
	//     1. parallax is high (more significant viewing angle changes)
	//     2. n.v is greater -- due to Fresnel, at grazing viewing angles more light is 
	//     reflected off the surface (which eventually reaches the eye), and reflections 
	//     by definition don't follow the surface motion.
	//     3. roughness is lower (specular lobe is less spread out)

	// Possible explanation of why rougher surfaces tend to follow surface motion. As view 
	// direction changes:
	//  - For a smoother surface, radiance reflected towards the eye is the result
	//  of integration over a less spread-out domain, so it's more sensitive to view
	//  direction
	//  - For a rougher surface, radiance reflected towards the eye is the result
	//  of integration over a more spread-out domain, so when the view direction
	//  changes, integral is more-or-less over the same domain and the result is less
	//  sensitive to view direction changes
	
	// sensitivity to parallax becomes exponentially higher as:
	//  - whDotWo approaches 0 (grazing angles)
	//  - roughness goes down
	float a = saturate(1.0f - ndotwo);
	a = pow(a, g_local.ViewAngleExp);
	float b = 1.1f + roughness * roughness;
	float parallaxSensitivity = (b + a) / (b - a); // range in [1, +inf)
	
	// exponetially less temporal accumulation as roughness goes to 0
	float powScale = 1.0f + parallax * parallaxSensitivity;
	float f = 1.0f - exp2(-200.0f * roughness * roughness);
	
	// exponentially higher reactivity depending on roughness, parallax and its sensitivity
	f *= pow(roughness, g_local.RoughnessExpScale * powScale);
		
	return 1 - f;
}

float4 GeometryWeight(float4 prevDepths, float2 prevUVs[4], float3 currNormal, float3 currPos, float linearDepth)
{
	float3 prevPos[4];
	prevPos[0] = Math::Transform::WorldPosFromUV(prevUVs[0], prevDepths.x, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	prevPos[1] = Math::Transform::WorldPosFromUV(prevUVs[1], prevDepths.y, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	prevPos[2] = Math::Transform::WorldPosFromUV(prevUVs[2], prevDepths.z, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	prevPos[3] = Math::Transform::WorldPosFromUV(prevUVs[3], prevDepths.w, g_frame.TanHalfFOV, g_frame.AspectRatio,
		g_frame.PrevViewInv);
	
	float4 planeDist = float4(dot(currNormal, prevPos[0] - currPos),
		dot(currNormal, prevPos[1] - currPos),
		dot(currNormal, prevPos[2] - currPos),
		dot(currNormal, prevPos[3] - currPos));
	
	float4 weights = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA * linearDepth;
	
	return weights;
}

float4 NormalWeight(float3 prevNormals[4], float3 currNormal, float alpha)
{
	float4 cosTheta = float4(dot(currNormal, prevNormals[0]),
		dot(currNormal, prevNormals[1]),
		dot(currNormal, prevNormals[2]),
		dot(currNormal, prevNormals[3]));

	float4 angle = Math::ArcCos(cosTheta);
	
	// tolerance angle becomes narrower based on specular lobe half angle
	// Ref: D. Zhdan, "Fast Denoising with Self-Stabilizing Recurrent Blurs," GDC, 2020.
	float scale = alpha / (1.0 + alpha);
	float tolerance = 0.08726646 + 0.27925268 * scale; // == [5.0, 16.0] degrees 
	float4 weight = saturate((tolerance - angle) / tolerance);
	
	return weight;
}

// helps with high frequency roughness textures
float4 RoughnessWeight(float currRoughness, float4 prevRoughness)
{
	float n = currRoughness * currRoughness * 0.99f + 0.01f;
	float4 w = abs(currRoughness - prevRoughness) / n;
	w = saturate(1.0f - w);
	w *= prevRoughness <= g_local.RoughnessCutoff;
	bool4 b1 = prevRoughness < g_local.MinRoughnessResample;
	bool4 b2 = currRoughness < g_local.MinRoughnessResample;
	// don't take roughness into account when there's a sudden change
	w = select(w, 1.0.xxxx, b1 ^ b2);
	
	return w;
}

// resample history using a 2x2 bilinear filter with custom weights
void SampleTemporalCache(uint2 DTid, float3 posW, float3 normal, float linearDepth, float2 uv, float roughness,
	BRDF::SurfaceInteraction surface, float3 samplePos, float curvature, out float tspp, out float3 color, out float prevLinearDepth, 
	out float2 prevUV)
{
	color = 0.0.xxx;
	tspp = 0.0;
	prevLinearDepth = FLT_MAX;
	
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

	// reverse reproject using surface motion
	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
	const half2 motionVec = g_motionVector[DTid];
	const float2 currUV = (DTid + 0.5f) / renderDim;
	float2 prevSurfaceUV = currUV - motionVec;

	// reverse reproject using virtual motion
	float currRayT = length(samplePos - posW);
	float relectionRayT;
	float2 prevVirtualUV = RGI_Spec_Util::VirtualMotionReproject(posW, roughness, surface, currRayT, curvature,
		linearDepth, g_frame.TanHalfFOV, g_frame.PrevViewProj, relectionRayT);

	prevUV = roughness < 0.1f ? prevVirtualUV : prevSurfaceUV;
	
	//	p0-----------p1
	//	|-------------|
	//	|--prev-------|
	//	|-------------|
	//	p2-----------p3
	const float2 f = prevUV * renderDim;
	const float2 topLeft = floor(f - 0.5f); // e.g if p0 is at (20.5, 30.5), then topLeft would be (20, 30)
	const float2 offset = f - (topLeft + 0.5f);
	const float2 topLeftTexelUV = (topLeft + 0.5f) / renderDim;

	// screen-bounds check
	float4 weights = float4(Math::IsWithinBoundsExc(topLeft, renderDim),
							Math::IsWithinBoundsExc(topLeft + float2(1, 0), renderDim),
							Math::IsWithinBoundsExc(topLeft + float2(0, 1), renderDim),
							Math::IsWithinBoundsExc(topLeft + float2(1, 1), renderDim));

	if (dot(1, weights) == 0)
		return;
			
	// geometry weight
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float4 prevDepthsNDC = g_prevDepth.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	float4 prevLinearDepths = Math::Transform::LinearDepthFromNDC(prevDepthsNDC, g_frame.CameraNear);
	
	float2 prevUVs[4];
	prevUVs[0] = topLeftTexelUV;
	prevUVs[1] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 0.0f);
	prevUVs[2] = topLeftTexelUV + float2(0.0f, 1.0f / g_frame.RenderHeight);
	prevUVs[3] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 1.0f / g_frame.RenderHeight);
	weights *= GeometryWeight(prevLinearDepths, prevUVs, normal, posW, linearDepth);
	
	// normal weight
	GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];

	// w (0, 0)		z (1,0)
	// x (0, 1)		y (1, 1)
	const float4 prevNormalsXEncoded = g_prevNormal.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	const float4 prevNormalsYEncoded = g_prevNormal.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
	
	float3 prevNormals[4];
	prevNormals[0] = Math::Encoding::DecodeUnitNormal(float2(prevNormalsXEncoded.x, prevNormalsYEncoded.x));
	prevNormals[1] = Math::Encoding::DecodeUnitNormal(float2(prevNormalsXEncoded.y, prevNormalsYEncoded.y));
	prevNormals[2] = Math::Encoding::DecodeUnitNormal(float2(prevNormalsXEncoded.z, prevNormalsYEncoded.z));
	prevNormals[3] = Math::Encoding::DecodeUnitNormal(float2(prevNormalsXEncoded.w, prevNormalsYEncoded.w));
	weights *= NormalWeight(prevNormals, normal, roughness * roughness);

	// roughness weight
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float4 prevRoughness = g_metalnessRoughness.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
	weights *= RoughnessWeight(roughness, prevRoughness);

	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);
	
	prevLinearDepth = Math::Transform::LinearDepthFromNDC(dot(bilinearWeights, prevDepthsNDC), g_frame.CameraNear);

	weights *= bilinearWeights;
	weights *= weights > 1e-3f;
	const float weightSum = dot(1.0f, weights);

	if (weightSum < 1e-4f)
		return;
	
	// uniformly distribute the weight over the consistent samples
	weights *= rcp(weightSum);

	// tspp
	Texture2D<float4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheDescHeapIdx];
	uint4 histTspp = (uint4) g_prevTemporalCache.GatherAlpha(g_samPointClamp, topLeftTexelUV).wzxy;
	histTspp = max(1, histTspp);
	tspp = round(dot(histTspp, weights));
		
	if (tspp > 0)
	{
		float3 histColor[4];
		const float4 histR = g_prevTemporalCache.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
		const float4 histG = g_prevTemporalCache.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
		const float4 histB = g_prevTemporalCache.GatherBlue(g_samPointClamp, topLeftTexelUV).wzxy;
			
		histColor[0] = float3(histR.x, histG.x, histB.x);
		histColor[1] = float3(histR.y, histG.y, histB.y);
		histColor[2] = float3(histR.z, histG.z, histB.z);
		histColor[3] = float3(histR.w, histG.w, histB.w);

		color = histColor[0] * weights[0] +
				histColor[1] * weights[1] +
				histColor[2] * weights[2] +
				histColor[3] * weights[3];
	}
}

float4 Integrate(uint2 DTid, int2 GTid, SpecularReservoir r, float3 posW, float linearDepth, float roughness, 
	BRDF::SurfaceInteraction surface, float surfaceTspp, float3 surfaceColor, float prevLinearDepth, float2 prevUV)
{
	// adjust accumulation speed base on "reactivity" -- more reactive means higher weight
	// given to recent samples and vice versa
	const float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, g_frame.PrevViewInv._m23);
	const float3 prevSurfacePos = Math::Transform::WorldPosFromUV(prevUV,
		prevLinearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.PrevViewInv);

	const float parallax = Parallax(posW, prevSurfacePos, g_frame.CameraPos, prevCameraPos);
	// 2nd argument to following is supposed to be ndotwo, but for some reason using the half vector
	// results in noticeably lower temporal lag
	float f = Reactivity(roughness, surface.whdotwo, parallax);

	// accumulate
	const float3 signal = r.EvaluateRISEstimate();
	const float maxTspp = roughness >= g_local.MinRoughnessResample ? 
		g_local.MaxTSPP : 
		smoothstep(0, 1, roughness / g_local.MinRoughnessResample) * 6;
	float currTspp = clamp((1 - f) * maxTspp, 0, maxTspp);
	float3 currColor = lerp(surfaceColor, signal, 1.0f / (1.0f + currTspp));
	
	return float4(currColor, currTspp);
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(SPECULAR_DNSR_GROUP_DIM_X, SPECULAR_DNSR_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID)
{
	if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
		return;
	
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_currDepth[DTid.xy];

	// skip sky pixels
	if (depth == 0.0)
		return;

	// roughness and metallic mask
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float2 mr = g_metalnessRoughness[DTid.xy];

	// roughness cuttoff
	if (mr.y > g_local.RoughnessCutoff)
		return;

	SpecularReservoir r = RGI_Spec_Util::PartialReadReservoir_Denoise(DTid.xy,
				g_local.InputReservoir_A_DescHeapIdx,
				g_local.InputReservoir_B_DescHeapIdx,
				g_local.InputReservoir_D_DescHeapIdx);
	
	RWTexture2D<float4> g_nextTemporalCache = ResourceDescriptorHeap[g_local.CurrTemporalCacheDescHeapIdx];
	
	if (!g_local.IsTemporalCacheValid || !g_local.Denoise)
	{
		float3 color = r.EvaluateRISEstimate();
		g_nextTemporalCache[DTid.xy].xyzw = float4(color, 0);
		
		return;
	}

	// current frame's normals
	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_normal[DTid.xy]);

	// current frame's depth
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	const float2 uv = (DTid.xy + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);

	const float3 posW = Math::Transform::WorldPosFromUV(uv,
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	const float3 wo = normalize(g_frame.CameraPos - posW);
	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::InitPartial(normal, mr.y, wo);

	const float3 wi = normalize(r.SamplePos - posW);
	surface.InitComplete(wi, 0.0.xxx, mr.x);

	GBUFFER_CURVATURE g_curvature = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::CURVATURE];
	const float localCurvature = g_curvature[DTid.xy];

	float3 color;
	float prevLinearDepth;
	float tspp;
	float2 prevUV;
	SampleTemporalCache(DTid.xy, posW, normal, linearDepth, uv, mr.y, surface, r.SamplePos, 
		localCurvature, tspp, color, prevLinearDepth, prevUV);

	float4 integrated = Integrate(DTid.xy, GTid.xy, r, posW, linearDepth, mr.y, surface, tspp, color, 
		prevLinearDepth, prevUV);

	g_nextTemporalCache[DTid.xy].xyzw = integrated;
}