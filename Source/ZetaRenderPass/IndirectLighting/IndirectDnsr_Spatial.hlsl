#include "IndirectLighting_Common.h"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/Math.hlsli"
#include "../Common/Sampling.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/Common.hlsli"

#define THREAD_GROUP_SWIZZLING 1
#define NUM_SAMPLES 10
#define PLANE_DIST_RELATIVE_DELTA 0.0025f
#define FILTER_RADIUS_SPECULAR 16
#define SIGMA_L_SPECULAR 0.075f
#define MAX_ROUGHNESS_DELTA 1e-1f
#define NORMAL_EXP 1.5f
#define MIN_FILTER_RADIUS_DIFFUSE 12
#define MAX_FILTER_RADIUS_DIFFUSE 64

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
ConstantBuffer<cbIndirectDnsrSpatial> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

float EdgeStoppingGeometry(float3 samplePos, float3 centerNormal, float centerLinearDepth, 
    float3 currPos, float scale)
{
    float planeDist = dot(centerNormal, samplePos - currPos);
    float weight = abs(planeDist) <= PLANE_DIST_RELATIVE_DELTA * centerLinearDepth * scale;

    return weight;
}

float EdgeStoppingNormal_Diffuse(float3 centerNormal, float3 sampleNormal)
{
    float cosTheta = saturate(dot(centerNormal, sampleNormal));
    float weight = pow(cosTheta, NORMAL_EXP);

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
}

float EdgeStoppingLuminance(float centerLum, float sampleLum, float sigma, float scale)
{
    const float s = 1e-6 + sigma * scale;
    return exp(-abs(centerLum - sampleLum) / s);
}

