#include "Display_Common.h"
#include "Tonemap.hlsli"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/GBuffers.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbDisplayPass> g_local : register(b1);

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
    const float2 uv = psin.PosSS.xy / float2(g_frame.DisplayWidth, g_frame.DisplayHeight);

    Texture2D<half4> g_composited = ResourceDescriptorHeap[g_local.InputDescHeapIdx];
    //float4 composited = g_composited[psin.PosSS.xy].rgba;
    float3 composited = g_composited.SampleLevel(g_samPointClamp, uv, 0).rgb;
    float3 display = composited;

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
    float z = g_depth.SampleLevel(g_samPointClamp, uv, 0);
    if(z == FLT_MAX)
        return float4(display, 1);

    if(g_local.AutoExposure)
    {
        Texture2D<float2> g_exposure = ResourceDescriptorHeap[g_local.ExposureDescHeapIdx];
        const float exposure = g_exposure[int2(0, 0)].x;
        const float3 exposedColor = composited * exposure;
        display = exposedColor;
    }

    if (g_local.Tonemapper == (int) Tonemapper::NEUTRAL)
    {
        display = Tonemap::tony_mc_mapface(display, g_local.LUTDescHeapIdx);
        float3 desaturation = Math::Luminance(display);
        display = lerp(desaturation, display, g_local.Saturation);
    }
    else if (g_local.Tonemapper == (int) Tonemapper::AgX_DEFAULT)
        display = Tonemap::AgX_Default(display);
    else if (g_local.Tonemapper == (int) Tonemapper::AgX_GOLDEN)
        display = Tonemap::AgX_Golden(display);
    else if (g_local.Tonemapper == (int) Tonemapper::AgX_PUNCHY)
        display = Tonemap::AgX_Punchy(display);
    else if (g_local.Tonemapper == (int) Tonemapper::AgX_CUSTOM)
        display = Tonemap::AgX_Custom(display, g_local.Saturation, g_local.AgXExp);

    if (g_local.DisplayOption == (int) DisplayOption::DEPTH)
    {
        float z_ndc = z = g_frame.CameraNear / z;
        display = z_ndc;
    }
    else if (g_local.DisplayOption == (int) DisplayOption::NORMAL)
    {
        GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
        float2 encodedNormal = g_normal.SampleLevel(g_samPointClamp, uv, 0);
        display = Math::DecodeUnitVector(encodedNormal.xy);
        display = display * 0.5 + 0.5;
    }
    else if (g_local.DisplayOption == (int) DisplayOption::BASE_COLOR)
    {
        GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::BASE_COLOR];
        display = g_baseColor.SampleLevel(g_samPointClamp, uv, 0).xyz;
    }
    else if (g_local.DisplayOption == (int) DisplayOption::METALNESS_ROUGHNESS)
    {
        GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];
        float2 mr = g_metallicRoughness.SampleLevel(g_samPointClamp, uv, 0);

        bool isMetallic = GBuffer::DecodeMetallic(mr.x);
        mr.x = isMetallic;

        display = float3(mr, 0.0f);
    }
    else if (g_local.DisplayOption == (int) DisplayOption::ROUGHNESS_TH)
    {
        GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];
        float r = g_metallicRoughness.SampleLevel(g_samPointClamp, uv, 0).y;

        display = (r >= g_local.RoughnessTh) * float3(0.26, 0.014, 0.021);
    }
    else if (g_local.DisplayOption == (int) DisplayOption::EMISSIVE)
    {
        GBUFFER_EMISSIVE_COLOR g_emissiveColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::EMISSIVE_COLOR];
        GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::BASE_COLOR];

        GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];
        float m = g_metallicRoughness.SampleLevel(g_samPointClamp, uv, 0).x;
        bool metallic;
        bool emissive;
        bool transmissive;
        GBuffer::DecodeMetallic(m, metallic, transmissive, emissive);

        display = emissive ? g_emissiveColor.SampleLevel(g_samPointClamp, uv, 0).rgb :
            g_baseColor.SampleLevel(g_samPointClamp, uv, 0).xyz * 0.005;
    }
    else if (g_local.DisplayOption == (int) DisplayOption::TRANSMISSION)
    {
        GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::METALLIC_ROUGHNESS];
        float m = g_metallicRoughness.SampleLevel(g_samPointClamp, uv, 0).x;
        bool metallic;
        bool emissive;
        bool transmissive;
        GBuffer::DecodeMetallic(m, metallic, transmissive, emissive);

        display = float3(transmissive, !transmissive, 0);
    }

    return float4(display, 1.0f);
}