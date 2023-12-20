#include "../Common/FrameConstants.h"
#include "../Common/GBuffers.hlsli"
#include "../Common/Math.hlsli"

struct cbGenerateDepthBuffer
{
    uint UavDescHeapIdx;
};

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbGenerateDepthBuffer> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
        return;

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
    const float depth = g_depth[DTid.xy];

    RWTexture2D<float> g_rasterDepthBuffer = ResourceDescriptorHeap[g_local.UavDescHeapIdx];
    float ndcDepth = depth == FLT_MAX ? 0 : g_frame.CameraNear / depth;
    g_rasterDepthBuffer[DTid.xy].r = ndcDepth;
}