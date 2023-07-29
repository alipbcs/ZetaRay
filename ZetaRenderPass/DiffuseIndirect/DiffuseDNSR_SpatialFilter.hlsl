#include "Reservoir_Diffuse.hlsli"
#include "../Common/Math.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/Sampling.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/BRDF.hlsli"
#include "../Common/Common.hlsli"

#define THREAD_GROUP_SWIZZLING 1
#define NUM_SAMPLES 10
#define PLANE_DIST_RELATIVE_DELTA 0.015f

static const float2 k_poissonDisk[NUM_SAMPLES] =
{
	float2(-0.2738789916038513, -0.1372080147266388),
	float2(0.12518197298049927, 0.056990981101989746),
	float2(0.12813198566436768, -0.3150410056114197),
	float2(-0.3912320137023926, 0.14719200134277344),
	float2(-0.13983601331710815, 0.25584501028060913),
	float2(-0.15115699172019958, -0.3827739953994751),
	float2(0.2855219841003418, 0.24613499641418457),
	float2(0.35111498832702637, 0.018658995628356934),
	float2(0.3842160105705261, -0.289002001285553),
	float2(0.14527297019958496, 0.425881028175354)
};

static const float k_gaussian[NUM_SAMPLES] =
{
	0.9399471833920812,
	0.9875914185128816,
	0.9264999409121231,
	0.8910805411044391,
	0.9454378629206168,
	0.8942405252636968,
	0.9104744409458791,
	0.9216444796377524,
	0.8585115766108588,
	0.8749084196560382
};

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbDiffuseDNSRSpatial> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Edge-stopping functions and other helpers
//--------------------------------------------------------------------------------------

float EdgeStoppingGeometry(float3 samplePos, float3 centerNormal, float centerLinearDepth, float3 currPos, float scale)
{
	float planeDist = dot(centerNormal, samplePos - currPos);
	
	// lower the tolerance as more samples are accumulated
	float weight = abs(planeDist) <= PLANE_DIST_RELATIVE_DELTA * centerLinearDepth * scale;

	return weight;
}

float EdgeStoppingNormal(float3 input, float3 sample, float scale)
{
	float cosTheta = dot(input, sample);
	
#if 0
	float angle = Math::ArcCos(abs(cosTheta));
	// tolerance angle becomes narrower as more samples are accumulated
	float tolerance = 0.08726646 + 0.7853981f * scale; // == [5.0, 46.0] degrees
	float weight = pow(saturate((tolerance - angle) / tolerance), g_local.NormalExp);
#else	
	float weight = pow(saturate(cosTheta), g_local.NormalExp);
#endif
	
	return weight;
}

