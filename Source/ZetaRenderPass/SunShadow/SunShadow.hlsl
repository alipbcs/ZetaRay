#include "SunShadow_Common.h"
#include "../Common/FrameConstants.h"
#include "../Common/Common.hlsli"
#include "../Common/LightSource.hlsli"
#include "../Common/GBuffers.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbSunShadow> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

bool EvaluateVisibility(float3 pos, float3 wi, float3 normal)
{
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
             RAY_FLAG_FORCE_OPAQUE> rayQuery;
    
    // float maxNormalOffset = lerp(2e-5, 1e-4, 1 - wi.y);
    // float3 adjustedOrigin = g_local.test ? RT::OffsetRayRTG(pos, normal) : RT::OffsetRay2(pos, wi, normal, 3e-6, maxNormalOffset);
    normal = dot(wi, normal) < 0 ? -normal : normal;
    float3 adjustedOrigin = RT::OffsetRayRTG(pos, normal);
    adjustedOrigin.y = max(adjustedOrigin.y, 5e-2);

    RayDesc ray;
    ray.Origin = adjustedOrigin;
    ray.TMin = 0;
    ray.TMax = FLT_MAX;
    ray.Direction = wi;
    
    rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);
    rayQuery.Proceed();

    if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        return false;

    return true;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[WaveSize(32)]
[numthreads(SUN_SHADOW_THREAD_GROUP_SIZE_X, SUN_SHADOW_THREAD_GROUP_SIZE_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex)
{
    if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
        return;

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    const float z_view = g_depth[DTid.xy];
    if(z_view == FLT_MAX)
        return;

    float2 lensSample = 0;
    float3 origin = g_frame.CameraPos;
    if(g_frame.DoF)
    {
        RNG rngDoF = RNG::Init(RNG::PCG3d(DTid.xyx).zy, g_frame.FrameNum);
        lensSample = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
        lensSample *= g_frame.LensRadius;
    }

    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float3 pos = Math::WorldPosFromScreenSpace2(DTid.xy, renderDim, z_view, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrCameraJitter, 
        g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, g_frame.CurrView[2].xyz, 
        g_frame.DoF, lensSample, g_frame.FocusDepth, origin);

    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_normal[DTid.xy]);

    GBUFFER_METALLIC_ROUGHNESS g_mr = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 mr = g_mr[DTid.xy];
    GBuffer::Flags flags = GBuffer::DecodeMetallic(mr.x);

    float eta_curr = ETA_AIR;
    float eta_next = DEFAULT_ETA_MAT;

    if(flags.transmissive)
    {
        GBUFFER_IOR g_ior = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::IOR];

        float ior = g_ior[DTid.xy];
        eta_next = GBuffer::DecodeIOR(ior);
    }

    float3 baseColor = 0;
    if(BSDF::ShadingData::IsSpecular(mr.y) && g_local.SoftShadows)
    {
        GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::BASE_COLOR];
        baseColor = g_baseColor[DTid.xy].rgb;
    }

    const float3 wo = normalize(origin - pos);
    // Actual subsurface weight isn't needed, just whether it's non-zero
    BSDF::ShadingData surface = BSDF::ShadingData::Init(normal, wo, flags.metallic, mr.y, 
        baseColor, eta_curr, eta_next, flags.transmissive, 0, flags.subsurface);

    float3 wi = -g_frame.SunDir;
    float pdf = 1;

    // Sample the cone subtended by sun
    if (g_local.SoftShadows)
    {
        Light::SunSample sunSample = Light::SampleSunDirection(DTid.xy, g_frame.FrameNum, 
            -g_frame.SunDir, g_frame.SunCosAngularRadius, normal, surface);
        wi = sunSample.wi;
    }

    const bool trace = 
        // Sun below the horizon
        (wi.y > 0) &&
        // Sun hits the backside of non-transmissive surface
        ((dot(wi, normal) > 0) || surface.Transmissive()) &&
        // Make sure BSDF samples are within the valid range
        (dot(wi, -g_frame.SunDir) >= g_frame.SunCosAngularRadius);
    const bool isUnoccluded = trace ? EvaluateVisibility(pos, wi, normal) : false;
    const uint laneMask = ((uint)isUnoccluded << WaveGetLaneIndex());
    const uint ret = WaveActiveBitOr(laneMask);

    if (WaveIsFirstLane())
    {
        RWTexture2D<uint> g_shadowMask = ResourceDescriptorHeap[g_local.OutShadowMaskDescHeapIdx];
        uint waveIdx = Gidx / WaveGetLaneCount();
        uint2 outIdx = uint2(Gid.x, Gid.y * 2 + waveIdx);
        g_shadowMask[outIdx] = ret;
    }
}
