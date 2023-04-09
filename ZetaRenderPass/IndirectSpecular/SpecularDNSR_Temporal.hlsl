#include "ReSTIR_GI_Specular_Common.h"
#include "Reservoir_Specular.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/BRDF.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/Common.hlsli"

#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.015f

//--------------------------------------------------------------------------------------
// 
//--------------------------------------------------------------------------------------

groupshared half3 g_normalDepth[SPECULAR_DNSR_GROUP_DIM_Y][SPECULAR_DNSR_GROUP_DIM_X];
groupshared float3 g_pos[SPECULAR_DNSR_GROUP_DIM_Y][SPECULAR_DNSR_GROUP_DIM_X];

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbDNSR> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

// Ref: T. Akenine-Moller, J. Nilsson, M. Andersson, C. Barre-Brisebois, R. Toth 
// and T. Karras, "Texture Level of Detail Strategies for Real-Time Ray Tracing," in 
// Ray Tracing Gems 1, 2019.
float EstimateLocalCurvature(float3 normal, float3 pos, float linearDepth, int2 GTid)
{
	const float depthX = (GTid.x & 0x1) == 0 ? g_normalDepth[GTid.y][GTid.x + 1].z : g_normalDepth[GTid.y][GTid.x - 1].z;
	const float depthY = (GTid.y & 0x1) == 0 ? g_normalDepth[GTid.y + 1][GTid.x].z : g_normalDepth[GTid.y - 1][GTid.x].z;
	
	const float maxDepthDiscontinuity = 0.005f;
	const bool invalidX = abs(linearDepth - depthX) >= maxDepthDiscontinuity * linearDepth;
	const bool invalidY = abs(linearDepth - depthY) >= maxDepthDiscontinuity * linearDepth;
	
	const float3 normalX = (GTid.x & 0x1) == 0 ?
		Math::Encoding::DecodeUnitNormal(g_normalDepth[GTid.y][GTid.x + 1].xy) :
		Math::Encoding::DecodeUnitNormal(g_normalDepth[GTid.y][GTid.x - 1].xy);
	const float3 normalY = (GTid.y & 0x1) == 0 ?
		Math::Encoding::DecodeUnitNormal(g_normalDepth[GTid.y + 1][GTid.x].xy) :
		Math::Encoding::DecodeUnitNormal(g_normalDepth[GTid.y - 1][GTid.x].xy);
	const float3 normalddx = (GTid.x & 0x1) == 0 ? normalX - normal : normal - normalX;
	const float3 normalddy = (GTid.y & 0x1) == 0 ? normalY - normal : normal - normalY;

	const float3 posX = (GTid.x & 0x1) == 0 ? g_pos[GTid.y][GTid.x + 1] : g_pos[GTid.y][GTid.x - 1];
	const float3 posY = (GTid.y & 0x1) == 0 ? g_pos[GTid.y + 1][GTid.x] : g_pos[GTid.y - 1][GTid.x];
	const float3 posddx = (GTid.x & 0x1) == 0 ? posX - pos : pos - posX;
	const float3 posddy = (GTid.y & 0x1) == 0 ? posY - pos : pos - posY;
	
	const float phi = sqrt((invalidX ? 0 : dot(normalddx, normalddx)) + (invalidY ? 0 : dot(normalddy, normalddy)));
	const float s = sign((invalidX ? 0 : dot(posddx, normalddx)) + (invalidY ? 0 : dot(posddy, normalddy)));
	const float k = 2.0f * phi * s;
	
	return k;
}

// output range is [0, +inf)
float Parallax(float3 currPos, float3 prevPos, float3 currCamPos, float3 prevCamPos)
{
	float3 v1 = normalize(currPos - currCamPos);
	float3 v2 = normalize(prevPos - prevCamPos); // prevCamPos = PrevViewInv[3].xyz
	
	// theta is the angle between v1 & v2
	float cosTheta = saturate(dot(v1, v2));
	float tanTheta = sqrt(1.0f - cosTheta * cosTheta) / max(1e-6f, cosTheta);

	return tanTheta;
}

