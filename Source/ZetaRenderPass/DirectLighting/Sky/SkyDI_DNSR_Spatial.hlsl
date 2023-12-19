#include "SkyDI_Common.h"
#include "../../Common/GBuffers.hlsli"
#include "../../Common/FrameConstants.h"
#include "../../Common/BRDF.hlsli"
#include "../../Common/StaticTextureSamplers.hlsli"
#include "../../Common/Common.hlsli"

#define THREAD_GROUP_SWIZZLING 1
#define NUM_SAMPLES 8
#define PLANE_DIST_RELATIVE_DELTA 0.005f
#define BASE_FILTER_RADIUS_DIFFUSE 5
#define FILTER_RADIUS_SPECULAR 16
#define SIGMA_L_DIFFUSE 0.005f
#define SIGMA_L_SPECULAR 0.025f
#define MAX_ROUGHNESS_DELTA 5e-2f

static const float2 k_poissonDisk[NUM_SAMPLES] =
{
	float2(0.13039499521255493, 0.06484097242355347),
	float2(-0.2676680088043213, 0.2932010293006897),
	float2(-0.14931300282478333, -0.25950801372528076),
	float2(0.03327000141143799, 0.4452369809150696),
	float2(0.08669400215148926, -0.35944098234176636),
	float2(-0.32113897800445557, 0.016631007194519043),
	float2(0.4022529721260071, -0.12155899405479431),
	float2(0.3521779775619507, 0.18613797426223755)
};

static const float k_gaussian[NUM_SAMPLES] =
{
	0.9861007420525922,
	0.9012031391418621,
	0.942554438329143,
	0.8767211619661169,
	0.913720071901302,
	0.934028331505585,
	0.8899896334884482,
	0.9005706898990838
};

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_SkyDI_DNSR_Spatial> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

float EdgeStoppingGeometry(float3 samplePos, float3 centerNormal, float centerLinearDepth, float3 currPos, float scale)
{
	float planeDist = dot(centerNormal, samplePos - currPos);
	float weight = abs(planeDist) <= PLANE_DIST_RELATIVE_DELTA * centerLinearDepth * scale;

	return weight;
}

float EdgeStoppingNormal_Diffuse(float3 centerNormal, float3 sampleNormal, float roughness)
{
	float cosTheta = saturate(dot(centerNormal, sampleNormal));
	float normalExp = 8 + smoothstep(0, 1, 1 - roughness) * 120;
	float weight = pow(cosTheta, normalExp);

	return weight;
}

float EdgeStoppingNormal_Specular(float3 centerNormal, float3 sampleNormal, float alpha)
{
	float cosTheta = saturate(dot(centerNormal, sampleNormal));
	float angle = Math::ArcCos(cosTheta);
	
	// tolerance angle becomes narrower based on specular lobe half angle
	// Ref: D. Zhdan, "Fast Denoising with Self-Stabilizing Recurrent Blurs," GDC, 2020.
	float specularLobeHalfAngle = alpha / (1.0 + alpha) * 1.5707963267f;
	float tolerance = 0.08726646 + specularLobeHalfAngle;
	float weight = saturate((tolerance - angle) / tolerance);
	
	return weight;
}

float EdgeStoppingRoughness(float centerRoughness, float sampleRoughness)
{
	return 1.0f - saturate(abs(centerRoughness - sampleRoughness) / MAX_ROUGHNESS_DELTA);
#if 0
	bool b1 = sampleRoughness < g_local.MinRoughnessResample;
	bool b2 = centerRoughness < g_local.MinRoughnessResample;
	// don't take roughness into account when there's been a sudden change
	w = select(w, 1.0, b1 ^ b2);
	
	return w;
#endif
}

float EdgeStoppingLuminance(float centerLum, float sampleLum, float sigma, float scale)
{
	const float s = 1e-6 + sigma * scale;
	return exp(-abs(centerLum - sampleLum) / s);
}