float3 Filter(int2 DTid, float3 centerColor, float3 normal, float linearDepth, float tspp)
{
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	Texture2D<half4> g_inTemporalCache = ResourceDescriptorHeap[g_local.TemporalCacheInDescHeapIdx];

	// 1 / 32 <= x <= 1
	const float accSpeed = tspp / (float) g_local.MaxTspp;

	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 uv = (DTid + 0.5f) / renderDim;
	const float3 pos = Math::Transform::WorldPosFromUV(uv, 
		linearDepth, 
		g_frame.TanHalfFOV, 
		g_frame.AspectRatio, 
		g_frame.CurrViewInv, 
		g_frame.CurrProjectionJitter);

	RNG rng = RNG::Init(DTid, g_frame.FrameNum, uint2(g_frame.RenderWidth, g_frame.RenderHeight));
	const float u0 = rng.Uniform();

	const float theta = u0 * TWO_PI;
	const float sinTheta = sin(theta);
	const float cosTheta = cos(theta);

	float3 weightedColor = 0.0.xxx;
	float weightSum = 0.0f;
	int numSamples = 0;
	
	[unroll]
	for (int i = 0; i < NUM_SAMPLES; i++)
	{
		// rotate
		float2 sampleLocalXZ = k_poissonDisk[i];
		float2 rotatedXZ;
		rotatedXZ.x = dot(sampleLocalXZ, float2(cosTheta, -sinTheta));
		rotatedXZ.y = dot(sampleLocalXZ, float2(sinTheta, cosTheta));

		const float filterRadiusScale = smoothstep(1.0 / 32.0, 1.0, accSpeed);
		const float filterRadius = lerp(g_local.MaxFilterRadius * g_local.FilterRadiusScale, g_local.MinFilterRadius, filterRadiusScale);
		float2 relativeSamplePos = rotatedXZ * filterRadius;
		const float2 relSamplePosAbs = abs(relativeSamplePos);
		
		// make sure sampled pixel isn't pixel itself
		if (relSamplePosAbs.x <= 0.5f && relSamplePosAbs.y <= 0.5f)
		{
			relativeSamplePos = relSamplePosAbs.x > relSamplePosAbs.y ?
				float2(sign(relativeSamplePos.x) * (0.5f + 1e-5f), relativeSamplePos.y) :
				float2(relativeSamplePos.x, sign(relativeSamplePos.y) * (0.5f + 1e-5f));
		}
	
		const int2 samplePosSS = round((float2) DTid + relativeSamplePos);

		if (Math::IsWithinBounds(samplePosSS, (int2) renderDim))
		{
			const float sampleDepth = Math::Transform::LinearDepthFromNDC(g_currDepth[samplePosSS], g_frame.CameraNear);
			const float3 samplePosW = Math::Transform::WorldPosFromScreenSpace(samplePosSS, renderDim,
				sampleDepth,
				g_frame.TanHalfFOV,
				g_frame.AspectRatio,
				g_frame.CurrViewInv,
				g_frame.CurrProjectionJitter);
			const float w_z = EdgeStoppingGeometry(samplePosW, normal, linearDepth, pos, 1);
					
			const float3 sampleNormal = Math::Encoding::DecodeUnitNormal(g_currNormal[samplePosSS]);
			const float normalToleranceScale = 1.0f;
			const float w_n = EdgeStoppingNormal(normal, sampleNormal, normalToleranceScale);
					
			const float3 sampleColor = g_inTemporalCache[samplePosSS].rgb;

			const float weight = w_z * w_n * k_gaussian[i];
			if (weight < 1e-3)
				continue;
			
			weightedColor += weight * sampleColor;
			weightSum += weight;
			numSamples++;
		}
	}
	
	float3 filtered = weightSum > 1e-3 ? weightedColor / weightSum : 0.0.xxx;
	float s = (weightSum > 1e-3) && (numSamples > 0) ? min(accSpeed, 0.3f) : 1.0f;	
	filtered = lerp(filtered, centerColor, s);
	
	return filtered;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

static const uint16_t2 GroupDim = uint16_t2(DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_X, DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_Y);

[numthreads(DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_X, DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	const uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, GroupDim, g_local.DispatchDimX,
		DiffuseDNSR_SPATIAL_TILE_WIDTH, DiffuseDNSR_SPATIAL_LOG2_TILE_WIDTH, g_local.NumGroupsInTile);
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

	// skip metallic surfaces
	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	bool isMetallic;
	bool hasBaseColorTexture;
	bool isEmissive;
	GBuffer::DecodeMetallic(g_metallicRoughness[swizzledDTid.xy].x, isMetallic, hasBaseColorTexture, isEmissive);
	
	if (isMetallic || isEmissive)
		return;

	Texture2D<float4> g_inTemporalCache = ResourceDescriptorHeap[g_local.TemporalCacheInDescHeapIdx];
	float4 integratedVals = g_inTemporalCache[swizzledDTid];
	
	GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_currNormal[swizzledDTid].xy);
		
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);

	float3 filtered = Filter(swizzledDTid, integratedVals.xyz, normal, linearDepth, integratedVals.w);
	RWTexture2D<float4> g_outTemporalCache = ResourceDescriptorHeap[g_local.TemporalCacheOutDescHeapIdx];
	g_outTemporalCache[swizzledDTid] = float4(filtered, integratedVals.w);
}