float3 FilterDiffuse2(int2 DTid, float3 normal, float linearDepth, bool metallic, float3 posW, inout RNG rng)
{
    if (metallic)
        return 0.0.xxx;

    GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];

    Texture2D<float4> g_temporalCache_Diffuse = ResourceDescriptorHeap[g_local.TemporalCacheDiffuseDescHeapIdx];
    const float3 centerColor = g_temporalCache_Diffuse[DTid].rgb;

    if (!g_local.FilterDiffuse)
        return centerColor;

    // 1 / 32 <= x <= 1
    const float tspp = g_temporalCache_Diffuse[DTid].w;
    const float accSpeed = tspp / (float) g_local.MaxTsppDiffuse;

    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

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
        const float filterRadius = lerp(MAX_FILTER_RADIUS_DIFFUSE, MIN_FILTER_RADIUS_DIFFUSE, filterRadiusScale);
        float2 relativeSamplePos = rotatedXZ * filterRadius;
        const float2 relSamplePosAbs = abs(relativeSamplePos);
    
        const int2 samplePosSS = round((float2) DTid + relativeSamplePos);

        if (Math::IsWithinBounds(samplePosSS, (int2) renderDim))
        {
            const float sampleDepth = g_currDepth[samplePosSS];
            const float3 samplePosW = Math::WorldPosFromScreenSpace(samplePosSS, renderDim,
                sampleDepth,
                g_frame.TanHalfFOV,
                g_frame.AspectRatio,
                g_frame.CurrViewInv,
                g_frame.CurrCameraJitter);
            const float w_z = EdgeStoppingGeometry(samplePosW, normal, linearDepth, posW, 1);
                    
            const float3 sampleNormal = Math::DecodeUnitVector(g_currNormal[samplePosSS]);
            const float w_n = EdgeStoppingNormal_Diffuse(normal, sampleNormal);
                    
            const float3 sampleColor = g_temporalCache_Diffuse[samplePosSS].rgb;

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

float3 FilterSpecular(int2 DTid, float3 normal, float linearDepth, bool metallic, float roughness, 
    float3 posW, float3 baseColor, inout RNG rng)
{
    GBUFFER_NORMAL g_currNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
    GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];

    Texture2D<float4> g_temporalCache_Specular = ResourceDescriptorHeap[g_local.TemporalCacheSpecularDescHeapIdx];
    const float3 centerColor = g_temporalCache_Specular[DTid].rgb;

    if (!g_local.FilterSpecular || roughness <= 0.01 || (roughness <= 0.05 && metallic))
        return centerColor;

    const int2 renderDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float centerLum = Math::Luminance(centerColor);
    const float alpha = roughness * roughness;
    const float u0 = rng.Uniform();
    const uint offset = rng.UniformUintBounded_Faster(NUM_SAMPLES);

    const float theta = u0 * TWO_PI;
    const float sinTheta = sin(theta);
    const float cosTheta = cos(theta);

    float3 weightedColor = 0.0.xxx;
    float weightSum = 0.0f;
    int numValidSamples = 0;
    int numSamples = !metallic ? round(smoothstep(0, 0.65, roughness) * 3) : 8;
    numSamples = roughness <= 0.5 ? 1 : numSamples;
    // const float filterRadius = roughness <= 0.5 ? 3 : FILTER_RADIUS_SPECULAR;
    const float filterRadius = FILTER_RADIUS_SPECULAR;

    for (int i = 0; i < numSamples; i++)
    {
        // rotate
        float2 sampleLocalXZ = k_poissonDisk[(offset + i) & (NUM_SAMPLES - 1)];
        float2 rotatedXZ;
        rotatedXZ.x = dot(sampleLocalXZ, float2(cosTheta, -sinTheta));
        rotatedXZ.y = dot(sampleLocalXZ, float2(sinTheta, cosTheta));

        float2 relativeSamplePos = rotatedXZ * filterRadius;
        const int2 samplePosSS = round((float2) DTid + relativeSamplePos);

        if (samplePosSS.x == DTid.x && samplePosSS.y == DTid.y)
            continue;

        if (all(samplePosSS < renderDim) && all(samplePosSS > 0))
        {
            const float sampleDepth = g_currDepth[samplePosSS];
            const float3 samplePosW = Math::WorldPosFromScreenSpace(samplePosSS,
                renderDim,
                sampleDepth,
                g_frame.TanHalfFOV,
                g_frame.AspectRatio,
                g_frame.CurrViewInv,
                g_frame.CurrCameraJitter);
            const float w_z = EdgeStoppingGeometry(samplePosW, normal, linearDepth, posW, 1);

            const float3 sampleNormal = Math::DecodeUnitVector(g_currNormal[samplePosSS]);
            const float w_n = EdgeStoppingNormal_Specular(normal, sampleNormal, alpha);

            const float3 sampleColor = g_temporalCache_Specular[samplePosSS].rgb;
            const float sampleLum = Math::Luminance(sampleColor);
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
    float s = (weightSum > 1e-3) && (numSamples > 0) ? min(accSpeed, 0.3f) : 1.0f;
    filtered = lerp(filtered, centerColor, 0.1);

    return filtered;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

[numthreads(INDIRECT_DNSR_SPATIAL_GROUP_DIM_X, INDIRECT_DNSR_SPATIAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
    if (!g_local.Denoise)
        return;

#if THREAD_GROUP_SWIZZLING
    uint16_t2 swizzledGid;

    // swizzle thread groups for better L2-cache behavior
    // Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
    const uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint16_t2(INDIRECT_DNSR_SPATIAL_GROUP_DIM_X, INDIRECT_DNSR_SPATIAL_GROUP_DIM_Y),
        g_local.DispatchDimX, 
        INDIRECT_DNSR_SPATIAL_TILE_WIDTH, 
        INDIRECT_DNSR_SPATIAL_LOG2_TILE_WIDTH, 
        g_local.NumGroupsInTile,
        swizzledGid);
#else
    const uint2 swizzledDTid = DTid.xy;
    const uint2 swizzledGid = Gid.xy;
#endif

    if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
        return;

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
    const float linearDepth = g_depth[swizzledDTid];

    // skip sky pixels
    if (linearDepth == FLT_MAX)
        return;

    const float2 currUV = (swizzledDTid + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float3 posW = Math::WorldPosFromUV(currUV,
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
    const float3 normal = Math::DecodeUnitVector(g_currNormal[swizzledDTid]);

    GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::BASE_COLOR];
    const float3 baseColor = g_baseColor[swizzledDTid].rgb;

    Texture2D<half4> g_colorB = ResourceDescriptorHeap[g_local.ColorBSrvDescHeapIdx];
    uint16_t2 encoded = asuint16(g_colorB[swizzledDTid].zw);
    uint tmpU = encoded.x | (uint(encoded.y) << 16);
    float tmp = asfloat(tmpU);
    float3 F = baseColor + (1.0f - baseColor) * tmp;

    RNG rng = RNG::Init(swizzledDTid.xy, g_frame.FrameNum);

    float3 filteredDiffuse = FilterDiffuse2(swizzledDTid, normal, linearDepth, isMetallic, posW, rng);
    float3 filteredSpecular = FilterSpecular(swizzledDTid, normal, linearDepth, isMetallic, mr.y, 
        posW, baseColor, rng);

    RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];
    g_final[swizzledDTid.xy].rgb = filteredDiffuse * baseColor + filteredSpecular * (isMetallic ? F : 1);
}