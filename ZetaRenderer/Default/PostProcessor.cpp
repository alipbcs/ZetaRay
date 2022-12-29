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

void PostProcessor::Init(const RenderSettings& settings, PostProcessData& data, const LightData& lightata) noexcept
{
	// Auto Exposure
	data.AutoExposurePass.Init(AutoExposure::MODE::HISTOGRAM);

	// Final Pass
	data.FinalDrawPass.Init();

	// GUI
	data.GuiPass.Init();

	data.WindowSizeConstSRVs = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((int)PostProcessData::DESC_TABLE_CONST::COUNT);
	data.HdrLightAccumRTV = App::GetRenderer().GetRtvDescriptorHeap().Allocate(1);

	Direct3DHelper::CreateRTV(lightata.HdrLightAccumTex, data.HdrLightAccumRTV.CPUHandle(0));
	Direct3DHelper::CreateTexture2DSRV(lightata.HdrLightAccumTex, 
		data.WindowSizeConstSRVs.CPUHandle((int)PostProcessData::DESC_TABLE_CONST::HDR_LIGHT_ACCUM_SRV));
	Direct3DHelper::CreateTexture2DSRV(data.AutoExposurePass.GetOutput(AutoExposure::SHADER_OUT_RES::EXPOSURE),
		data.WindowSizeConstSRVs.CPUHandle((int)PostProcessData::DESC_TABLE_CONST::EXPOSURE_SRV));
}

void PostProcessor::UpdateDescriptors(const RenderSettings& settings, PostProcessData& postData) noexcept
{
	if (settings.AntiAliasing == AA::FSR2)
	{
		const Texture& upscaled = postData.Fsr2Pass.GetOutput(FSR2Pass::SHADER_OUT_RES::UPSCALED);
		Assert(upscaled.IsInitialized(), "Upscaled output hasn't been initialized.");
		Assert(!postData.TaaOrFsr2OutSRV.IsEmpty(), "Empty desc. table.");

		Direct3DHelper::CreateTexture2DSRV(upscaled, postData.TaaOrFsr2OutSRV.CPUHandle(0));
	}
	else if (settings.AntiAliasing == AA::NATIVE_TAA)
	{
		const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

		// due to ping-ponging between textures, TAA's output texture changes every frame
		const TAA::SHADER_OUT_RES taaOutIdx = outIdx == 0 ? TAA::SHADER_OUT_RES::OUTPUT_B : TAA::SHADER_OUT_RES::OUTPUT_A;
		Texture& taaOut = postData.TaaPass.GetOutput(taaOutIdx);
		Direct3DHelper::CreateTexture2DSRV(taaOut, postData.TaaOrFsr2OutSRV.CPUHandle(0));
	}
}

void PostProcessor::UpdatePasses(const RenderSettings& settings, PostProcessData& postData) noexcept
{
	if (settings.AntiAliasing != AA::FSR2 && postData.Fsr2Pass.IsInitialized())
	{
		postData.Fsr2Pass.Reset();
		postData.TaaOrFsr2OutSRV.Reset();
	}

	if (settings.AntiAliasing != AA::NATIVE_TAA && postData.TaaPass.IsInitialized())
	{
		postData.TaaPass.Reset();
		postData.TaaOrFsr2OutSRV.Reset();
	}

	if (settings.AntiAliasing == AA::NATIVE_TAA)
	{
		if(!postData.TaaPass.IsInitialized())
			postData.TaaPass.Init();
	
		if(postData.TaaOrFsr2OutSRV.IsEmpty())
			postData.TaaOrFsr2OutSRV = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(1);
	}
	else if (settings.AntiAliasing == AA::FSR2)
	{
		if(!postData.Fsr2Pass.IsInitialized())
			postData.Fsr2Pass.Init();
	
		if (postData.TaaOrFsr2OutSRV.IsEmpty())
			postData.TaaOrFsr2OutSRV = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(1);
	}
}

void PostProcessor::OnWindowSizeChanged(const RenderSettings& settings, PostProcessData& data, 
	const LightData& lightData) noexcept
{
	if (settings.AntiAliasing == AA::NATIVE_TAA)
		data.TaaPass.OnWindowResized();
	else if (settings.AntiAliasing == AA::FSR2)
		data.Fsr2Pass.OnWindowResized();

	data.AutoExposurePass.OnWindowResized();
	
	data.TaaOrFsr2OutSRV.Reset();

	Direct3DHelper::CreateRTV(lightData.HdrLightAccumTex, data.HdrLightAccumRTV.CPUHandle(0));
	Direct3DHelper::CreateTexture2DSRV(lightData.HdrLightAccumTex,
		data.WindowSizeConstSRVs.CPUHandle((int)PostProcessData::DESC_TABLE_CONST::HDR_LIGHT_ACCUM_SRV));
}

