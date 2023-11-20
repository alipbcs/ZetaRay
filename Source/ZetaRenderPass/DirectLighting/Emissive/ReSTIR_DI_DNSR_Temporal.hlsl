#include "DirectLighting_Common.h"
#include "../../Common/GBuffers.hlsli"
#include "../../Common/FrameConstants.h"
#include "../../Common/BRDF.hlsli"
#include "../../Common/StaticTextureSamplers.hlsli"
#include "../../Common/Common.hlsli"

#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.005f
#define VIEW_ANGLE_EXP 0.5f
#define ROUGHNESS_EXP_SCALE 0.65f

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_ReSTIR_DI_DNSR_Temporal> g_local : register(b1);

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
	float a = saturate(1.0f - ndotwo);
	a = pow(a, VIEW_ANGLE_EXP);
	float b = 1.1f + roughness * roughness;
	float parallaxSensitivity = (b + a) / (b - a); // range in [1, +inf)

	// exponetially less temporal accumulation as roughness goes to 0
	float powScale = 1.0f + parallax * parallaxSensitivity;
	float f = 1.0f - exp2(-200.0f * roughness * roughness);

	// exponentially higher reactivity depending on roughness, parallax and its sensitivity
	f *= pow(roughness, ROUGHNESS_EXP_SCALE * powScale);

	return 1 - f;
}

float4 GeometryTest(float4 prevDepths, float2 prevUVs[4], float3 currNormal, float3 currPos, float linearDepth)
{
	float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	
	float3 prevPos[4];
	prevPos[0] = Math::WorldPosFromUV(prevUVs[0], renderDim, prevDepths.x, 
		g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevViewInv, g_frame.PrevCameraJitter);
	prevPos[1] = Math::WorldPosFromUV(prevUVs[1], renderDim, prevDepths.y, 
		g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevViewInv, g_frame.PrevCameraJitter);
	prevPos[2] = Math::WorldPosFromUV(prevUVs[2], renderDim, prevDepths.z, 
		g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevViewInv, g_frame.PrevCameraJitter);
	prevPos[3] = Math::WorldPosFromUV(prevUVs[3], renderDim, prevDepths.w, 
		g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevViewInv, g_frame.PrevCameraJitter);

	float4 planeDist = float4(dot(currNormal, prevPos[0] - currPos),
		dot(currNormal, prevPos[1] - currPos),
		dot(currNormal, prevPos[2] - currPos),
		dot(currNormal, prevPos[3] - currPos));

	float4 weights = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA * linearDepth;

	return weights;
}

float GeometryTest(float prevDepth, float2 prevUV, float3 currNormal, float3 currPos, float linearDepth)
{
	float3 prevPos = Math::WorldPosFromUV(prevUV, 
		float2(g_frame.RenderWidth, g_frame.RenderHeight), 
		prevDepth, 
		g_frame.TanHalfFOV, 
		g_frame.AspectRatio,
		g_frame.PrevViewInv, 
		g_frame.PrevCameraJitter);

	float planeDist = dot(currNormal, prevPos - currPos);
	float weight = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA * linearDepth;

	return weight;
}

float4 NormalWeight(float3 prevNormals[4], float3 currNormal, float roughness)
{
	float4 cosTheta = saturate(float4(dot(currNormal, prevNormals[0]),
		dot(currNormal, prevNormals[1]),
		dot(currNormal, prevNormals[2]),
		dot(currNormal, prevNormals[3])));
	
	float normalExp = lerp(16, 64, 1 - roughness);
	float4 weight = pow(cosTheta, normalExp);

	return weight;
}

float4 RoughnessWeight(float currRoughness, float4 prevRoughness)
{
	float n = currRoughness * currRoughness * 0.99f + 0.01f;
	float4 w = abs(currRoughness - prevRoughness) / n;
	w = saturate(1.0f - w);

	return w;
}

