#include "DefaultRendererImpl.h"
#include <App/App.h>
#include <RayTracing/RtAccelerationStructure.h>

using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::DefaultRenderer;
using namespace ZetaRay::Util;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;

//--------------------------------------------------------------------------------------
// PostProcessor
//--------------------------------------------------------------------------------------

void PostProcessor::Init(const RenderSettings& settings, PostProcessData& data)
{
    data.AutoExposurePass.Init();
    data.DisplayPass.Init();
    data.GuiPass.Init();
    data.CompositingPass.Init();

    UpdateWndDependentDescriptors(settings, data);
}

void PostProcessor::UpdateWndDependentDescriptors(const RenderSettings& settings, 
    PostProcessData& data)
{
    data.WindowSizeConstSRVs = App::GetRenderer().GetGpuDescriptorHeap().Allocate(
        (int)PostProcessData::DESC_TABLE_CONST::COUNT);

    Direct3DUtil::CreateTexture2DSRV(data.AutoExposurePass.GetOutput(
        AutoExposure::SHADER_OUT_RES::EXPOSURE),
        data.WindowSizeConstSRVs.CPUHandle((int)PostProcessData::DESC_TABLE_CONST::EXPOSURE_SRV));

    const Texture& lightAccum = data.CompositingPass.GetOutput(
        Compositing::SHADER_OUT_RES::COMPOSITED);
    Direct3DUtil::CreateTexture2DSRV(lightAccum,
        data.WindowSizeConstSRVs.CPUHandle(
            (int)PostProcessData::DESC_TABLE_CONST::HDR_LIGHT_ACCUM_SRV));
}

void PostProcessor::UpdateFrameDescriptors(const RenderSettings& settings, PostProcessData& data)
{
    if (settings.AntiAliasing == AA::TAA)
    {
        const int outIdx = App::GetRenderer().GlobalIdxForDoubleBufferedResources();

        data.TaaOrFsr2OutSRV = App::GetRenderer().GetGpuDescriptorHeap().Allocate(1);

        // Due to ping-ponging, TAA's output texture changes every frame
        const TAA::SHADER_OUT_RES taaOutIdx = outIdx == 0 ? TAA::SHADER_OUT_RES::OUTPUT_B : 
            TAA::SHADER_OUT_RES::OUTPUT_A;
        Texture& taaOut = data.TaaPass.GetOutput(taaOutIdx);
        Direct3DUtil::CreateTexture2DSRV(taaOut, data.TaaOrFsr2OutSRV.CPUHandle(0));
    }
}

void PostProcessor::UpdatePasses(const RenderSettings& settings, PostProcessData& data)
{
    if (settings.AntiAliasing != AA::FSR2 && data.Fsr2Pass.IsInitialized())
        data.Fsr2Pass.Reset();

    if (settings.AntiAliasing != AA::TAA && data.TaaPass.IsInitialized())
        data.TaaPass.Reset();

    if (settings.AntiAliasing == AA::TAA && !data.TaaPass.IsInitialized())
        data.TaaPass.Init();
    else if (settings.AntiAliasing == AA::FSR2 && !data.Fsr2Pass.IsInitialized())
    {
        data.Fsr2Pass.Activate();

        data.TaaOrFsr2OutSRV = App::GetRenderer().GetGpuDescriptorHeap().Allocate(1);

        const Texture& upscaled = data.Fsr2Pass.GetOutput(FSR2Pass::SHADER_OUT_RES::UPSCALED);
        Direct3DUtil::CreateTexture2DSRV(upscaled, data.TaaOrFsr2OutSRV.CPUHandle(0));
    }
}

void PostProcessor::OnWindowSizeChanged(const RenderSettings& settings, PostProcessData& data,
    const RayTracerData& rtData)
{
    data.CompositingPass.OnWindowResized();

    if (settings.AntiAliasing == AA::TAA)
        data.TaaPass.OnWindowResized();
    else if (settings.AntiAliasing == AA::FSR2)
        data.Fsr2Pass.OnWindowResized();

    UpdateWndDependentDescriptors(settings, data);
}