void PostProcessor::Shutdown(PostProcessData& data) noexcept
{
	data.HdrLightAccumRTV.Reset();
	data.WindowSizeConstSRVs.Reset();
	data.TaaOrFsr2OutSRV.Reset();
	data.FinalDrawPass.Reset();
	data.AutoExposurePass.Reset();
	data.TaaPass.Reset();
	data.Fsr2Pass.Reset();
	data.GuiPass.Reset();
}

void PostProcessor::Update(const RenderSettings& settings, const GBufferData& gbuffData, 
	const LightData& lightData, const RayTracerData& rayTracerData, PostProcessData& data) noexcept
{
	UpdatePasses(settings, data);
	UpdateDescriptors(settings, data);

	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

	// Final
	auto backBuffRTV = App::GetRenderer().GetCurrBackBufferRTV();
	data.FinalDrawPass.SetCpuDescriptor(FinalPass::SHADER_IN_CPU_DESC::RTV, backBuffRTV);

	const Texture& exposureTex = data.AutoExposurePass.GetOutput(AutoExposure::SHADER_OUT_RES::EXPOSURE);
	data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::EXPOSURE, 
		data.WindowSizeConstSRVs.GPUDesciptorHeapIndex((int)PostProcessData::DESC_TABLE_CONST::EXPOSURE_SRV));

	data.GuiPass.SetCPUDescriptor(GuiPass::SHADER_IN_CPU_DESC::DEPTH_BUFFER, gbuffData.DSVDescTable[outIdx].CPUHandle(0));
	data.GuiPass.SetCPUDescriptor(GuiPass::SHADER_IN_CPU_DESC::RTV, backBuffRTV);

	// Auto Exposure
	data.AutoExposurePass.SetDescriptor(AutoExposure::SHADER_IN_DESC::COMPOSITED, 
		data.WindowSizeConstSRVs.GPUDesciptorHeapIndex((int)PostProcessData::DESC_TABLE_CONST::HDR_LIGHT_ACCUM_SRV));

	// TAA
	if (settings.AntiAliasing == AA::NATIVE_TAA)
	{
		data.TaaPass.SetDescriptor(TAA::SHADER_IN_DESC::SIGNAL, 
			data.WindowSizeConstSRVs.GPUDesciptorHeapIndex((int)PostProcessData::DESC_TABLE_CONST::HDR_LIGHT_ACCUM_SRV));

		// Final
		data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::FINAL_LIGHTING, data.TaaOrFsr2OutSRV.GPUDesciptorHeapIndex(0));
	}
	// FSR2
	else if(settings.AntiAliasing == AA::FSR2)
	{
		data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::DEPTH, const_cast<Texture&>(gbuffData.DepthBuffer[outIdx]).GetResource());
		data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::MOTION_VECTOR, const_cast<Texture&>(gbuffData.MotionVec).GetResource());
		data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::COLOR, const_cast<Texture&>(lightData.HdrLightAccumTex).GetResource());

		// Final
		data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::FINAL_LIGHTING, data.TaaOrFsr2OutSRV.GPUDesciptorHeapIndex(0));
	}
	else
	{ 
		// Final
		data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::FINAL_LIGHTING, 
			data.WindowSizeConstSRVs.GPUDesciptorHeapIndex((int)PostProcessData::DESC_TABLE_CONST::HDR_LIGHT_ACCUM_SRV));
	}

	data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::ReSTIR_GI_TEMPORAL_RESERVOIR_A,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::TEMPORAL_RESERVOIR_A));

	data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::ReSTIR_GI_TEMPORAL_RESERVOIR_B,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::TEMPORAL_RESERVOIR_B));

	data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::ReSTIR_GI_TEMPORAL_RESERVOIR_C,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::TEMPORAL_RESERVOIR_C));

	data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::ReSTIR_GI_SPATIAL_RESERVOIR_A,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::SPATIAL_RESERVOIR_A));

	data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::ReSTIR_GI_SPATIAL_RESERVOIR_B,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::SPATIAL_RESERVOIR_B));

	data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::ReSTIR_GI_SPATIAL_RESERVOIR_C,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::SPATIAL_RESERVOIR_C));

	if (settings.IndirectDiffuseDenoiser == DENOISER::STAD)
	{
		data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::DENOISER_TEMPORAL_CACHE,
			rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::STAD_TEMPORAL_CACHE));
	}
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

	// Final
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.FinalDrawPass, &FinalPass::Render);
		data.FinalHandle = renderGraph.RegisterRenderPass("FinalPass", RENDER_NODE_TYPE::RENDER, dlg);
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

