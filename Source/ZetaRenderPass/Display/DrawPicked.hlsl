#include "../Common/FrameConstants.h"
#include "Display_Common.h"

//--------------------------------------------------------------------------------------
// Helper structs
//--------------------------------------------------------------------------------------

struct VSIn
{
    float3 PosL : POSITION;
    float2 TexUV : TEXUV;
    int2 NormalL : NORMAL;
    int2 TangentU : TANGENT;
};

struct VSOut
{
    float4 PosSS : SV_Position;
};

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbDrawPicked> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

VSOut mainVS(VSIn vsin)
{
    VSOut vsout;

    float4x4 WVP = float4x4(g_local.row0, g_local.row1, g_local.row2, g_local.row3);
    vsout.PosSS = mul(float4(vsin.PosL, 1.0f), WVP);

    return vsout;
}

float mainPS(VSOut psin) : SV_Target
{
    return 1;
}
