#include "SunShadow_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/RT.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/Sampling.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbSunShadow> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

bool EvaluateVisibility(float3 pos, float3 wi, float3 normal, float linearDepth)
{
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
             RAY_FLAG_FORCE_OPAQUE> rayQuery;
    
    // float maxNormalOffset = lerp(2e-5, 1e-4, 1 - wi.y);
    // float3 adjustedOrigin = g_local.test ? RT::OffsetRayRTG(pos, normal) : RT::OffsetRay2(pos, wi, normal, 3e-6, maxNormalOffset);
    float3 adjustedOrigin = RT::OffsetRayRTG(pos, normal);
    adjustedOrigin.y = max(adjustedOrigin.y, 5e-2);

    RayDesc ray;
    ray.Origin = adjustedOrigin;
    ray.TMin = 0;
    ray.TMax = FLT_MAX;
    ray.Direction = wi;
    
    // initialize
    rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);
    
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

[WaveSize(32)]
[numthreads(SUN_SHADOW_THREAD_GROUP_SIZE_X, SUN_SHADOW_THREAD_GROUP_SIZE_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID)
{
    if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
        return;

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
    const float linearDepth = g_depth[DTid.xy];

    if (linearDepth == FLT_MAX)
        return;

    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
    float3 posW = Math::WorldPosFromScreenSpace(DTid.xy,
        renderDim,
        linearDepth, 
        g_frame.TanHalfFOV, 
        g_frame.AspectRatio, 
        g_frame.CurrViewInv,
        g_frame.CurrCameraJitter);

    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_normal[DTid.xy]);
    
    float3 wi = -g_frame.SunDir;

    // sample the cone subtended by sun    
    if (g_local.SoftShadows)
    {
        RNG rng = RNG::Init(DTid.xy, g_frame.FrameNum);
        float pdf = 1.0f;
        float3 sampleLocal = Sampling::UniformSampleCone(rng.Uniform2D(), g_frame.SunCosAngularRadius, pdf);
        
        float3 T;
        float3 B;
        Math::revisedONB(wi, T, B);
        wi = sampleLocal.x * T + sampleLocal.y * B + sampleLocal.z * wi;
    }
    
    bool trace = wi.y > 0 && dot(wi, normal) > 0;
    const bool isUnoccluded = trace ? EvaluateVisibility(posW, wi, normal, linearDepth) : false;
    const uint laneMask = (isUnoccluded << WaveGetLaneIndex());
    const uint ret = WaveActiveBitOr(laneMask);
    
    if (WaveIsFirstLane())
    {
        RWTexture2D<uint> g_shadowMask = ResourceDescriptorHeap[g_local.OutShadowMaskDescHeapIdx];
        g_shadowMask[Gid.xy] = ret;
    }
}