float3 FilterDiffuse(int2 DTid, float3 normal, float linearDepth, bool metallic, float roughness, float3 posW, inout RNG rng)
{
	if (metallic)
		return 0.0.xxx;

	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];

	Texture2D<float4> g_temporalCache_Diffuse = ResourceDescriptorHeap[g_local.TemporalCacheDiffuseDescHeapIdx];
	const float3 centerColor = g_temporalCache_Diffuse[DTid].rgb;
	const float centerLum = Math::Color::Luminance(centerColor);

	if (!IS_CB_FLAG_SET(CB_SKY_DI_DNSR_SPATIAL_FLAGS::FILTER_DIFFUSE))
		return centerColor;

	const int2 renderDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float u0 = rng.Uniform();
	const uint offset = rng.UintRange(0, NUM_SAMPLES);

	const float theta = u0 * TWO_PI;
	const float sinTheta = sin(theta);
	const float cosTheta = cos(theta);

	float3 weightedColor = 0.0.xxx;
	float weightSum = 0.0f;
	int numValidSamples = 0;
	int scale = 1;
	const int numSamples = max(round(smoothstep(0, 0.2, roughness) * 6), 1);

	for (int i = 0; i < numSamples; i++)
	{
		// rotate
		float2 sampleLocalXZ = k_poissonDisk[(offset + i) & (NUM_SAMPLES - 1)];
		float2 rotatedXZ;
		rotatedXZ.x = dot(sampleLocalXZ, float2(cosTheta, -sinTheta));
		rotatedXZ.y = dot(sampleLocalXZ, float2(sinTheta, cosTheta));

		const float filterRadius = BASE_FILTER_RADIUS_DIFFUSE * (1u << scale);
		scale = scale < 4 ? scale << 1 : 1;
		const float biasScale = scale + 1;

		const float2 relativeSamplePos = rotatedXZ * filterRadius;
		const int2 samplePosSS = round((float2) DTid + relativeSamplePos);
		
		if (samplePosSS.x == DTid.x && samplePosSS.y == DTid.y)
			continue;

		if (all(samplePosSS < renderDim) && all(samplePosSS > 0))
		{
			const float sampleDepth = g_currDepth[samplePosSS];
			const float3 samplePosW = Math::Transform::WorldPosFromScreenSpace(samplePosSS,
				renderDim,
				sampleDepth,
				g_frame.TanHalfFOV,
				g_frame.AspectRatio,
				g_frame.CurrViewInv,
				g_frame.CurrCameraJitter);
			const float w_z = EdgeStoppingGeometry(samplePosW, normal, linearDepth, posW, 1);
					
			const float3 sampleNormal = Math::Encoding::DecodeUnitVector(g_currNormal[samplePosSS]);
			const float w_n = EdgeStoppingNormal_Diffuse(normal, sampleNormal, roughness);
					
			const float3 sampleColor = g_temporalCache_Diffuse[samplePosSS].rgb;
			const float sampleLum = Math::Color::Luminance(sampleColor);
			const float w_l = roughness > g_local.MinRoughnessResample ? EdgeStoppingLuminance(centerLum, sampleLum, SIGMA_L_DIFFUSE, biasScale) : 1.0f;
			
			const float weight = w_z * w_n * w_l * k_gaussian[i];
			if (weight < 1e-3)
				continue;
			
			weightedColor += weight * sampleColor;
			weightSum += weight;
			numValidSamples++;
		}
	}

	const float tspp = g_temporalCache_Diffuse[DTid].w;
	const float accSpeed = tspp / (float) g_local.MaxTsppDiffuse;

	float3 filtered = weightSum > 1e-3 ? weightedColor / weightSum : 0.0.xxx;
	float s = (weightSum > 1e-3) && (numValidSamples > 0) ? min(accSpeed, 0.07) : 1.0f;
	filtered = lerp(filtered, centerColor, s);
	
	return filtered;
}