void PostProcessor::Update(const RenderSettings& settings, PostProcessData& data, 
    const GBufferData& gbuffData, const RayTracerData& rtData)
{
    UpdatePasses(settings, data);
    UpdateFrameDescriptors(settings, data);

    const int outIdx = App::GetRenderer().GlobalIdxForDoubleBufferedResources();
    const auto compositedSrv = PostProcessData::DESC_TABLE_CONST::HDR_LIGHT_ACCUM_SRV;

    if (rtData.RtAS.IsReady())
    {
        // Emissive DI
        if (App::GetScene().NumEmissiveInstances())
        {
            data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::EMISSIVE_DI,
                rtData.WndConstDescTable.GPUDescriptorHeapIndex(
                    (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::EMISSIVE_DI));
        }
        // Sky DI
        else
        {
            data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::SKY_DI,
                rtData.WndConstDescTable.GPUDescriptorHeapIndex(
                    (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SKY_DI));
        }

        // Indirect lighting
        data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::INDIRECT,
            rtData.WndConstDescTable.GPUDescriptorHeapIndex(
                (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::INDIRECT));

        if (settings.Inscattering)
        {
            data.CompositingPass.SetInscatteringEnablement(true);

            const float p = rtData.SkyPass.GetVoxelGridMappingExp();
            float2 depths = rtData.SkyPass.GetVoxelGridDepth();

            data.CompositingPass.SetVoxelGridMappingExp(p);
            data.CompositingPass.SetVoxelGridDepth(depths.x, depths.y);
            data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::INSCATTERING,
                rtData.ConstDescTable.GPUDescriptorHeapIndex((int)RayTracerData::DESC_TABLE_CONST::INSCATTERING_SRV));
        }
        else
            data.CompositingPass.SetInscatteringEnablement(false);
    }

    // Display
    auto backBuffRTV = App::GetRenderer().GetCurrBackBufferRTV();
    data.DisplayPass.SetCpuDescriptor(DisplayPass::SHADER_IN_CPU_DESC::RTV, backBuffRTV);

    const Texture& exposureTex = data.AutoExposurePass.GetOutput(
        AutoExposure::SHADER_OUT_RES::EXPOSURE);
    data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::EXPOSURE,
        data.WindowSizeConstSRVs.GPUDescriptorHeapIndex(
            (int)PostProcessData::DESC_TABLE_CONST::EXPOSURE_SRV));

    data.GuiPass.SetCPUDescriptor(GuiPass::SHADER_IN_CPU_DESC::RTV, backBuffRTV);

    // Auto Exposure
    data.AutoExposurePass.SetDescriptor(AutoExposure::SHADER_IN_DESC::COMPOSITED,
        data.WindowSizeConstSRVs.GPUDescriptorHeapIndex((int)compositedSrv));

    // TAA
    if (settings.AntiAliasing == AA::TAA)
    {
        data.TaaPass.SetDescriptor(TAA::SHADER_IN_DESC::SIGNAL,
            data.WindowSizeConstSRVs.GPUDescriptorHeapIndex((int)compositedSrv));

        // Display
        data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::COMPOSITED, 
            data.TaaOrFsr2OutSRV.GPUDescriptorHeapIndex(0));
    }
    // FSR2
    else if (settings.AntiAliasing == AA::FSR2)
    {
        Texture& composited = const_cast<Texture&>(data.CompositingPass.GetOutput(
            Compositing::SHADER_OUT_RES::COMPOSITED));

        data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::DEPTH, 
            const_cast<Texture&>(gbuffData.Depth[outIdx]).Resource());
        data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::MOTION_VECTOR, 
            const_cast<Texture&>(gbuffData.MotionVec).Resource());
        data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::COLOR, composited.Resource());
        data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::EXPOSURE, 
            const_cast<Texture&>(exposureTex).Resource());

        // Display
        data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::COMPOSITED, 
            data.TaaOrFsr2OutSRV.GPUDescriptorHeapIndex(0));
    }
    else
    {
        // Display
        data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::COMPOSITED,
            data.WindowSizeConstSRVs.GPUDescriptorHeapIndex((int)compositedSrv));
    }
}