/*
float3 FilterFirefly(float3 currColor, int2 DTid, float linearDepth, float3 normal, float3 posW, float roughness)
{
	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	Texture2D<float4> g_colorA = ResourceDescriptorHeap[g_local.ColorASrvDescHeapIdx];	

	float3 minColor = currColor;
	float3 maxColor = 0.0.xxx;
	float currLum = Math::Luminance(currColor);
	if(currLum < 1e-4)
		return currColor;
	
	float minLum = FLT_MAX;
	float maxLum = 0;
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 rcpRenderDim = 1.0f / float2(g_frame.RenderWidth, g_frame.RenderHeight);
	
	//int r = roughness < 0.3 && g_local.RoughnessExpScale > 20 ? 2 : 1;
	const int r = roughness < 0.3  ? 2 : 1;
	
	//[unroll]
	for (int i = -r; i <= r; i++)
	{
		//[unroll]
		for (int j = -r; j <= r; j++)
		{
			if (i == 0 && j == 0)
				continue;
			
			int2 addr = int2(DTid.x + j, DTid.y + i);
			if (any(addr) < 0 || any(addr >= renderDim))
				continue;
			
			const float neighborLinearDepth = Math::Transform::LinearDepthFromNDC(g_depth[addr], g_frame.CameraNear);
			if (neighborLinearDepth == FLT_MAX)
				continue;
			
			float2 neighborUV = (addr + 0.5) * rcpRenderDim;
//			if (!GeometryTest2(neighborLinearDepth, neighborUV, normal, posW, linearDepth))
//				continue;
			
			float3 neighborColor = g_colorA[addr].rgb;
			float neighborLum = Math::Luminance(neighborColor);

			if (neighborLum < minLum)
			{
				minLum = neighborLum;
				minColor = neighborColor;
			}
			else if (neighborLum > g_local.RoughnessExpScale * maxLum)
			{
				maxLum = neighborLum;
				maxColor = neighborColor;
			}
		}
	}
	
	float3 ret = currLum < minLum ? minColor : (currLum > maxLum ? maxColor : currColor);
	//float3 ret = lumSum > 1e-5 && currLum > maxLum ? maxColor : currColor;
//	float3 ret;
//	if(g_local.RoughnessExpScale > 5)
//		ret = currLum > maxLum ? maxColor : currColor;
//	else
//		ret = currLum < minLum ? minColor : (currLum > maxLum ? maxColor : currColor);
	
	return ret;
}
*/

void SampleTemporalCache_Bilinear(uint2 DTid, float3 currPos, float3 currNormal, float linearDepth, float2 currUV, 
	float2 prevUV, out float tspp, out float3 color)
{
	color = 0.0.xxx;
	tspp = 0;

	//	p0-----------p1
	//	|-------------|
	//	|--prev-------|
	//	|-------------|
	//	p2-----------p3
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 f = prevUV * renderDim;
	const float2 topLeft = floor(f - 0.5f); // e.g if p0 is at (20.5, 30.5), then topLeft would be (20, 30)
	const float2 offset = f - (topLeft + 0.5f);
	const float2 topLeftTexelUV = (topLeft + 0.5f) / renderDim;

	// previous frame's depth
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float4 prevDepths = g_prevDepth.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;

	float2 prevUVs[4];
	prevUVs[0] = topLeftTexelUV;
	prevUVs[1] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 0.0f);
	prevUVs[2] = topLeftTexelUV + float2(0.0f, 1.0f / g_frame.RenderHeight);
	prevUVs[3] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 1.0f / g_frame.RenderHeight);
	const float4 geoWeights = GeometryTest(prevDepths, prevUVs, currNormal, currPos, linearDepth);

	// weight must be zero for out-of-bound samples
	const float4 isInBounds = float4(Math::IsWithinBounds(topLeft, renderDim),
									 Math::IsWithinBounds(topLeft + float2(1, 0), renderDim),
									 Math::IsWithinBounds(topLeft + float2(0, 1), renderDim),
									 Math::IsWithinBounds(topLeft + float2(1, 1), renderDim));

	// previous frame's metallic
	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];	
	float4 prevMetallic = g_metallicRoughness.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;

	bool4 isNeighborMetallic;
	bool4 isNeighborEmissive;
	GBuffer::DecodeMetallicEmissive(prevMetallic, isNeighborMetallic, isNeighborEmissive);	

	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);

	float4 weights = geoWeights * !isNeighborEmissive * !isNeighborMetallic * bilinearWeights * isInBounds;
	// zero out samples with very low weights to avoid bright spots
	weights *= weights > 1e-3f;
	const float weightSum = dot(1.0f, weights);

	if (1e-4f < weightSum)
	{
		// uniformly distribute the total weight over the valid samples
		weights /= weightSum;

		// tspp
		Texture2D<float4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheDiffuseDescHeapIdx];
		uint4 histTspp = (uint4) g_prevTemporalCache.GatherAlpha(g_samPointClamp, topLeftTexelUV).wzxy;
		
		histTspp = max(1, histTspp);
		tspp = round(dot(histTspp, weights));

		if (tspp > 0)
		{
			// color
			float3 colorHistSamples[4];
			const float4 histR = g_prevTemporalCache.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
			const float4 histG = g_prevTemporalCache.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
			const float4 histB = g_prevTemporalCache.GatherBlue(g_samPointClamp, topLeftTexelUV).wzxy;
			
			colorHistSamples[0] = float3(histR.x, histG.x, histB.x);
			colorHistSamples[1] = float3(histR.y, histG.y, histB.y);
			colorHistSamples[2] = float3(histR.z, histG.z, histB.z);
			colorHistSamples[3] = float3(histR.w, histG.w, histB.w);

			color = colorHistSamples[0] * weights[0] +
					colorHistSamples[1] * weights[1] +
					colorHistSamples[2] * weights[2] +
					colorHistSamples[3] * weights[3];
		}
	}
}