void PostProcessor::DeclareAdjacencies(const RenderSettings& settings, const GBufferData& gbuffData, 
	const LightData& lightData, const RayTracerData& rayTracerData, PostProcessData& postData, 
	RenderGraph& renderGraph) noexcept
{
	// TAA
	if (settings.AntiAliasing == AA::NATIVE_TAA)
	{
		const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

		const TAA::SHADER_OUT_RES taaCurrOutIdx = outIdx == 0 ? TAA::SHADER_OUT_RES::OUTPUT_B : TAA::SHADER_OUT_RES::OUTPUT_A;
		const TAA::SHADER_OUT_RES taaPrevOutIdx = outIdx == 0 ? TAA::SHADER_OUT_RES::OUTPUT_A : TAA::SHADER_OUT_RES::OUTPUT_B;
		Texture& taaCurrOut = postData.TaaPass.GetOutput(taaCurrOutIdx);
		Texture& taaPrevOut = postData.TaaPass.GetOutput(taaPrevOutIdx);

		renderGraph.AddInput(postData.TaaHandle,
			gbuffData.DepthBuffer[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(postData.TaaHandle,
			lightData.HdrLightAccumTex.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(postData.TaaHandle,
			taaPrevOut.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddOutput(postData.TaaHandle,
			taaCurrOut.GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		// Final
		renderGraph.AddInput(postData.FinalHandle,
			taaCurrOut.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
	}
	// FSR2
	else if (settings.AntiAliasing == AA::FSR2)
	{
		const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

		renderGraph.AddInput(postData.Fsr2Handle,
			gbuffData.DepthBuffer[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(postData.Fsr2Handle,
			lightData.HdrLightAccumTex.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(postData.Fsr2Handle,
			gbuffData.MotionVec.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		const Texture& upscaled = postData.Fsr2Pass.GetOutput(FSR2Pass::SHADER_OUT_RES::UPSCALED);
		Assert(upscaled.IsInitialized(), "Upscaled output hasn't been initialized.");

		renderGraph.AddOutput(postData.Fsr2Handle,
			upscaled.GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		// Final
		renderGraph.AddInput(postData.FinalHandle,
			upscaled.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
	}

	const Texture& exposureTex = postData.AutoExposurePass.GetOutput(AutoExposure::SHADER_OUT_RES::EXPOSURE);
	
	// Auto Exposure
	{
		renderGraph.AddInput(postData.AutoExposureHandle,
			lightData.HdrLightAccumTex.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddOutput(postData.AutoExposureHandle,
			exposureTex.GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	// Final
	renderGraph.AddInput(postData.FinalHandle,
		exposureTex.GetPathID(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	if (const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS().IsInitialized())
	{
		renderGraph.AddInput(postData.FinalHandle,
			rayTracerData.ReSTIR_GIPass.GetOutput(ReSTIR_GI::SHADER_OUT_RES::TEMPORAL_RESERVOIR_A).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddInput(postData.FinalHandle,
			rayTracerData.ReSTIR_GIPass.GetOutput(ReSTIR_GI::SHADER_OUT_RES::TEMPORAL_RESERVOIR_B).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddInput(postData.FinalHandle,
			rayTracerData.ReSTIR_GIPass.GetOutput(ReSTIR_GI::SHADER_OUT_RES::TEMPORAL_RESERVOIR_C).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddInput(postData.FinalHandle,
			rayTracerData.ReSTIR_GIPass.GetOutput(ReSTIR_GI::SHADER_OUT_RES::SPATIAL_RESERVOIR_A).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddInput(postData.FinalHandle,
			rayTracerData.ReSTIR_GIPass.GetOutput(ReSTIR_GI::SHADER_OUT_RES::SPATIAL_RESERVOIR_B).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddInput(postData.FinalHandle,
			rayTracerData.ReSTIR_GIPass.GetOutput(ReSTIR_GI::SHADER_OUT_RES::SPATIAL_RESERVOIR_C).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		if (settings.IndirectDiffuseDenoiser == DENOISER::STAD)
		{
			renderGraph.AddInput(postData.FinalHandle,
				rayTracerData.DiffuseDNSRPass.GetOutput(DiffuseDNSR::SHADER_OUT_RES::SPATIAL_FILTER_OUT).GetPathID(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
	}

	renderGraph.AddOutput(postData.FinalHandle,
		App::GetRenderer().GetCurrBackBuffer().GetPathID(),
		D3D12_RESOURCE_STATE_RENDER_TARGET);

	// for GUI Pass
	renderGraph.AddOutput(postData.FinalHandle,
		RenderGraph::DUMMY_RES::RES_1,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// ImGui, due to blending, should go last
	renderGraph.AddInput(postData.GuiHandle,
		RenderGraph::DUMMY_RES::RES_1,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	renderGraph.AddOutput(postData.GuiHandle,
		App::GetRenderer().GetCurrBackBuffer().GetPathID(),
		D3D12_RESOURCE_STATE_RENDER_TARGET);
}