float MaxM(float M_max, float alpha, float whDotWo, float parallax)
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
	float a = 1.0f - whDotWo;
	float b = 1.1f + alpha * alpha;
	float parallaxSensitivity = (b + a) / (b - a); // range in [1, +inf)
	
	// exponetially less temporal accumulation as roughness goes to 0
	float powScale = 1.0f + parallax * parallaxSensitivity;
	float f = 1.0f - exp2(-200.0f * alpha * alpha);
	
	// exponentially bring down f depending on parallax and its sensitivity
	f *= pow(alpha, powScale);
		
	return clamp(f * M_max, 1, M_max);
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
	//float weight = pow(saturate((tolerance - angle) / tolerance), g_local.NormalExp);
	float4 weight = saturate((tolerance - angle) / tolerance);
	weight *= weight;
	
	return weight;
}

// helps with high frequency roughness textures
// Ref: D. Zhdan, "Fast Denoising with Self-Stabilizing Recurrent Blurs," GDC, 2020.
float4 RoughnessWeight(float currRoughness, float4 prevRoughness)
{
	float n = currRoughness * currRoughness * 0.99f + 0.01f;
	float4 w = abs(currRoughness - prevRoughness) / n;
	w = saturate(1.0f - w);
	w *= prevRoughness <= g_local.RoughnessCutoff;
	
	return w;
}

// resample history using a 2x2 bilinear filter with custom weights
void SampleTemporalCache_Bilinear(uint2 DTid, float3 posW, float3 normal, float linearDepth, float2 uv, float roughness,
	BRDF::SurfaceInteraction surface, float localCurvature, float3 samplePos, inout uint tspp, out float3 color)
{
	// pixel position for this thread
	// reminder: pixel positions are 0.5, 1.5, 2.5, ...
	const float2 screenDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

	// reverse reproject current pixel
	float relectionRayT;
	float2 prevUV = RGI_Spec_Util::VirtualMotionReproject(posW, roughness, surface, samplePos, localCurvature, linearDepth,
		g_frame.TanHalfFOV, g_frame.PrevViewProj, relectionRayT);

	//	p0-----------p1
	//	|-------------|
	//	|--prev-------|
	//	|-------------|
	//	p2-----------p3
	const float2 f = prevUV * screenDim;
	const float2 topLeft = floor(f - 0.5f); // e.g if p0 is at (20.5, 30.5), then topLeft would be (20, 30)
	const float2 offset = f - (topLeft + 0.5f);
	const float2 topLeftTexelUV = (topLeft + 0.5f) / screenDim;

	// screen-bounds check
	const float4 isInBounds = float4(Math::IsWithinBoundsExc(topLeft, screenDim),
									 Math::IsWithinBoundsExc(topLeft + float2(1, 0), screenDim),
									 Math::IsWithinBoundsExc(topLeft + float2(0, 1), screenDim),
									 Math::IsWithinBoundsExc(topLeft + float2(1, 1), screenDim));

	if(dot(1, isInBounds) == 0)
		return;
			
	// geometry weight
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float4 prevDepths = g_prevDepth.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	prevDepths = Math::Transform::LinearDepthFromNDC(prevDepths, g_frame.CameraNear);
	
	float2 prevUVs[4];
	prevUVs[0] = topLeftTexelUV;
	prevUVs[1] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 0.0f);
	prevUVs[2] = topLeftTexelUV + float2(0.0f, 1.0f / g_frame.RenderHeight);
	prevUVs[3] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 1.0f / g_frame.RenderHeight);
	const float4 geoWeights = GeometryWeight(prevDepths, prevUVs, normal, posW, linearDepth);
	
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
		
	const float4 normalWeights = NormalWeight(prevNormals, normal, surface.alpha);

	// roughness weight
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	const float4 prevRoughness = g_metalnessRoughness.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
	
	const float4 roughnessWeights = RoughnessWeight(roughness, prevRoughness);

	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);
	
	float4 weights = geoWeights * normalWeights * roughnessWeights * bilinearWeights * isInBounds;
	//float4 weights = geoWeights * bilinearWeights * isInBounds;
	const float weightSum = dot(1.0f, weights);

	if (1e-6f < weightSum)
	{
		// uniformly distribute the weight over the consistent samples
		weights *= rcp(weightSum);

		// tspp
		Texture2D<float4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheDescHeapIdx];
		uint4 histTspp = (uint4) g_prevTemporalCache.GatherAlpha(g_samPointClamp, topLeftTexelUV).wzxy;
		histTspp = max(1, histTspp);
		tspp = round(dot(histTspp, weights));
		
		if (tspp > 0)
		{
			// color
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
}