bool SampleTemporalCache_CatmullRom(uint2 DTid, float3 currPos, float3 currNormal, float linearDepth, float2 currUV, 
	float2 prevUV, out float tspp, out float3 color)
{
	color = 0.0.xxx;
	tspp = 0;
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

	float2 samplePos = prevUV * renderDim;
	float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
	float2 f = samplePos - texPos1;
	float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
	float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
	float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
	float2 w3 = f * f * (-0.5f + 0.5f * f);

	float2 w12 = w1 + w2;
	float2 offset12 = w2 / (w1 + w2);

	float2 texPos0 = texPos1 - 1;
	float2 texPos3 = texPos1 + 2;
	float2 texPos12 = texPos1 + offset12;

	texPos0 /= renderDim;
	texPos3 /= renderDim;
	texPos12 /= renderDim;

	float2 prevUVs[5];
	prevUVs[0] = float2(texPos12.x, texPos0.y);
	prevUVs[1] = float2(texPos0.x, texPos12.y);
	prevUVs[2] = float2(texPos12.x, texPos12.y);
	prevUVs[3] = float2(texPos3.x, texPos12.y);
	prevUVs[4] = float2(texPos12.x, texPos3.y);

	// previous frame's depth
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float2 prevDepths[5];

	[unroll]
	for (int i = 0; i < 5; i++)
	{
		prevDepths[i].x = g_prevDepth.SampleLevel(g_samLinearClamp, prevUVs[i], 0.0f);
		prevDepths[i].y = prevDepths[i].x;
	}

	float weights[5];

	[unroll]
	for (int j = 0; j < 5; j++)
	{
		float isInBounds = all(prevUVs[j] <= 1.0.xx) && all(prevUVs[j] >= 0.0f);
		float geoWeight = GeometryTest(prevDepths[j].y, prevUVs[j], currNormal, currPos, linearDepth);
		weights[j] = isInBounds * geoWeight;
	}

	bool allValid = weights[0] > 1e-3f;

	[unroll]
	for (int k = 1; k < 5; k++)
		allValid = allValid && (weights[k] > 1e-3f);

	if (allValid)
	{
		Texture2D<float4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheDiffuseDescHeapIdx];
		
		float4 results[5];
		results[0] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[0], 0.0f) * w12.x * w0.y;

		results[1] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[1], 0.0f) * w0.x * w12.y;
		results[2] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[2], 0.0f) * w12.x * w12.y;
		results[3] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[3], 0.0f) * w3.x * w12.y;

		results[4] = g_prevTemporalCache.SampleLevel(g_samLinearClamp, prevUVs[4], 0.0f) * w12.x * w3.y;

		tspp = results[0].w * weights[0] +
			results[1].w * weights[1] +
			results[2].w * weights[2] +
			results[3].w * weights[3] +
			results[4].w * weights[4];
		
		tspp = max(1, tspp);
		color = results[0].rgb + results[1].rgb + results[2].rgb + results[3].rgb + results[4].rgb;

		return true;
	}

	return false;
}

void TemporalAccumulation_Diffuse(uint2 DTid, float2 currUV, float3 posW, float3 normal, float linearDepth, bool metallic, 
	float roughness, float3 Li_d, float prevSurfaceLinearDepth, float2 prevSurfaceUV, bool motionVecValid)
{
	float tspp = 0;
	float3 color = 0.0.xxx;

	if (g_local.IsTemporalCacheValid && motionVecValid)
	{
		// try to use Catmull-Rom interpolation first
		bool success = SampleTemporalCache_CatmullRom(DTid, posW, normal, linearDepth, currUV, prevSurfaceUV, 
			tspp, color);
		
		// if it failed, then interpolate history using bilinear filtering with custom weights
		if (!success)
			SampleTemporalCache_Bilinear(DTid, posW, normal, linearDepth, currUV, prevSurfaceUV, tspp, color);
	}

	//float3 currColor = dot(color, 1) <= 1e-5 ? Li_d : lerp(color, Li_d, 1.0f / (1.0f + tspp));
	float3 currColor = lerp(color, Li_d, 1.0f / (1.0f + tspp));
	tspp = min(tspp + 1, g_local.MaxTsppDiffuse);

	RWTexture2D<float4> g_currTemporalCache_Diffuse = ResourceDescriptorHeap[g_local.CurrTemporalCacheDiffuseDescHeapIdx];
	g_currTemporalCache_Diffuse[DTid] = float4(currColor, tspp);
}

