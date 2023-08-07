#include "DefaultRendererImpl.h"
#include <App/App.h>
#include <App/Timer.h>
#include <Core/Direct3DHelpers.h>
#include <RayTracing/RtAccelerationStructure.h>

using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::DefaultRenderer;
using namespace ZetaRay::DefaultRenderer::Settings;
using namespace ZetaRay::Util;
using namespace ZetaRay::Core;

void PostProcessor::Init(const RenderSettings& settings, PostProcessData& data, const LightData& lightData) noexcept
{
	data.AutoExposurePass.Init();
	data.DisplayPass.Init();
	data.GuiPass.Init();

	UpdateWndDependentDescriptors(settings, data, lightData);
}

void PostProcessor::UpdateWndDependentDescriptors(const RenderSettings& settings, PostProcessData& data, const LightData& lightData) noexcept
{
	data.WindowSizeConstSRVs = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((int)PostProcessData::DESC_TABLE_CONST::COUNT);

	Direct3DHelper::CreateTexture2DSRV(data.AutoExposurePass.GetOutput(AutoExposure::SHADER_OUT_RES::EXPOSURE),
		data.WindowSizeConstSRVs.CPUHandle((int)PostProcessData::DESC_TABLE_CONST::EXPOSURE_SRV));

	const Core::Texture& lightAccum = lightData.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED_DEFAULT);
	Direct3DHelper::CreateTexture2DSRV(lightAccum,
		data.WindowSizeConstSRVs.CPUHandle((int)PostProcessData::DESC_TABLE_CONST::HDR_LIGHT_ACCUM_SRV));
}

void PostProcessor::UpdateFrameDescriptors(const RenderSettings& settings, PostProcessData& data, const LightData& lightData) noexcept
{
	if (settings.AntiAliasing == AA::FSR2 || settings.AntiAliasing == AA::NATIVE_TAA)
		data.TaaOrFsr2OutSRV = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(1);

	if (settings.AntiAliasing == AA::FSR2)
	{
		const Texture& upscaled = data.Fsr2Pass.GetOutput(FSR2Pass::SHADER_OUT_RES::UPSCALED);
		Assert(upscaled.IsInitialized(), "Upscaled output hasn't been initialized.");

		Direct3DHelper::CreateTexture2DSRV(upscaled, data.TaaOrFsr2OutSRV.CPUHandle(0));
	}
	else if (settings.AntiAliasing == AA::NATIVE_TAA)
	{
		const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

		// due to ping-ponging between textures, TAA's output texture changes every frame
		const TAA::SHADER_OUT_RES taaOutIdx = outIdx == 0 ? TAA::SHADER_OUT_RES::OUTPUT_B : TAA::SHADER_OUT_RES::OUTPUT_A;
		Texture& taaOut = data.TaaPass.GetOutput(taaOutIdx);
		Direct3DHelper::CreateTexture2DSRV(taaOut, data.TaaOrFsr2OutSRV.CPUHandle(0));
	}

	// can change every frame due to UI controls
	if (settings.DoF || settings.FireflyFilter)
	{
		const Core::Texture& dof = lightData.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::FINAL_OUTPUT);
		Direct3DHelper::CreateTexture2DSRV(dof,
			data.WindowSizeConstSRVs.CPUHandle((int)PostProcessData::DESC_TABLE_CONST::DoF_SRV));
	}
}

void PostProcessor::UpdatePasses(const RenderSettings& settings, PostProcessData& data) noexcept
{
	if (settings.AntiAliasing != AA::FSR2 && data.Fsr2Pass.IsInitialized())
	{
		data.Fsr2Pass.Reset();
		data.TaaOrFsr2OutSRV.Reset();
	}

	if (settings.AntiAliasing != AA::NATIVE_TAA && data.TaaPass.IsInitialized())
	{
		data.TaaPass.Reset();
		data.TaaOrFsr2OutSRV.Reset();
	}

	if (settings.AntiAliasing == AA::NATIVE_TAA && !data.TaaPass.IsInitialized())
		data.TaaPass.Init();
	else if (settings.AntiAliasing == AA::FSR2 && !data.Fsr2Pass.IsInitialized())
		data.Fsr2Pass.Init();
}

