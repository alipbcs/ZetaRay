#include "Compositing_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/LightVoxelGrid.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbCompositing> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Helper Functions
//--------------------------------------------------------------------------------------

float3 MapIdxToColor(uint i) 
{
    float r = (i & 0xff) / 255.0f;
    float g = ((i >> 8) & 0xff) / 255.0f;
    float b = ((i >> 16) & 0xff) / 255.0f;

    return float3(r, g, b);
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(COMPOSITING_THREAD_GROUP_DIM_X, COMPOSITING_THREAD_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint Gidx : SV_GroupIndex)
{
    if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
        return;

    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    GBuffer::Flags flags = GBuffer::DecodeMetallic(g_metallicRoughness[DTid.xy].x);

    RWTexture2D<float4> g_composited = ResourceDescriptorHeap[g_local.OutputUAVDescHeapIdx];
    const bool accumulate = g_frame.Accumulate && g_frame.CameraStatic;
    
    if (flags.invalid && !accumulate)
    {
        const bool dirLighting = IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::SKY_DI) || IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::EMISSIVE_DI);
        g_composited[DTid.xy].xyz = dirLighting ? Light::Le_SkyWithSunDisk(DTid.xy, g_frame) : 0;
        return;
    }

    const uint numFramesAccumulated = accumulate ? g_frame.NumFramesCameraStatic : 1;
    float3 color = 0; 

    if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::SKY_DI) && g_local.SkyDIDescHeapIdx != 0)
    {
        Texture2D<float4> g_sky = ResourceDescriptorHeap[g_local.SkyDIDescHeapIdx];
        color = g_sky[DTid.xy].rgb;
    }
    else if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::EMISSIVE_DI) && g_local.EmissiveDIDescHeapIdx != 0)
    {
        Texture2D<float4> g_emissive = ResourceDescriptorHeap[g_local.EmissiveDIDescHeapIdx];
        float3 le = g_emissive[DTid.xy].rgb;
        color += le;
    }

    if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::INDIRECT) && !flags.emissive && g_local.IndirectDescHeapIdx != 0)
    {
        Texture2D<float4> g_indirect = ResourceDescriptorHeap[g_local.IndirectDescHeapIdx];
        float3 li = g_indirect[DTid.xy].rgb;
        color += li;
    }

    color /= numFramesAccumulated;

    if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::INSCATTERING))
    {
        GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::DEPTH];
        const float z_view = g_depth[DTid.xy];

        if (z_view > 1e-4f)
        {
            float2 posTS = (DTid.xy + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);
            float p = pow(max(z_view - g_local.VoxelGridNearZ, 0.0f) / 
                (g_local.VoxelGridFarZ - g_local.VoxelGridNearZ), 1.0f / g_local.DepthMappingExp);
        
            //float p = linearDepth / g_local.VoxelGridDepth;
            //p /= exp(-1.0 + p);
        
            Texture3D<half4> g_voxelGrid = ResourceDescriptorHeap[g_local.InscatteringDescHeapIdx];
            float3 posCube = float3(posTS, p);
            //float3 uvw = float3(posCube.xy, p);
            //const float3 uvw = float3(posTS, 12 / 32.0);
        
            half3 inscattering = g_voxelGrid.SampleLevel(g_samLinearClamp, posCube, 0.0f).rgb;
            color += inscattering;
        }
    }

    if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::VISUALIZE_LVG))
    {
        // int3 voxelIdx;
        // color = 0;

        // GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        //     GBUFFER_OFFSET::DEPTH];
        // const float z_view = g_depth[DTid.xy];
        // float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
        // const float3 pos = Math::WorldPosFromScreenSpace(DTid.xy, renderDim,
        //     z_view, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv, 
        //     g_frame.CurrCameraJitter);

        // if(LVG::MapPosToVoxel(pos, uint3(g_local.GridDim_x, g_local.GridDim_y, g_local.GridDim_z), 
        //     float3(g_local.Extents_x, g_local.Extents_y, g_local.Extents_z), g_frame.CurrView, 
        //     voxelIdx, g_local.Offset_y))
        // {
        //     GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        //         GBUFFER_OFFSET::BASE_COLOR];
        //     float3 baseColor = g_baseColor[DTid.xy].rgb;

        //     uint3 c = RNG::PCG3d(voxelIdx);
        //     color = baseColor * 0.1 + MapIdxToColor(c.x) * 0.05;
        // }
    }

    g_composited[DTid.xy].xyz = color;
}