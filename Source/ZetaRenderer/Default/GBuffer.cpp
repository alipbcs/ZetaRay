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
        data.SrvDescTable[i] = App::GetRenderer().GetGpuDescriptorHeap().Allocate(
            GBufferData::COUNT);
        data.UavDescTable[i] = App::GetRenderer().GetGpuDescriptorHeap().Allocate(
            GBufferData::COUNT);
    }

    CreateGBuffers(data);

    data.GBufferPass.Init();
}

void GBuffer::CreateGBuffers(GBufferData& data)
{
    auto& renderer = App::GetRenderer();
    const int width = renderer.GetRenderWidth();
    const int height = renderer.GetRenderHeight();

    const auto texFlags = TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS;
    const auto texFlagsDepth = TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS;
    const D3D12_RESOURCE_STATES depthInitState = D3D12_RESOURCE_STATE_COMMON;

    const auto emissiveColFormat = App::GetRenderer().IsRGBESupported() ?
        DXGI_FORMAT_R9G9B9E5_SHAREDEXP :
        DXGI_FORMAT_R11G11B10_FLOAT;

    // Except emissive and motion vector, everything is double-buffered
    constexpr int N = 2 * (GBufferData::COUNT - 2) + 2;
    PlacedResourceList<N> list;

    // Base color
    for (int i = 0; i < 2; i++)
        list.PushTex2D(GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::BASE_COLOR], width, height, texFlags);
    // Normal
    for (int i = 0; i < 2; i++)
        list.PushTex2D(GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::NORMAL], width, height, texFlags);
    // Metallic-roughness
    for (int i = 0; i < 2; i++)
        list.PushTex2D(GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::METALLIC_ROUGHNESS], width, height, texFlags);
    // Motion vector
    list.PushTex2D(GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::MOTION_VECTOR], width, height, texFlags);
    // Emissive color
    list.PushTex2D(emissiveColFormat, width, height, texFlags);
    // IOR
    for (int i = 0; i < 2; i++)
        list.PushTex2D(GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::IOR], width, height, texFlags);
    // Coat
    for (int i = 0; i < 2; i++)
        list.PushTex2D(GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::COAT], width, height, texFlags);
    // Triangle differential geometry - A
    for (int i = 0; i < 2; i++)
        list.PushTex2D(GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::TRI_DIFF_GEO_A], width, height, texFlags);
    // Triangle differential geometry - B
    for (int i = 0; i < 2; i++)
        list.PushTex2D(GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::TRI_DIFF_GEO_B], width, height, texFlags);
    // Depth
    for (int i = 0; i < 2; i++)
        list.PushTex2D(GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::DEPTH], width, height, texFlags);

    list.End();

    data.ResHeap = GpuMemory::GetResourceHeap(list.Size());
    auto allocs = list.AllocInfos();
    int currRes = 0;

    // Base color
    {
        for (int i = 0; i < 2; i++)
        {
            StackStr(name, n, "GBuffer_BaseColor_%d", i);

            data.BaseColor[i] = ZetaMove(GpuMemory::GetPlacedTexture2D(name,
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::BASE_COLOR],
                data.ResHeap.Heap(),
                allocs[currRes++].Offset,
                D3D12_RESOURCE_STATE_COMMON,
                texFlags));

            // UAV
            Direct3DUtil::CreateTexture2DUAV(data.BaseColor[i], data.UavDescTable[i].CPUHandle(
                GBufferData::GBUFFER::BASE_COLOR));
            // SRV
            Direct3DUtil::CreateTexture2DSRV(data.BaseColor[i], data.SrvDescTable[i].CPUHandle(
                GBufferData::GBUFFER::BASE_COLOR));
        }
    }

    // Normal
    {
        for (int i = 0; i < 2; i++)
        {
            StackStr(name, n, "GBuffer_Normal_%d", i);

            data.Normal[i] = ZetaMove(GpuMemory::GetPlacedTexture2D(name,
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::NORMAL],
                data.ResHeap.Heap(),
                allocs[currRes++].Offset,
                D3D12_RESOURCE_STATE_COMMON,
                texFlags));

            // UAV
            Direct3DUtil::CreateTexture2DUAV(data.Normal[i], data.UavDescTable[i].CPUHandle(
                GBufferData::GBUFFER::NORMAL));
            // SRV
            Direct3DUtil::CreateTexture2DSRV(data.Normal[i], data.SrvDescTable[i].CPUHandle(
                GBufferData::GBUFFER::NORMAL));
        }
    }

    // Metallic-roughness
    {
        for (int i = 0; i < 2; i++)
        {
            StackStr(name, n, "GBuffer_MR_%d", i);

            data.MetallicRoughness[i] = ZetaMove(GpuMemory::GetPlacedTexture2D(name,
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::METALLIC_ROUGHNESS],
                data.ResHeap.Heap(),
                allocs[currRes++].Offset,
                D3D12_RESOURCE_STATE_COMMON,
                texFlags));

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
        data.MotionVec = ZetaMove(GpuMemory::GetPlacedTexture2D("GBuffer_MV",
            width, height,
            GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::MOTION_VECTOR],
            data.ResHeap.Heap(),
            allocs[currRes++].Offset,
            D3D12_RESOURCE_STATE_COMMON,
            texFlags));

        //UAV
        Direct3DUtil::CreateTexture2DUAV(data.MotionVec, data.UavDescTable[0].CPUHandle(
            GBufferData::GBUFFER::MOTION_VECTOR));
        Direct3DUtil::CreateTexture2DUAV(data.MotionVec, data.UavDescTable[1].CPUHandle(
            GBufferData::GBUFFER::MOTION_VECTOR));
        // SRV
        Direct3DUtil::CreateTexture2DSRV(data.MotionVec, data.SrvDescTable[0].CPUHandle(
            GBufferData::GBUFFER::MOTION_VECTOR));
        Direct3DUtil::CreateTexture2DSRV(data.MotionVec, data.SrvDescTable[1].CPUHandle(
            GBufferData::GBUFFER::MOTION_VECTOR));
    }

    // Emissive color
    {
        data.EmissiveColor = ZetaMove(GpuMemory::GetPlacedTexture2D("GBuffer_Emissive",
            width, height,
            emissiveColFormat,
            data.ResHeap.Heap(),
            allocs[currRes++].Offset,
            D3D12_RESOURCE_STATE_COMMON,
            texFlags));

        //UAV
        Direct3DUtil::CreateTexture2DUAV(data.EmissiveColor, data.UavDescTable[0].CPUHandle(
            GBufferData::GBUFFER::EMISSIVE_COLOR));
        Direct3DUtil::CreateTexture2DUAV(data.EmissiveColor, data.UavDescTable[1].CPUHandle(
            GBufferData::GBUFFER::EMISSIVE_COLOR));
        // SRV
        Direct3DUtil::CreateTexture2DSRV(data.EmissiveColor, data.SrvDescTable[0].CPUHandle(
            GBufferData::GBUFFER::EMISSIVE_COLOR));
        Direct3DUtil::CreateTexture2DSRV(data.EmissiveColor, data.SrvDescTable[1].CPUHandle(
            GBufferData::GBUFFER::EMISSIVE_COLOR));
    }

    // IOR
    {
        for (int i = 0; i < 2; i++)
        {
            StackStr(name, n, "GBuffer_IOR_%d", i);

            data.IORBuffer[i] = ZetaMove(GpuMemory::GetPlacedTexture2D(name,
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::IOR],
                data.ResHeap.Heap(),
                allocs[currRes++].Offset,
                D3D12_RESOURCE_STATE_COMMON,
                texFlags));

            // UAV
            Direct3DUtil::CreateTexture2DUAV(data.IORBuffer[i], data.UavDescTable[i].CPUHandle(
                GBufferData::GBUFFER::IOR));
            // SRV
            Direct3DUtil::CreateTexture2DSRV(data.IORBuffer[i], data.SrvDescTable[i].CPUHandle(
                GBufferData::GBUFFER::IOR));
        }
    }

    // Coat
    {
        for (int i = 0; i < 2; i++)
        {
            StackStr(name, n, "GBuffer_Coat_%d", i);

            data.CoatBuffer[i] = ZetaMove(GpuMemory::GetPlacedTexture2D(name,
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::COAT],
                data.ResHeap.Heap(),
                allocs[currRes++].Offset,
                D3D12_RESOURCE_STATE_COMMON,
                texFlags));

            // UAV
            Direct3DUtil::CreateTexture2DUAV(data.CoatBuffer[i], data.UavDescTable[i].CPUHandle(
                GBufferData::GBUFFER::COAT));
            // SRV
            Direct3DUtil::CreateTexture2DSRV(data.CoatBuffer[i], data.SrvDescTable[i].CPUHandle(
                GBufferData::GBUFFER::COAT));
        }
    }

    // Triangle differential geometry
    {
        for (int i = 0; i < 2; i++)
        {
            StackStr(nameA, nA, "TriDiffGeoA_%d", i);

            data.TriDiffGeo_A[i] = ZetaMove(GpuMemory::GetPlacedTexture2D(nameA,
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::TRI_DIFF_GEO_A],
                data.ResHeap.Heap(),
                allocs[currRes++].Offset,
                D3D12_RESOURCE_STATE_COMMON,
                texFlags));

            // UAV
            Direct3DUtil::CreateTexture2DUAV(data.TriDiffGeo_A[i],
                data.UavDescTable[i].CPUHandle(GBufferData::GBUFFER::TRI_DIFF_GEO_A));
            // SRV
            Direct3DUtil::CreateTexture2DSRV(data.TriDiffGeo_A[i],
                data.SrvDescTable[i].CPUHandle(GBufferData::GBUFFER::TRI_DIFF_GEO_A));
        }

        for (int i = 0; i < 2; i++)
        {
            StackStr(nameA, nA, "TriDiffGeoB_%d", i);

            data.TriDiffGeo_B[i] = ZetaMove(GpuMemory::GetPlacedTexture2D(nameA,
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::TRI_DIFF_GEO_B],
                data.ResHeap.Heap(),
                allocs[currRes++].Offset,
                D3D12_RESOURCE_STATE_COMMON,
                texFlags));

            // UAV
            Direct3DUtil::CreateTexture2DUAV(data.TriDiffGeo_B[i],
                data.UavDescTable[i].CPUHandle(GBufferData::GBUFFER::TRI_DIFF_GEO_B));
            // SRV
            Direct3DUtil::CreateTexture2DSRV(data.TriDiffGeo_B[i],
                data.SrvDescTable[i].CPUHandle(GBufferData::GBUFFER::TRI_DIFF_GEO_B));
        }
    }

    // Depth
    {
        for (int i = 0; i < 2; i++)
        {
            StackStr(name, n, "Depth_%d", i);

            data.Depth[i] = ZetaMove(GpuMemory::GetPlacedTexture2D(name,
                width, height,
                GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::DEPTH],
                data.ResHeap.Heap(),
                allocs[currRes++].Offset,
                depthInitState,
                texFlagsDepth));

            // UAV
            Direct3DUtil::CreateTexture2DUAV(data.Depth[i], data.UavDescTable[i].CPUHandle(
                GBufferData::GBUFFER::DEPTH));
            // SRV
            Direct3DUtil::CreateTexture2DSRV(data.Depth[i], data.SrvDescTable[i].CPUHandle(
                GBufferData::GBUFFER::DEPTH),
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
    const int outIdx = App::GetRenderer().GlobalIdxForDoubleBufferedResources();

    gbufferData.GBufferPass.SetGBufferUavDescTableGpuHeapIdx(
        gbufferData.UavDescTable[outIdx].GPUDescriptorHeapIndex(GBufferData::GBUFFER::BASE_COLOR));
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
        renderGraph.RegisterResource(data.Depth[i].Resource(), data.Depth[i].ID(), initDepthState);
        renderGraph.RegisterResource(data.MetallicRoughness[i].Resource(), data.MetallicRoughness[i].ID());
        renderGraph.RegisterResource(data.BaseColor[i].Resource(), data.BaseColor[i].ID());
        renderGraph.RegisterResource(data.IORBuffer[i].Resource(), data.IORBuffer[i].ID());
        renderGraph.RegisterResource(data.CoatBuffer[i].Resource(), data.CoatBuffer[i].ID());
        renderGraph.RegisterResource(data.TriDiffGeo_A[i].Resource(), data.TriDiffGeo_A[i].ID());
        renderGraph.RegisterResource(data.TriDiffGeo_B[i].Resource(), data.TriDiffGeo_B[i].ID());
    }

    renderGraph.RegisterResource(data.MotionVec.Resource(), data.MotionVec.ID());
    renderGraph.RegisterResource(data.EmissiveColor.Resource(), data.EmissiveColor.ID());
}

void GBuffer::AddAdjacencies(GBufferData& data, const RayTracerData& rayTracerData,
    RenderGraph& renderGraph)
{
    const int outIdx = App::GetRenderer().GlobalIdxForDoubleBufferedResources();

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
    renderGraph.AddOutput(data.GBufferPassHandle, data.IORBuffer[outIdx].ID(), gbufferOutState);
    renderGraph.AddOutput(data.GBufferPassHandle, data.CoatBuffer[outIdx].ID(), gbufferOutState);
    renderGraph.AddOutput(data.GBufferPassHandle, data.Depth[outIdx].ID(), depthBuffOutState);
}