void PostProcessor::OnWindowSizeChanged(const RenderSettings& settings, PostProcessData& data,
	const LightData& lightData) noexcept
{
	if (settings.AntiAliasing == AA::NATIVE_TAA)
		data.TaaPass.OnWindowResized();
	else if (settings.AntiAliasing == AA::FSR2)
		data.Fsr2Pass.OnWindowResized();

	UpdateWndDependentDescriptors(settings, data, lightData);
}

void PostProcessor::Shutdown(PostProcessData& data) noexcept
{
	data.WindowSizeConstSRVs.Reset();
	data.TaaOrFsr2OutSRV.Reset();
	data.DisplayPass.Reset();
	data.AutoExposurePass.Reset();
	data.TaaPass.Reset();
	data.Fsr2Pass.Reset();
	data.GuiPass.Reset();
}

void PostProcessor::Update(const RenderSettings& settings, PostProcessData& data, const GBufferData& gbuffData,
	const LightData& lightData, const RayTracerData& rayTracerData) noexcept
{
	UpdatePasses(settings, data);
	UpdateFrameDescriptors(settings, data, lightData);

	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();
	const auto compositedSrv = !settings.DoF && !settings.FireflyFilter ? PostProcessData::DESC_TABLE_CONST::HDR_LIGHT_ACCUM_SRV :
		PostProcessData::DESC_TABLE_CONST::DoF_SRV;

	// Display
	auto backBuffRTV = App::GetRenderer().GetCurrBackBufferRTV();
	data.DisplayPass.SetCpuDescriptor(DisplayPass::SHADER_IN_CPU_DESC::RTV, backBuffRTV);

	const Texture& exposureTex = data.AutoExposurePass.GetOutput(AutoExposure::SHADER_OUT_RES::EXPOSURE);
	data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::EXPOSURE,
		data.WindowSizeConstSRVs.GPUDesciptorHeapIndex((int)PostProcessData::DESC_TABLE_CONST::EXPOSURE_SRV));

	data.GuiPass.SetCPUDescriptor(GuiPass::SHADER_IN_CPU_DESC::DEPTH_BUFFER, gbuffData.DSVDescTable[outIdx].CPUHandle(0));
	data.GuiPass.SetCPUDescriptor(GuiPass::SHADER_IN_CPU_DESC::RTV, backBuffRTV);

	// Auto Exposure
	data.AutoExposurePass.SetDescriptor(AutoExposure::SHADER_IN_DESC::COMPOSITED,
		data.WindowSizeConstSRVs.GPUDesciptorHeapIndex((int)compositedSrv));

	// TAA
	if (settings.AntiAliasing == AA::NATIVE_TAA)
	{
		data.TaaPass.SetDescriptor(TAA::SHADER_IN_DESC::SIGNAL,
			data.WindowSizeConstSRVs.GPUDesciptorHeapIndex((int)compositedSrv));

		// Display
		data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::FINAL_LIGHTING, data.TaaOrFsr2OutSRV.GPUDesciptorHeapIndex(0));
	}
	// FSR2
	else if (settings.AntiAliasing == AA::FSR2)
	{
		Core::Texture& composited = const_cast<Core::Texture&>(lightData.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::FINAL_OUTPUT));

		data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::DEPTH, const_cast<Texture&>(gbuffData.DepthBuffer[outIdx]).GetResource());
		data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::MOTION_VECTOR, const_cast<Texture&>(gbuffData.MotionVec).GetResource());
		data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::COLOR, composited.GetResource());
		data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::EXPOSURE, const_cast<Texture&>(exposureTex).GetResource());

		// Display
		data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::FINAL_LIGHTING, data.TaaOrFsr2OutSRV.GPUDesciptorHeapIndex(0));
	}
	else
	{
		// Display
		data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::FINAL_LIGHTING,
			data.WindowSizeConstSRVs.GPUDesciptorHeapIndex((int)compositedSrv));
	}

	// indirect diffuse reservoirs
	data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::ReSTIR_GI_DIFFUSE_TEMPORAL_RESERVOIR_A,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::DIFFUSE_TEMPORAL_RESERVOIR_A));

	data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::ReSTIR_GI_DIFFUSE_TEMPORAL_RESERVOIR_B,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::DIFFUSE_TEMPORAL_RESERVOIR_B));

	data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::ReSTIR_GI_DIFFUSE_SPATIAL_RESERVOIR_A,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::DIFFUSE_SPATIAL_RESERVOIR_A));

	data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::ReSTIR_GI_DIFFUSE_SPATIAL_RESERVOIR_B,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::DIFFUSE_SPATIAL_RESERVOIR_B));

	// denoised indirect diffuse
	data.DisplayPass.SetGpuDescriptor(DisplayPass::SHADER_IN_GPU_DESC::DIFFUSE_DNSR_CACHE,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::DIFFUSE_DNSR_TEMPORAL_CACHE));

	data.GuiPass.Update();
}

