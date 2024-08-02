#include "Display_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/GBuffers.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbSobel> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper structs
//--------------------------------------------------------------------------------------

struct VSOut
{
    float4 PosSS : SV_Position;
    float2 TexCoord : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------

float Sobel(int2 DTid, Texture2D<float> g_mask)
{
    float3 gradientX = -1.0f * g_mask[int2(DTid.x - 1, DTid.y - 1)] -
        2.0f * g_mask[int2(DTid.x - 1, DTid.y)] -
        1.0f * g_mask[int2(DTid.x - 1, DTid.y + 1)] +
        1.0f * g_mask[int2(DTid.x + 1, DTid.y - 1)] +
        2.0f * g_mask[int2(DTid.x + 1, DTid.y)] +
        1.0f * g_mask[int2(DTid.x + 1, DTid.y + 1)];

    float3 gradientY = 1.0f * g_mask[int2(DTid.x - 1, DTid.y - 1)] +
        2.0f * g_mask[int2(DTid.x, DTid.y - 1)] +
        1.0f * g_mask[int2(DTid.x + 1, DTid.y - 1)] -
        1.0f * g_mask[int2(DTid.x - 1, DTid.y + 1)] -
        2.0f * g_mask[int2(DTid.x, DTid.y + 1)] -
        1.0f * g_mask[int2(DTid.x + 1, DTid.y + 1)];

    float3 gradientMagnitude = sqrt(gradientX * gradientX + gradientY * gradientY);
    float lum = Math::Luminance(gradientMagnitude);

    return lum;
}

bool CheckNeighborHood(int2 DTid, Texture2D<float> g_mask)
{
    int2 renderDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);

    [unroll]
    for(int i = 0; i < 3; i++)
    {
        [unroll]
        for(int j = 0; j < 3; j++)
        {
            int2 addr = int2(DTid.x - 1 + i, DTid.y - 1 + j);
            if(!Math::IsWithinBounds(addr, renderDim))
                continue;

            float m = g_mask[addr];
            if(m > 0)
                return true;
        }
    }

    return false;
}

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------

VSOut mainVS(uint vertexID : SV_VertexID)
{
    VSOut vsout;

    vsout.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
    vsout.PosSS = float4(vsout.TexCoord.x * 2 - 1, -vsout.TexCoord.y * 2 + 1, 0, 1);

    return vsout;
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------

float4 mainPS(VSOut psin) : SV_Target
{
    int2 DTid = psin.PosSS.xy;
    Texture2D<float> g_mask = ResourceDescriptorHeap[g_local.MaskDescHeapIdx];

    // Return if mask is zero - pixel is not part of the object
    clip(CheckNeighborHood(DTid, g_mask) ? 1 : -1);

    float lum = Sobel(DTid, g_mask);

    // Return if pixel is not on the outline
    clip(lum - 1e-7);

    return float4(0.913098693, 0.332451582, 0.048171822, 1);
}