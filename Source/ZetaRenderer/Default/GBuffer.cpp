#include "DefaultRendererImpl.h"
#include <Core/CommandList.h>
#include <App/App.h>

using namespace ZetaRay::Math;
using namespace ZetaRay::Model;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Util;
using namespace ZetaRay::DefaultRenderer;

//--------------------------------------------------------------------------------------
// GBuffer
//--------------------------------------------------------------------------------------

void GBuffer::Init(const RenderSettings& settings, GBufferData& data)
{
    for (int i = 0; i < 2; i++)
    {
        data.SrvDescTable[i] = App::GetRenderer().GetGpuDescriptorHeap().Allocate(GBufferData::COUNT);
        data.UavDescTable[i] = App::GetRenderer().GetGpuDescriptorHeap().Allocate(GBufferData::COUNT);
    }

    CreateGBuffers(data);

    data.GBufferPass.Init();
}

void GBuffer::CreateGBuffers(GBufferData& data)
{
    auto* device = App::GetRenderer().GetDevice();
    const int width = App::GetRenderer().GetRenderWidth();
    const int height = App::GetRenderer().GetRenderHeight();

    const auto texFlags = TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS;
    const auto texFlagsDepth = TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS;

    D3D12_CLEAR_VALUE* clearValuePtr = nullptr;
    D3D12_CLEAR_VALUE* clearValuePtrDepth = nullptr;

    const D3D12_RESOURCE_STATES depthInitState = D3D12_RESOURCE_STATE_COMMON;

    // Base color
    {
        for (int i = 0; i < 2; i++)
        {
            StackStr(name, n, "GBuffer_BaseColor_%d", i);

            data.BaseColor[i] = ZetaMove(GpuMemory::GetTexture2D(name,
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::BASE_COLOR],
                D3D12_RESOURCE_STATE_COMMON,
                texFlags,
                1,
                clearValuePtr));

            // UAV
            Direct3DUtil::CreateTexture2DUAV(data.BaseColor[i], data.UavDescTable[i].CPUHandle(GBufferData::GBUFFER::BASE_COLOR));
            // SRV
            Direct3DUtil::CreateTexture2DSRV(data.BaseColor[i], data.SrvDescTable[i].CPUHandle(GBufferData::GBUFFER::BASE_COLOR));
        }
    }

    // Normal
    {
        for (int i = 0; i < 2; i++)
        {
            StackStr(name, n, "GBuffer_Normal_%d", i);

            data.Normal[i] = ZetaMove(GpuMemory::GetTexture2D(name, 
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::NORMAL],
                D3D12_RESOURCE_STATE_COMMON,
                texFlags,
                1,
                clearValuePtr));

            // UAV
            Direct3DUtil::CreateTexture2DUAV(data.Normal[i], data.UavDescTable[i].CPUHandle(GBufferData::GBUFFER::NORMAL));
            // SRV
            Direct3DUtil::CreateTexture2DSRV(data.Normal[i], data.SrvDescTable[i].CPUHandle(GBufferData::GBUFFER::NORMAL));
        }
    }

    // Metallic-roughness
    {
        for (int i = 0; i < 2; i++)
        {
            StackStr(name, n, "GBuffer_MR_%d", i);

            data.MetallicRoughness[i] = ZetaMove(GpuMemory::GetTexture2D(name, 
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::METALLIC_ROUGHNESS],
                D3D12_RESOURCE_STATE_COMMON,
                texFlags,
                1,
                clearValuePtr));

            // UAV
            Direct3DUtil::CreateTexture2DUAV(data.MetallicRoughness[i], data.UavDescTable[i].CPUHandle(
                GBufferData::GBUFFER::METALLIC_ROUGHNESS));
            // SRV
            Direct3DUtil::CreateTexture2DSRV(data.MetallicRoughness[i], data.SrvDescTable[i].CPUHandle(
                GBufferData::GBUFFER::METALLIC_ROUGHNESS));
        }
    }

    // Motion vector
    {
        data.MotionVec = ZetaMove(GpuMemory::GetTexture2D("GBuffer_MV", 
            width, height,
            GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::MOTION_VECTOR],
            D3D12_RESOURCE_STATE_COMMON,
            texFlags,
            1,
            clearValuePtr));

        //UAV
        Direct3DUtil::CreateTexture2DUAV(data.MotionVec, data.UavDescTable[0].CPUHandle(GBufferData::GBUFFER::MOTION_VECTOR));
        Direct3DUtil::CreateTexture2DUAV(data.MotionVec, data.UavDescTable[1].CPUHandle(GBufferData::GBUFFER::MOTION_VECTOR));
        // SRV
        Direct3DUtil::CreateTexture2DSRV(data.MotionVec, data.SrvDescTable[0].CPUHandle(GBufferData::GBUFFER::MOTION_VECTOR));
        Direct3DUtil::CreateTexture2DSRV(data.MotionVec, data.SrvDescTable[1].CPUHandle(GBufferData::GBUFFER::MOTION_VECTOR));
    }

    // Emissive color
    {
        const auto format = App::GetRenderer().IsRGBESupported() ?
            DXGI_FORMAT_R9G9B9E5_SHAREDEXP :
            DXGI_FORMAT_R11G11B10_FLOAT;

        data.EmissiveColor = ZetaMove(GpuMemory::GetTexture2D("GBuffer_Emissive", 
            width, height,
            format,
            D3D12_RESOURCE_STATE_COMMON,
            texFlags,
            1,
            clearValuePtr));

        //UAV
        Direct3DUtil::CreateTexture2DUAV(data.EmissiveColor, data.UavDescTable[0].CPUHandle(GBufferData::GBUFFER::EMISSIVE_COLOR));
        Direct3DUtil::CreateTexture2DUAV(data.EmissiveColor, data.UavDescTable[1].CPUHandle(GBufferData::GBUFFER::EMISSIVE_COLOR));
        // SRV
        Direct3DUtil::CreateTexture2DSRV(data.EmissiveColor, data.SrvDescTable[0].CPUHandle(GBufferData::GBUFFER::EMISSIVE_COLOR));
        Direct3DUtil::CreateTexture2DSRV(data.EmissiveColor, data.SrvDescTable[1].CPUHandle(GBufferData::GBUFFER::EMISSIVE_COLOR));
    }

    // Transmission
    {
        for (int i = 0; i < 2; i++)
        {
            StackStr(name, n, "GBuffer_TR_%d", i);

            data.Transmission[i] = ZetaMove(GpuMemory::GetTexture2D(name,
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::TRANSMISSION],
                D3D12_RESOURCE_STATE_COMMON,
                texFlags,
                1,
                clearValuePtr));

            // UAV
            Direct3DUtil::CreateTexture2DUAV(data.Transmission[i], data.UavDescTable[i].CPUHandle(
                GBufferData::GBUFFER::TRANSMISSION));
            // SRV
            Direct3DUtil::CreateTexture2DSRV(data.Transmission[i], data.SrvDescTable[i].CPUHandle(
                GBufferData::GBUFFER::TRANSMISSION));
        }    
    }

    // Depth
    {
        for (int i = 0; i < 2; i++)
        {
            StackStr(name, n, "Depth_%d", i);

            data.DepthBuffer[i] = ZetaMove(GpuMemory::GetTexture2D(name, 
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::DEPTH],
                depthInitState,
                texFlagsDepth,
                1,
                clearValuePtrDepth));

            // UAV
            Direct3DUtil::CreateTexture2DUAV(data.DepthBuffer[i], data.UavDescTable[i].CPUHandle(GBufferData::GBUFFER::DEPTH));
            // SRV
            Direct3DUtil::CreateTexture2DSRV(data.DepthBuffer[i], data.SrvDescTable[i].CPUHandle(GBufferData::GBUFFER::DEPTH), 
                DXGI_FORMAT_R32_FLOAT);
        }
    }
}