void PostProcessor::Register(const RenderSettings& settings, PostProcessData& data, 
    GBufferData& gbufferData, RenderGraph& renderGraph)
{
    // Compositing
    {
        Texture& lightAccum = const_cast<Texture&>(data.CompositingPass.GetOutput(
            Compositing::SHADER_OUT_RES::COMPOSITED));
        renderGraph.RegisterResource(lightAccum.Resource(), lightAccum.ID());

        fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.CompositingPass,
            &Compositing::Render);
        data.CompositingHandle = renderGraph.RegisterRenderPass("Compositing", 
            RENDER_NODE_TYPE::COMPUTE, dlg);
    }

    // TAA
    if (settings.AntiAliasing == AA::TAA)
    {
        fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.TaaPass,
            &TAA::Render);

        data.TaaHandle = renderGraph.RegisterRenderPass("TAA", RENDER_NODE_TYPE::COMPUTE, dlg);

        Texture& taaA = data.TaaPass.GetOutput(TAA::SHADER_OUT_RES::OUTPUT_A);
        renderGraph.RegisterResource(taaA.Resource(), taaA.ID());

        Texture& taaB = data.TaaPass.GetOutput(TAA::SHADER_OUT_RES::OUTPUT_B);
        renderGraph.RegisterResource(taaB.Resource(), taaB.ID());
    }
    // FSR2
    else if (settings.AntiAliasing == AA::FSR2)
    {
        fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.Fsr2Pass,
            &FSR2Pass::Render);

        data.Fsr2Handle = renderGraph.RegisterRenderPass("FSR2", RENDER_NODE_TYPE::COMPUTE, dlg);

        const Texture& upscaled = data.Fsr2Pass.GetOutput(FSR2Pass::SHADER_OUT_RES::UPSCALED);
        renderGraph.RegisterResource(const_cast<Texture&>(upscaled).Resource(), upscaled.ID());
    }

    // Auto Exposure
    {
        fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(
            &data.AutoExposurePass, &AutoExposure::Render);

        data.AutoExposureHandle = renderGraph.RegisterRenderPass("AutoExposure", 
            RENDER_NODE_TYPE::COMPUTE, dlg);

        Texture& exposureTex = data.AutoExposurePass.GetOutput(AutoExposure::SHADER_OUT_RES::EXPOSURE);
        renderGraph.RegisterResource(exposureTex.Resource(), exposureTex.ID(), 
            D3D12_RESOURCE_STATE_COMMON, false);
    }

    // Display
    {
        fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.DisplayPass, 
            &DisplayPass::Render);
        data.DisplayHandle = renderGraph.RegisterRenderPass("DisplayPass", RENDER_NODE_TYPE::RENDER, dlg);

        // When there's a pending pick in this frame, DisplayPass::Render() will call the delegate
        // below to clear it later in the same frame
        if (gbufferData.GBufferPass.HasPendingPick())
        {
            auto pickDlg = fastdelegate::MakeDelegate(&gbufferData.GBufferPass, &GBufferRT::ClearPick);
            auto& readback = gbufferData.GBufferPass.GetPickReadbackBuffer();

            data.DisplayPass.SetPickData(gbufferData.GBufferPassHandle, &readback, pickDlg);
        }
    }

    // ImGui
    {
        fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.GuiPass, 
            &GuiPass::Render);
        data.GuiHandle = renderGraph.RegisterRenderPass("GuiPass", RENDER_NODE_TYPE::RENDER, dlg);
    }

    // Register backbuffer
    const Texture& backbuff = App::GetRenderer().GetCurrBackBuffer();
    renderGraph.RegisterResource(const_cast<Texture&>(backbuff).Resource(), backbuff.ID());

    // Dummy resource
    renderGraph.RegisterResource(nullptr, RenderGraph::DUMMY_RES::RES_1);
}