float3 FilterSpecular(int2 DTid, float3 normal, float linearDepth, bool metallic, float roughness, 
	float3 posW, float3 baseColor, inout RNG rng)
{
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];

	Texture2D<float4> g_temporalCache_Specular = ResourceDescriptorHeap[g_local.TemporalCacheSpecularDescHeapIdx];
	const float3 centerColor = g_temporalCache_Specular[DTid].rgb;

	if (!IS_CB_FLAG_SET(CB_SKY_DI_DNSR_SPATIAL_FLAGS::FILTER_SPECULAR) || 
		(roughness <= g_local.MinRoughnessResample && metallic))		 // avoid filtering textured surfaces
		return centerColor;
	
	const int2 renderDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float centerLum = Math::Color::Luminance(centerColor);
	const float alpha = roughness * roughness;
	const float u0 = rng.Uniform();
	const uint offset = rng.UintRange(0, NUM_SAMPLES);

	const float theta = u0 * TWO_PI;
	const float sinTheta = sin(theta);
	const float cosTheta = cos(theta);

	float3 weightedColor = 0.0.xxx;
	float weightSum = 0.0f;
	int numValidSamples = 0;
	const int numSamplesMetal = max(round(smoothstep(0, 0.3, roughness) * 3), 1);
	const int numSamples = metallic ? numSamplesMetal : 3;
	
	for (int i = 0; i < numSamples; i++)
	{
		// rotate
		float2 sampleLocalXZ = k_poissonDisk[(offset + i) & (NUM_SAMPLES - 1)];
		float2 rotatedXZ;
		rotatedXZ.x = dot(sampleLocalXZ, float2(cosTheta, -sinTheta));
		rotatedXZ.y = dot(sampleLocalXZ, float2(sinTheta, cosTheta));

		float2 relativeSamplePos = rotatedXZ * FILTER_RADIUS_SPECULAR;
		const int2 samplePosSS = round((float2) DTid + relativeSamplePos);

		if (samplePosSS.x == DTid.x && samplePosSS.y == DTid.y)
			continue;

		if (all(samplePosSS < renderDim) && all(samplePosSS > 0))
		{
			const float sampleDepth = g_currDepth[samplePosSS];
			const float3 samplePosW = Math::Transform::WorldPosFromScreenSpace(samplePosSS,
				renderDim,
				sampleDepth,
				g_frame.TanHalfFOV,
				g_frame.AspectRatio,
				g_frame.CurrViewInv,
				g_frame.CurrCameraJitter);
			const float w_z = EdgeStoppingGeometry(samplePosW, normal, linearDepth, posW, 1);
					
			const float3 sampleNormal = Math::Encoding::DecodeUnitVector(g_currNormal[samplePosSS]);
			const float w_n = EdgeStoppingNormal_Specular(normal, sampleNormal, alpha);
					
			const float3 sampleColor = g_temporalCache_Specular[samplePosSS].rgb;
			const float sampleLum = Math::Color::Luminance(sampleColor);
			const float w_l = EdgeStoppingLuminance(centerLum, sampleLum, SIGMA_L_SPECULAR, 1);

			const float sampleRoughness = g_metallicRoughness[samplePosSS].y;
			const float w_r = EdgeStoppingRoughness(roughness, sampleRoughness);

			const float weight = w_z * w_n * w_l * w_r * k_gaussian[i];
			if (weight < 1e-3)
				continue;
			
			weightedColor += weight * sampleColor;
			weightSum += weight;
			numValidSamples++;
		}
	}

	const float tspp = g_temporalCache_Specular[DTid].w;
	const float accSpeed = tspp / (float) g_local.MaxTsppSpecular;
	float3 filtered = weightSum > 1e-3 ? weightedColor / weightSum : 0.0.xxx;
	float s = (weightSum > 1e-3) && (numValidSamples > 0) ? min(accSpeed, 0.1f) : 1.0f;
	filtered = lerp(filtered, centerColor, s);
	
	return filtered;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(SKY_DI_DNSR_SPATIAL_GROUP_DIM_X, SKY_DI_DNSR_SPATIAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
	if (!IS_CB_FLAG_SET(CB_SKY_DI_DNSR_SPATIAL_FLAGS::DENOISE))
		return;

#if THREAD_GROUP_SWIZZLING
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, uint16_t2(SKY_DI_DNSR_SPATIAL_GROUP_DIM_X, SKY_DI_DNSR_SPATIAL_GROUP_DIM_Y),
		g_local.DispatchDimX, SKY_DI_DNSR_SPATIAL_TILE_WIDTH, SKY_DI_DNSR_SPATIAL_LOG2_TILE_WIDTH, g_local.NumGroupsInTile);
#else
	const uint2 swizzledDTid = DTid.xy;
#endif

	if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
		return;

	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float linearDepth = g_currDepth[swizzledDTid];

	// skip sky pixels
	if (linearDepth == FLT_MAX)
		return;

	const float2 currUV = (swizzledDTid.xy + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 posW = Math::Transform::WorldPosFromUV(currUV,
		float2(g_frame.RenderWidth, g_frame.RenderHeight),
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv,
		g_frame.CurrCameraJitter);

	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	float2 mr = g_metallicRoughness[swizzledDTid];

	bool isMetallic;
	bool isEmissive;
	GBuffer::DecodeMetallicEmissive(mr.x, isMetallic, isEmissive);

	if (isEmissive)
		return;

	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitVector(g_currNormal[swizzledDTid]);

	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float3 baseColor = g_baseColor[swizzledDTid].rgb;

	Texture2D<half4> g_colorB = ResourceDescriptorHeap[g_local.ColorBSrvDescHeapIdx];
	uint16_t2 encoded = asuint16(g_colorB[swizzledDTid].zw);
	uint tmpU = encoded.x | (uint(encoded.y) << 16);
	float tmp = asfloat(tmpU);
	float3 F = baseColor + (1.0f - baseColor) * tmp;

	RNG rng = RNG::Init(swizzledDTid.xy, g_frame.FrameNum);

	float3 filteredDiffuse = FilterDiffuse(swizzledDTid, normal, linearDepth, isMetallic, mr.y, posW, rng);
	float3 filteredSpecular = FilterSpecular(swizzledDTid, normal, linearDepth, isMetallic, mr.y, posW, baseColor, rng);

	RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];
	g_final[swizzledDTid.xy].rgb = filteredDiffuse * baseColor + filteredSpecular * (isMetallic ? F : 1);
}