void GBuffer::OnWindowSizeChanged(const RenderSettings& settings, GBufferData& data)
{
    GBuffer::CreateGBuffers(data);
}

void GBuffer::Update(GBufferData& gbufferData)
{
    const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

    gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::BASE_COLOR_UAV,
        gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::BASE_COLOR));
    gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::NORMAL_UAV,
        gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::NORMAL));
    gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::METALLIC_ROUGHNESS_UAV,
        gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::METALLIC_ROUGHNESS));
    gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::MOTION_VECTOR_UAV,
        gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::MOTION_VECTOR));
    gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::EMISSIVE_COLOR_UAV,
        gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::EMISSIVE_COLOR));
    gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::TRANSMISSION_UAV,
        gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::TRANSMISSION));
    gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::DEPTH_UAV,
        gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::DEPTH));
}

void GBuffer::Register(GBufferData& data, const RayTracerData& rayTracerData, RenderGraph& renderGraph)
{
    const bool tlasReady = rayTracerData.RtAS.IsReady();
    if (!tlasReady)
        return;

    // GBuffer
    fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.GBufferPass, &GBufferRT::Render);
    data.GBufferPassHandle = renderGraph.RegisterRenderPass("GBuffer", RENDER_NODE_TYPE::COMPUTE, dlg);

    const D3D12_RESOURCE_STATES initDepthState = D3D12_RESOURCE_STATE_COMMON;

    // Register current and previous frame's g-buffers
    for (int i = 0; i < 2; i++)
    {
        renderGraph.RegisterResource(data.Normal[i].Resource(), data.Normal[i].ID());
        renderGraph.RegisterResource(data.DepthBuffer[i].Resource(), data.DepthBuffer[i].ID(), initDepthState);
        renderGraph.RegisterResource(data.MetallicRoughness[i].Resource(), data.MetallicRoughness[i].ID());
        renderGraph.RegisterResource(data.BaseColor[i].Resource(), data.BaseColor[i].ID());
        renderGraph.RegisterResource(data.Transmission[i].Resource(), data.Transmission[i].ID());
    }

    renderGraph.RegisterResource(data.MotionVec.Resource(), data.MotionVec.ID());
    renderGraph.RegisterResource(data.EmissiveColor.Resource(), data.EmissiveColor.ID());
}