void PostProcessor::AddAdjacencies(const RenderSettings& settings, PostProcessData& data, 
    const GBufferData& gbuffData, const RayTracerData& rtData, RenderGraph& renderGraph)
{
    const Texture& composited = data.CompositingPass.GetOutput(
        Compositing::SHADER_OUT_RES::COMPOSITED);
    const Texture& exposureTex = data.AutoExposurePass.GetOutput(
        AutoExposure::SHADER_OUT_RES::EXPOSURE);
    const bool tlasReady = rtData.RtAS.IsReady();
    const int outIdx = App::GetRenderer().GlobalIdxForDoubleBufferedResources();

    // Compositing
    if (tlasReady)
    {
        // G-buffers
        renderGraph.AddInput(data.CompositingHandle,
            gbuffData.BaseColor[outIdx].ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        renderGraph.AddInput(data.CompositingHandle,
            gbuffData.Normal[outIdx].ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        renderGraph.AddInput(data.CompositingHandle,
            gbuffData.Depth[outIdx].ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        renderGraph.AddInput(data.CompositingHandle,
            gbuffData.MetallicRoughness[outIdx].ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        renderGraph.AddInput(data.CompositingHandle,
            gbuffData.IORBuffer[outIdx].ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        renderGraph.AddInput(data.CompositingHandle,
            gbuffData.CoatBuffer[outIdx].ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        if (tlasReady)
        {
            // Emissive DI
            if (App::GetScene().NumEmissiveInstances())
            {
                renderGraph.AddInput(data.CompositingHandle,
                    rtData.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED).ID(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            // Sky DI
            else
            {
                renderGraph.AddInput(data.CompositingHandle,
                    rtData.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED).ID(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }

            // Indirect lighting
            renderGraph.AddInput(data.CompositingHandle,
                rtData.IndirecLightingPass.GetOutput(IndirectLighting::SHADER_OUT_RES::DENOISED).ID(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            // Inscattering
            if (settings.Inscattering)
            {
                renderGraph.AddInput(data.CompositingHandle,
                    rtData.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING).ID(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        }
    }

    renderGraph.AddOutput(data.CompositingHandle,
        data.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED).ID(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // TAA
    if (tlasReady)
    {
        if (settings.AntiAliasing == AA::TAA)
        {
            const TAA::SHADER_OUT_RES taaCurrOutIdx = outIdx == 0 ? TAA::SHADER_OUT_RES::OUTPUT_B : 
                TAA::SHADER_OUT_RES::OUTPUT_A;
            const TAA::SHADER_OUT_RES taaPrevOutIdx = outIdx == 0 ? TAA::SHADER_OUT_RES::OUTPUT_A : 
                TAA::SHADER_OUT_RES::OUTPUT_B;
            Texture& taaCurrOut = data.TaaPass.GetOutput(taaCurrOutIdx);
            Texture& taaPrevOut = data.TaaPass.GetOutput(taaPrevOutIdx);

            renderGraph.AddInput(data.TaaHandle,
                gbuffData.Depth[outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(data.TaaHandle,
                composited.ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(data.TaaHandle,
                taaPrevOut.ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddOutput(data.TaaHandle,
                taaCurrOut.ID(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            // Display
            renderGraph.AddInput(data.DisplayHandle,
                taaCurrOut.ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
        // FSR2
        else if (settings.AntiAliasing == AA::FSR2)
        {
            renderGraph.AddInput(data.Fsr2Handle,
                gbuffData.Depth[outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(data.Fsr2Handle,
                composited.ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(data.Fsr2Handle,
                gbuffData.MotionVec.ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(data.Fsr2Handle,
                exposureTex.ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            const Texture& upscaled = data.Fsr2Pass.GetOutput(FSR2Pass::SHADER_OUT_RES::UPSCALED);
            Assert(upscaled.IsInitialized(), "Upscaled output hasn't been initialized.");

            renderGraph.AddOutput(data.Fsr2Handle,
                upscaled.ID(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            // Display
            renderGraph.AddInput(data.DisplayHandle,
                upscaled.ID(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    // Auto Exposure
    {
        renderGraph.AddInput(data.AutoExposureHandle,
            composited.ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        renderGraph.AddOutput(data.AutoExposureHandle,
            exposureTex.ID(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Display
    if (tlasReady)
    {
        renderGraph.AddInput(data.DisplayHandle,
            gbuffData.Depth[outIdx].ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        renderGraph.AddInput(data.DisplayHandle,
            gbuffData.BaseColor[outIdx].ID(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        renderGraph.AddInput(data.DisplayHandle,
            gbuffData.Normal[outIdx].ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        renderGraph.AddInput(data.DisplayHandle,
            gbuffData.MetallicRoughness[outIdx].ID(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        renderGraph.AddInput(data.DisplayHandle,
            gbuffData.CoatBuffer[outIdx].ID(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        renderGraph.AddInput(data.DisplayHandle,
            gbuffData.EmissiveColor.ID(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    renderGraph.AddInput(data.DisplayHandle,
        exposureTex.ID(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Backbuffer
    renderGraph.AddOutput(data.DisplayHandle,
        App::GetRenderer().GetCurrBackBuffer().ID(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    // For GUI Pass
    renderGraph.AddOutput(data.DisplayHandle,
        RenderGraph::DUMMY_RES::RES_1,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Due to blending, ImGui should go last
    renderGraph.AddInput(data.GuiHandle,
        RenderGraph::DUMMY_RES::RES_1,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    renderGraph.AddOutput(data.GuiHandle,
        App::GetRenderer().GetCurrBackBuffer().ID(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
}