void SampleTemporalCache_Virtual(uint2 DTid, float3 posW, float3 normal, float linearDepth, float2 uv, bool metallic, float roughness,
	float2 prevSurfaceUV, out float3 color, out float tspp)
{
	color = 0.0.xxx;
	tspp = 0.0;

	// reverse reproject using virtual motion
	float2 prevUV = prevSurfaceUV;
	
//	if (roughness < g_local.MinRoughnessResample)
	{
//		GBUFFER_CURVATURE g_curvature = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::CURVATURE];
//		const float localCurvature = g_curvature[DTid.xy];

//		prevUV = RDI_Util::VirtualMotionReproject(posW, roughness, surface, rayT, localCurvature, linearDepth,
//			g_frame.TanHalfFOV, g_frame.PrevViewProj);
	}

	//	p0-----------p1
	//	|-------------|
	//	|--prev-------|
	//	|-------------|
	//	p2-----------p3
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 f = prevUV * renderDim;
	const float2 topLeft = floor(f - 0.5f);
	const float2 offset = f - (topLeft + 0.5f);
	const float2 topLeftTexelUV = (topLeft + 0.5f) / renderDim;

	// screen-bounds check
	float4 weights = float4(Math::IsWithinBounds(topLeft, renderDim),
							Math::IsWithinBounds(topLeft + float2(1, 0), renderDim),
							Math::IsWithinBounds(topLeft + float2(0, 1), renderDim),
							Math::IsWithinBounds(topLeft + float2(1, 1), renderDim));

	if (dot(1, weights) == 0)
		return;

	// geometry weight
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float4 prevDepthsNDC = g_prevDepth.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	float4 prevLinearDepths = prevDepthsNDC;
	float2 prevUVs[4];
	prevUVs[0] = topLeftTexelUV;
	prevUVs[1] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 0.0f);
	prevUVs[2] = topLeftTexelUV + float2(0.0f, 1.0f / g_frame.RenderHeight);
	prevUVs[3] = topLeftTexelUV + float2(1.0f / g_frame.RenderWidth, 1.0f / g_frame.RenderHeight);
	weights *= GeometryTest(prevLinearDepths, prevUVs, normal, posW, linearDepth);

	// normal weight
	GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];

	// w (0, 0)		z (1,0)
	// x (0, 1)		y (1, 1)
	const float4 prevNormalsXEncoded = g_prevNormal.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	const float4 prevNormalsYEncoded = g_prevNormal.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;

	float3 prevNormals[4];
	prevNormals[0] = Math::DecodeUnitVector(float2(prevNormalsXEncoded.x, prevNormalsYEncoded.x));
	prevNormals[1] = Math::DecodeUnitVector(float2(prevNormalsXEncoded.y, prevNormalsYEncoded.y));
	prevNormals[2] = Math::DecodeUnitVector(float2(prevNormalsXEncoded.z, prevNormalsYEncoded.z));
	prevNormals[3] = Math::DecodeUnitVector(float2(prevNormalsXEncoded.w, prevNormalsYEncoded.w));
	weights *= NormalWeight(prevNormals, normal, roughness);

	// roughness weight
	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	const float4 prevRoughness = g_metallicRoughness.GatherGreen(g_samPointClamp, topLeftTexelUV).wzxy;
	weights *= RoughnessWeight(roughness, prevRoughness);

	// metallic weight
	const float4 prevEncodedMetalness= g_metallicRoughness.GatherRed(g_samPointClamp, topLeftTexelUV).wzxy;
	const bool4 prevMetallic = GBuffer::DecodeMetallic(prevEncodedMetalness);
	weights *= prevMetallic == metallic;
	
	const float4 bilinearWeights = float4((1.0f - offset.x) * (1.0f - offset.y),
									       offset.x * (1.0f - offset.y),
									       (1.0f - offset.x) * offset.y,
									       offset.x * offset.y);

	weights *= bilinearWeights;
	weights *= weights > 1e-3f;
	const float weightSum = dot(1.0f, weights);

	if (weightSum < 1e-3f)
		return;

	// uniformly distribute the weight over the nonzero samples
	weights /= weightSum;

	// tspp
	Texture2D<float4> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheSpecularDescHeapIdx];
	float4 histTspp = g_prevTemporalCache.GatherAlpha(g_samPointClamp, topLeftTexelUV).wzxy;
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