void PostProcessor::Register(const RenderSettings& settings, PostProcessData& data, RenderGraph& renderGraph) noexcept
{
	// TAA
	if (settings.AntiAliasing == AA::NATIVE_TAA)
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.TaaPass,
			&TAA::Render);

		data.TaaHandle = renderGraph.RegisterRenderPass("TAA", RENDER_NODE_TYPE::COMPUTE, dlg);

		Texture& taaA = data.TaaPass.GetOutput(TAA::SHADER_OUT_RES::OUTPUT_A);
		renderGraph.RegisterResource(taaA.GetResource(), taaA.GetPathID());

		Texture& taaB = data.TaaPass.GetOutput(TAA::SHADER_OUT_RES::OUTPUT_B);
		renderGraph.RegisterResource(taaB.GetResource(), taaB.GetPathID());
	}
	// FSR2
	else if (settings.AntiAliasing == AA::FSR2)
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.Fsr2Pass,
			&FSR2Pass::Render);

		data.Fsr2Handle = renderGraph.RegisterRenderPass("FSR2", RENDER_NODE_TYPE::COMPUTE, dlg);

		const Texture& upscaled = data.Fsr2Pass.GetOutput(FSR2Pass::SHADER_OUT_RES::UPSCALED);
		renderGraph.RegisterResource(const_cast<Texture&>(upscaled).GetResource(), upscaled.GetPathID());
	}

	// Auto Exposure
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.AutoExposurePass,
			&AutoExposure::Render);

		data.AutoExposureHandle = renderGraph.RegisterRenderPass("AutoExposure", RENDER_NODE_TYPE::COMPUTE, dlg);

		Texture& exposureTex = data.AutoExposurePass.GetOutput(AutoExposure::SHADER_OUT_RES::EXPOSURE);
		renderGraph.RegisterResource(exposureTex.GetResource(), exposureTex.GetPathID(), D3D12_RESOURCE_STATE_COMMON, false);
	}

	// Display
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.DisplayPass, &DisplayPass::Render);
		data.DisplayHandle = renderGraph.RegisterRenderPass("DisplayPass", RENDER_NODE_TYPE::RENDER, dlg);
	}

	// ImGui
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.GuiPass, &GuiPass::Render);
		data.GuiHandle = renderGraph.RegisterRenderPass("GuiPass", RENDER_NODE_TYPE::RENDER, dlg);
	}

	// register backbuffer
	const Texture& backbuff = App::GetRenderer().GetCurrBackBuffer();
	renderGraph.RegisterResource(const_cast<Texture&>(backbuff).GetResource(), backbuff.GetPathID());

	// dummy resource
	renderGraph.RegisterResource(nullptr, RenderGraph::DUMMY_RES::RES_1);
}