void SampleTemporalCache_CatmullRom(uint2 DTid, float3 posW, float3 normal, float linearDepth, float2 uv, float roughness,
	BRDF::SurfaceInteraction surface, float localCurvature, float3 samplePos, inout uint tspp, out float3 color)
{
	// pixel position for this thread
	// reminder: pixel positions are 0.5, 1.5, 2.5, ...
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

	// reverse reproject current pixel
	float relectionRayT;
	float2 prevUV = RGI_Spec_Util::VirtualMotionReproject(posW, roughness, surface, samplePos, localCurvature, linearDepth,
		g_frame.TanHalfFOV, g_frame.PrevViewProj, relectionRayT);

	if (!Math::IsWithinBoundsExc(prevUV, 1.0f.xx))
	{
		color = float3(1, 0, 0);
		return;
	}

	Texture2D<float4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheDescHeapIdx];
	color = Common::SampleTextureCatmullRom_5Tap(g_prevTemporalCache, g_samLinearClamp, prevUV, renderDim);

	uint4 histTspp = (uint4) g_prevTemporalCache.GatherAlpha(g_samPointClamp, prevUV).wzxy;
	histTspp = max(1, histTspp);
}

void Integrate(SpecularReservoir r, inout uint tspp, inout float3 histColor)
{
	const float3 signal = r.EvaluateRISEstimate();

	// use linear weights rather than exponential weights, which comparatively give a higher weight 
	// to initial samples right after disocclusion
	const float accumulationSpeed = 1.0f / (1.0f + tspp);

	// accumulate
	histColor = lerp(histColor, signal, accumulationSpeed);

	// don't accumulate more than MaxTspp temporal samples (temporal lag <-> noise tradeoff)
	tspp = min(tspp + 1, g_local.MaxTSPP);
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

	uint tspp = 0;
	float3 color = 0.0f.xxx;

	// current frame's normals
	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const half2 encodedNormal = g_normal[DTid.xy];
	const float3 normal = Math::Encoding::DecodeUnitNormal(encodedNormal);

	// current frame's depth
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	const float2 uv = (DTid.xy + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);

	const float3 posW = Math::Transform::WorldPosFromUV(uv,
		linearDepth,
		g_frame.TanHalfFOV, 
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	g_normalDepth[GTid.y][GTid.x] = half3(encodedNormal, linearDepth);
	g_pos[GTid.y][GTid.x] = posW;

	GroupMemoryBarrierWithGroupSync();
	
	const float k = EstimateLocalCurvature(normal, posW, linearDepth, GTid.xy);

	const float3 wo = normalize(g_frame.CameraPos - posW);
	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::InitPartial(normal, mr.y, wo);

	SpecularReservoir r = RGI_Spec_Util::PartialReadReservoir_Shading(DTid.xy,
				g_local.InputReservoir_A_DescHeapIdx,
				g_local.InputReservoir_B_DescHeapIdx,
				g_local.InputReservoir_D_DescHeapIdx);

	const float3 wi = normalize(r.SamplePos - posW);
	surface.InitComplete(wi, 0.0.xxx, mr.x);
	
	if (g_local.IsTemporalCacheValid && g_local.Denoise)
	{
		if (!g_local.CatmullRom)
			SampleTemporalCache_Bilinear(DTid.xy, posW, normal, linearDepth, uv, mr.y, surface, k, r.SamplePos, tspp, color);
		else
		{
//			color = float3(1, 0, 0);
			SampleTemporalCache_CatmullRom(DTid.xy, posW, normal, linearDepth, uv, mr.y, surface, k, r.SamplePos, tspp, color);
			tspp = 16;
		}
	}

	Integrate(r, tspp, color);
	
	RWTexture2D<float4> g_nextTemporalCache = ResourceDescriptorHeap[g_local.CurrTemporalCacheDescHeapIdx];
	g_nextTemporalCache[DTid.xy].xyzw = float4(color, tspp);
}