void GBuffer::AddAdjacencies(GBufferData& data, const RayTracerData& rayTracerData, 
    RenderGraph& renderGraph)
{
    const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

    const bool tlasReady = rayTracerData.RtAS.IsReady();
    if (!tlasReady)
        return;

    const D3D12_RESOURCE_STATES gbufferOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const D3D12_RESOURCE_STATES depthBuffOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    renderGraph.AddInput(data.GBufferPassHandle,
        rayTracerData.RtAS.GetTLAS().ID(),
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

    renderGraph.AddOutput(data.GBufferPassHandle, data.BaseColor[outIdx].ID(), gbufferOutState);
    renderGraph.AddOutput(data.GBufferPassHandle, data.Normal[outIdx].ID(), gbufferOutState);
    renderGraph.AddOutput(data.GBufferPassHandle, data.MetallicRoughness[outIdx].ID(), gbufferOutState);
    renderGraph.AddOutput(data.GBufferPassHandle, data.MotionVec.ID(), gbufferOutState);
    renderGraph.AddOutput(data.GBufferPassHandle, data.EmissiveColor.ID(), gbufferOutState);
    renderGraph.AddOutput(data.GBufferPassHandle, data.Transmission[outIdx].ID(), gbufferOutState);
    renderGraph.AddOutput(data.GBufferPassHandle, data.DepthBuffer[outIdx].ID(), depthBuffOutState);
}