void PostProcessor::DeclareAdjacencies(const RenderSettings& settings, PostProcessData& data, const GBufferData& gbuffData,
	const LightData& lightData, const RayTracerData& rayTracerData, RenderGraph& renderGraph) noexcept
{
	const Core::Texture& composited = lightData.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::FINAL_OUTPUT);
	const Texture& exposureTex = data.AutoExposurePass.GetOutput(AutoExposure::SHADER_OUT_RES::EXPOSURE);

	// TAA
	if (settings.AntiAliasing == AA::NATIVE_TAA)
	{
		const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

		const TAA::SHADER_OUT_RES taaCurrOutIdx = outIdx == 0 ? TAA::SHADER_OUT_RES::OUTPUT_B : TAA::SHADER_OUT_RES::OUTPUT_A;
		const TAA::SHADER_OUT_RES taaPrevOutIdx = outIdx == 0 ? TAA::SHADER_OUT_RES::OUTPUT_A : TAA::SHADER_OUT_RES::OUTPUT_B;
		Texture& taaCurrOut = data.TaaPass.GetOutput(taaCurrOutIdx);
		Texture& taaPrevOut = data.TaaPass.GetOutput(taaPrevOutIdx);

		renderGraph.AddInput(data.TaaHandle,
			gbuffData.DepthBuffer[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.TaaHandle,
			composited.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.TaaHandle,
			taaPrevOut.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddOutput(data.TaaHandle,
			taaCurrOut.GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		// Display
		renderGraph.AddInput(data.DisplayHandle,
			taaCurrOut.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
	}
	// FSR2
	else if (settings.AntiAliasing == AA::FSR2)
	{
		const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

		renderGraph.AddInput(data.Fsr2Handle,
			gbuffData.DepthBuffer[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.Fsr2Handle,
			composited.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.Fsr2Handle,
			gbuffData.MotionVec.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.Fsr2Handle,
			exposureTex.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		const Texture& upscaled = data.Fsr2Pass.GetOutput(FSR2Pass::SHADER_OUT_RES::UPSCALED);
		Assert(upscaled.IsInitialized(), "Upscaled output hasn't been initialized.");

		renderGraph.AddOutput(data.Fsr2Handle,
			upscaled.GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		// Display
		renderGraph.AddInput(data.DisplayHandle,
			upscaled.GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}

	// Auto Exposure
	{
		renderGraph.AddInput(data.AutoExposureHandle,
			composited.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddOutput(data.AutoExposureHandle,
			exposureTex.GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	// Display
	renderGraph.AddInput(data.DisplayHandle,
		gbuffData.Curvature.GetPathID(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	renderGraph.AddInput(data.DisplayHandle,
		exposureTex.GetPathID(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	if (const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS().IsInitialized())
	{
		// indirect diffuse reservoirs
		renderGraph.AddInput(data.DisplayHandle,
			rayTracerData.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_A).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddInput(data.DisplayHandle,
			rayTracerData.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_B).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddInput(data.DisplayHandle,
			rayTracerData.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_C).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddInput(data.DisplayHandle,
			rayTracerData.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_A).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddInput(data.DisplayHandle,
			rayTracerData.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_B).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddInput(data.DisplayHandle,
			rayTracerData.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_C).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		// denoised indirect diffuse
		renderGraph.AddInput(data.DisplayHandle,
			rayTracerData.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::DNSR_TEMPORAL_CACHE_POST_SPATIAL).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}

	// backbuffer
	renderGraph.AddOutput(data.DisplayHandle,
		App::GetRenderer().GetCurrBackBuffer().GetPathID(),
		D3D12_RESOURCE_STATE_RENDER_TARGET);

	// for GUI Pass
	renderGraph.AddOutput(data.DisplayHandle,
		RenderGraph::DUMMY_RES::RES_1,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// due to blending, ImGui should go last
	renderGraph.AddInput(data.GuiHandle,
		RenderGraph::DUMMY_RES::RES_1,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	renderGraph.AddOutput(data.GuiHandle,
		App::GetRenderer().GetCurrBackBuffer().GetPathID(),
		D3D12_RESOURCE_STATE_RENDER_TARGET);
}

