#include "SkyDI_Common.h"
#include "SkyDI_Reservoir.hlsli"
#include "../../Common/GBuffers.hlsli"
#include "../../Common/FrameConstants.h"
#include "../../Common/Sampling.hlsli"
#include "../../Common/StaticTextureSamplers.hlsli"
#include "../../Common/BRDF.hlsli"
#include "../../Common/Common.hlsli"

#define THREAD_GROUP_SWIZZLING 1
#define NUM_SAMPLES 8
#define PLANE_DIST_RELATIVE_DELTA 0.01f
#define BASE_FILTER_RADIUS_DIFFUSE 5
#define SIGMA_L_DIFFUSE 0.005f
#define SIGMA_L_SPECULAR 0.005f

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
//	float weight = cosTheta * cosTheta;
//	weight *= weight;
//	weight *= weight;
//	weight *= weight;
	
	float normalExp = 8 + smoothstep(0, 1, 1 - roughness) * 252;
	float weight = pow(cosTheta, normalExp);

	return weight;
}

float EdgeStoppingNormal_Specular(float3 centerNormal, float3 sampleNormal)
{
	float cosTheta = saturate(dot(centerNormal, sampleNormal));
	float weight = cosTheta * cosTheta;
	weight *= weight;
	weight *= weight;
	weight *= weight;
	weight *= weight;
	weight *= weight;
	weight *= weight;
	
	return weight;
}

float EdgeStoppingRoughness(float centerRoughness, float sampleRoughness)
{
	float n = centerRoughness * centerRoughness * 0.99f + 0.01f;
	float w = abs(centerRoughness - sampleRoughness) / n;
	w = saturate(1.0f - w);
	bool b1 = sampleRoughness < g_local.MinRoughnessResample;
	bool b2 = centerRoughness < g_local.MinRoughnessResample;
	// don't take roughness into account when there's been a sudden change
	w = select(w, 1.0, b1 ^ b2);
	
	return w;
}

float EdgeStoppingLuminance(float centerLum, float sampleLum, float sigma, float scale)
{
	const float s = 1e-6 + sigma * scale;
	return exp(-abs(centerLum - sampleLum) / s);
}

float3 FilterDiffuse(int2 DTid, float3 normal, float linearDepth, float metalness, float roughness, float3 posW, inout RNG rng)
{
	if (metalness >= MIN_METALNESS_METAL)
		return 0.0.xxx;
	
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	
	Texture2D<float4> g_temporalCache_Diffuse = ResourceDescriptorHeap[g_local.CurrTemporalCacheDiffuseDescHeapIdx];
	const float3 centerColor = g_temporalCache_Diffuse[DTid].rgb;
	const float centerLum = Math::Color::LuminanceFromLinearRGB(centerColor);
	
	if (!g_local.FilterDiffuse)
		return centerColor;
	
	const int2 renderDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float u0 = rng.Uniform();
	const uint offset = rng.UintRange(0, NUM_SAMPLES - 1);

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
			const float sampleDepth = Math::Transform::LinearDepthFromNDC(g_currDepth[samplePosSS], g_frame.CameraNear);
			const float3 samplePosW = Math::Transform::WorldPosFromScreenSpace(samplePosSS,
				renderDim,
				sampleDepth,
				g_frame.TanHalfFOV,
				g_frame.AspectRatio,
				g_frame.CurrViewInv);
			const float w_z = EdgeStoppingGeometry(samplePosW, normal, linearDepth, posW, 1);
					
			const float3 sampleNormal = Math::Encoding::DecodeUnitNormal(g_currNormal[samplePosSS]);
			const float w_n = EdgeStoppingNormal_Diffuse(normal, sampleNormal, roughness);
					
			const float3 sampleColor = g_temporalCache_Diffuse[samplePosSS].rgb;
			const float sampleLum = Math::Color::LuminanceFromLinearRGB(sampleColor);
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
	const float accSpeed = tspp / (float) g_local.MaxTSPP;

	float3 filtered = weightSum > 1e-3 ? weightedColor / weightSum : 0.0.xxx;
	float s = (weightSum > 1e-3) && (numValidSamples > 0) ? min(accSpeed, 0.2) : 1.0f;
	filtered = lerp(filtered, centerColor, s);
	
	return filtered;
}

