#include "PreLighting_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/FrameConstants.h"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbCurvature> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Globals
//--------------------------------------------------------------------------------------

groupshared float2 g_normal[ESTIMATE_CURVATURE_GROUP_DIM_Y][ESTIMATE_CURVATURE_GROUP_DIM_X];
groupshared float g_depth[ESTIMATE_CURVATURE_GROUP_DIM_Y][ESTIMATE_CURVATURE_GROUP_DIM_X];
groupshared float3 g_pos[ESTIMATE_CURVATURE_GROUP_DIM_Y][ESTIMATE_CURVATURE_GROUP_DIM_X];

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

void InitSharedMemory(uint2 GTid, float2 encodedNormal, float viewZ, float3 pos)
{
    g_normal[GTid.y][GTid.x] = encodedNormal;
    g_depth[GTid.y][GTid.x] = viewZ;
    g_pos[GTid.y][GTid.x] = pos;
}

float EstimateLocalCurvature(float3 normal, float3 pos, float linearDepth, int2 GTid)
{
    const bool evenGTid_x = (GTid.x & 0x1) == 0;
    const bool evenGTid_y = (GTid.y & 0x1) == 0;

    const float maxDepthDiscontinuity = 0.005f;
    const float depthX = evenGTid_x ? g_depth[GTid.y][GTid.x + 1] : g_depth[GTid.y][GTid.x - 1];
    const float depthY = evenGTid_y ? g_depth[GTid.y + 1][GTid.x] : g_depth[GTid.y - 1][GTid.x];    
    bool invalidX = abs(linearDepth - depthX) >= maxDepthDiscontinuity * linearDepth;
    bool invalidY = abs(linearDepth - depthY) >= maxDepthDiscontinuity * linearDepth;
    
    const float3 normalX = evenGTid_x ? 
        Math::DecodeUnitVector(g_normal[GTid.y][GTid.x + 1]) : 
        Math::DecodeUnitVector(g_normal[GTid.y][GTid.x - 1]);
    const float3 normalY = evenGTid_y ? 
        Math::DecodeUnitVector(g_normal[GTid.y + 1][GTid.x]) : 
        Math::DecodeUnitVector(g_normal[GTid.y - 1][GTid.x]);
    const float3 normalddx = evenGTid_x ? normalX - normal : normal - normalX;
    const float3 normalddy = evenGTid_y ? normalY - normal : normal - normalY;

    invalidX = invalidX || (dot(normalX, normal) < 0.35);
    invalidY = invalidY || (dot(normalY, normal) < 0.35);

    const float3 posX = evenGTid_x ? g_pos[GTid.y][GTid.x + 1] : g_pos[GTid.y][GTid.x - 1];
    const float3 posY = evenGTid_y ? g_pos[GTid.y + 1][GTid.x] : g_pos[GTid.y - 1][GTid.x];
    const float3 posddx = evenGTid_x ? posX - pos : pos - posX;
    const float3 posddy = evenGTid_y ? posY - pos : pos - posY;
    
    // Ref: T. Akenine-Moller, J. Nilsson, M. Andersson, C. Barre-Brisebois, R. Toth 
    // and T. Karras, "Texture Level of Detail Strategies for Real-Time Ray Tracing," in 
    // Ray Tracing Gems 1, 2019.
    const float phi = sqrt((invalidX ? 0 : dot(normalddx, normalddx)) + (invalidY ? 0 : dot(normalddy, normalddy)));
    const float s = sign((invalidX ? 0 : dot(posddx, normalddx)) + (invalidY ? 0 : dot(posddy, normalddy)));
    const float k = 2.0f * phi * s;
    
    return k;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(ESTIMATE_CURVATURE_GROUP_DIM_X, ESTIMATE_CURVATURE_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID)
{
    if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
        return;

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
    const float linearDepth = g_depth[DTid.xy];

    if(linearDepth == FLT_MAX)
        return;

    // reconstruct position from depth buffer
    const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float3 posW = Math::WorldPosFromScreenSpace(DTid.xy,
        renderDim,
        linearDepth,
        g_frame.TanHalfFOV,
        g_frame.AspectRatio,
        g_frame.CurrViewInv);

    GBUFFER_NORMAL g_shadingNormal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
    const float2 encodedNormal = g_shadingNormal[DTid.xy];
    const float3 normal = Math::DecodeUnitVector(encodedNormal);

    InitSharedMemory(GTid.xy, encodedNormal, linearDepth, posW);

    GroupMemoryBarrierWithGroupSync();

    const float k = EstimateLocalCurvature(normal, posW, linearDepth, GTid.xy);

    RWTexture2D<float> g_curvature = ResourceDescriptorHeap[g_local.OutputUAVDescHeapIdx];
    g_curvature[DTid.xy] = k;
}