void TemporalAccumulation_Specular(uint2 DTid, float2 currUV, float3 posW, float3 normal, float linearDepth, bool metallic, 
	float roughness, float3 Li_s, float prevSurfaceLinearDepth, float2 prevSurfaceUV, bool motionVecValid)
{
	float3 color = 0.0.xxx;
	float tspp = 0;
	if (g_local.IsTemporalCacheValid && motionVecValid)
		SampleTemporalCache_Virtual(DTid, posW, normal, linearDepth, currUV, metallic, roughness, prevSurfaceUV, color, tspp);

//	if(g_local.FilterFirefly)
//		Li_s = FilterFirefly(Li_s, DTid, linearDepth, normal, posW, roughness);

	const float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, g_frame.PrevViewInv._m23);
	const float3 prevSurfacePosW = Math::WorldPosFromUV(prevSurfaceUV,
		float2(g_frame.RenderWidth, g_frame.RenderHeight),
		prevSurfaceLinearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.PrevViewInv, 
		g_frame.PrevCameraJitter);

	const float3 wo = normalize(g_frame.CameraPos - posW);
	const float ndotwo = saturate(dot(normal, wo));

	const float parallax = Parallax(posW, prevSurfacePosW, g_frame.CameraPos, prevCameraPos);
	float reactivity = Reactivity(roughness, ndotwo, parallax);
	//float minTspp = metallic ? 0 : 1;
	tspp = clamp((1 - reactivity) * g_local.MaxTsppSpecular, 1, g_local.MaxTsppSpecular);

	float3 currColor = dot(color, 1) <= 1e-5 ? Li_s : lerp(color, Li_s, 1.0f / (1.0f + tspp));

	RWTexture2D<float4> g_currTemporalCache_Specular = ResourceDescriptorHeap[g_local.CurrTemporalCacheSpecularDescHeapIdx];
	g_currTemporalCache_Specular[DTid.xy].xyzw = float4(currColor, tspp);
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_DI_DNSR_TEMPORAL_GROUP_DIM_X, RESTIR_DI_DNSR_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
	if(!g_local.Denoise)
		return;

	if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
		return;

	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float linearDepth = g_currDepth[DTid.xy];

	if (linearDepth == FLT_MAX)
		return;

	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	const float2 mr = g_metallicRoughness[DTid.xy];

	bool isMetallic;
	bool isEmissive;
	GBuffer::DecodeMetallicEmissive(mr.x, isMetallic, isEmissive);

	if (isEmissive)
		return;

	Texture2D<float4> g_colorA = ResourceDescriptorHeap[g_local.ColorASrvDescHeapIdx];
	Texture2D<float4> g_colorB = ResourceDescriptorHeap[g_local.ColorBSrvDescHeapIdx];

	float3 Li_s = g_colorA[DTid.xy].rgb;
	float3 Li_d = float3(g_colorA[DTid.xy].a, g_colorB[DTid.xy].rg);

	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::DecodeUnitVector(g_normal[DTid.xy]);

	const float2 currUV = (DTid.xy + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 posW = Math::WorldPosFromUV(currUV,
		float2(g_frame.RenderWidth, g_frame.RenderHeight),
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv,
		g_frame.CurrCameraJitter);

	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
	const float2 motionVec = g_motionVector[DTid.xy];
	const float2 prevSurfaceUV = currUV - motionVec;
	const bool motionVecValid = all(prevSurfaceUV >= 0.0f) && all(prevSurfaceUV <= 1.0f);

	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	float z = g_prevDepth.SampleLevel(g_samLinearClamp, prevSurfaceUV, 0.0f);	
	const float prevSurfaceLinearDepth = motionVecValid ? z : FLT_MAX;

	if(!isMetallic)
	{
		TemporalAccumulation_Diffuse(DTid.xy, currUV, posW, normal, linearDepth, isMetallic, mr.y, Li_d,
			prevSurfaceLinearDepth, prevSurfaceUV, motionVecValid);	
	}

	TemporalAccumulation_Specular(DTid.xy, currUV, posW, normal, linearDepth, isMetallic, mr.y, Li_s,
		prevSurfaceLinearDepth, prevSurfaceUV, motionVecValid);
}