float3 FilterSpecular(int2 DTid, float3 normal, float linearDepth, float metalness, float roughness, float3 posW, float3 baseColor, 
	inout RNG rng)
{
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	
	Texture2D<float4> g_temporalCache_Specular = ResourceDescriptorHeap[g_local.CurrTemporalCacheSpecularDescHeapIdx];
	const float3 centerColor = g_temporalCache_Specular[DTid].rgb;
	const float centerLum = Math::Color::LuminanceFromLinearRGB(centerColor);
	const float baseColorLum = Math::Color::LuminanceFromLinearRGB(baseColor);
	const bool isMetal = metalness >= MIN_METALNESS_METAL;
	const bool isTextured = isMetal ? metalness == 1.0f : metalness >= 0.05f;
	
	if (!g_local.FilterSpecular || 
		(roughness <= g_local.MinRoughnessResample && (isMetal || baseColorLum < MAX_LUM_VNDF)) ||
		isTextured)		 // avoid filtering textured surfaces
		return centerColor;
	
	const int2 renderDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float alpha = roughness * roughness;
	const float u0 = rng.Uniform();
	const uint offset = rng.UintRange(0, NUM_SAMPLES - 1);

	const float theta = u0 * TWO_PI;
	const float sinTheta = sin(theta);
	const float cosTheta = cos(theta);

	float3 weightedColor = 0.0.xxx;
	float weightSum = 0.0f;
	int numValidSamples = 0;
	int scale = 1;
	const float filterRadiusBase = 4 + smoothstep(0, 1, 1 - roughness) * 5;
	
	const int numSamplesMetal = max(round(smoothstep(0, 0.3, roughness) * 3), 1);
	const int numSamples = isMetal ?
		numSamplesMetal :
		3;
	
	for (int i = 0; i < numSamples; i++)
	{
		// rotate
		float2 sampleLocalXZ = k_poissonDisk[(offset + i) & (NUM_SAMPLES - 1)];
		float2 rotatedXZ;
		rotatedXZ.x = dot(sampleLocalXZ, float2(cosTheta, -sinTheta));
		rotatedXZ.y = dot(sampleLocalXZ, float2(sinTheta, cosTheta));

		const float filterRadius = filterRadiusBase * (1u << scale);
		scale = scale < 4 ? scale << 1 : 1;
		
		float2 relativeSamplePos = rotatedXZ * filterRadius;
		const int2 samplePosSS = round((float2) DTid + relativeSamplePos);
		if (samplePosSS.x == DTid.x && samplePosSS.y == DTid.y)
			continue;

		if (all(samplePosSS < renderDim) && all(samplePosSS > 0))
		{
			const float sampleDepth = Math::Transform::LinearDepthFromNDC(g_currDepth[samplePosSS], g_frame.CameraNear);
			const float3 samplePosW = Math::Transform::WorldPosFromScreenSpace(samplePosSS,
				renderDim,
				sampleDepth,
				g_frame.TanHalfFOV,
				g_frame.AspectRatio,
				g_frame.CurrViewInv);
			const float w_z = EdgeStoppingGeometry(samplePosW, normal, linearDepth, posW, 1);
					
			const float3 sampleNormal = Math::Encoding::DecodeUnitNormal(g_currNormal[samplePosSS]);
			const float w_n = EdgeStoppingNormal_Specular(normal, sampleNormal);
					
			const float3 sampleColor = g_temporalCache_Specular[samplePosSS].rgb;
			const float sampleLum = Math::Color::LuminanceFromLinearRGB(sampleColor);
			const float w_l = roughness > g_local.MinRoughnessResample ? EdgeStoppingLuminance(centerLum, sampleLum, SIGMA_L_SPECULAR, scale + 1) : 1.0f;

			const float weight = w_z * w_n * w_l * k_gaussian[i];
			if (weight < 1e-3)
				continue;
			
			weightedColor += weight * sampleColor;
			weightSum += weight;
			numValidSamples++;
		}
	}
	
	const float tspp = g_temporalCache_Specular[DTid].w;
	const float accSpeed = tspp / (float) g_local.MaxTSPP;

	float3 filtered = weightSum > 1e-3 ? weightedColor / weightSum : 0.0.xxx;
	float s = (weightSum > 1e-3) && (numValidSamples > 0) ? min(accSpeed, 0.2f) : 1.0f;
	filtered = lerp(filtered, centerColor, s);
	
	return filtered;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(SKY_DI_DNSR_SPATIAL_GROUP_DIM_X, SKY_DI_DNSR_SPATIAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
	if (!g_local.Denoise)
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
	const float depth = g_currDepth[swizzledDTid];
	
	// skip sky pixels
	if (depth == 0.0)
		return;

	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	const float2 currUV = (swizzledDTid.xy + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 posW = Math::Transform::WorldPosFromUV(currUV,
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv);
	
	GBUFFER_METALNESS_ROUGHNESS g_metalnessRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALNESS_ROUGHNESS];
	float2 mr = g_metalnessRoughness[swizzledDTid.xy];

	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_currNormal[swizzledDTid].xy);
		
	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float3 baseColor = g_baseColor[swizzledDTid.xy].rgb;

	RNG rng = RNG::Init(swizzledDTid.xy, g_frame.FrameNum, uint2(g_frame.RenderWidth, g_frame.RenderHeight));

	float3 filteredDiffuse = FilterDiffuse(swizzledDTid, normal, linearDepth, mr.x, mr.y, posW, rng);
	float3 filteredSpecular = FilterSpecular(swizzledDTid, normal, linearDepth, mr.x, mr.y, posW, baseColor, rng);

	RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];
	g_final[swizzledDTid.xy].rgb = filteredDiffuse * baseColor + filteredSpecular;
}