#include "DefaultRendererImpl.h"
#include <App/App.h>
#include <App/Timer.h>
#include <Core/Direct3DHelpers.h>
#include <Core/SharedShaderResources.h>

using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::DefaultRenderer;
using namespace ZetaRay::Util;
using namespace ZetaRay::Core;

#define SHADOW 0

void Light::Init(const RenderSettings& settings, LightData& data) noexcept
{
	auto& renderer = App::GetRenderer();
	data.HdrLightAccumRTV = renderer.GetRtvDescriptorHeap().Allocate(1);
	data.GpuDescTable = renderer.GetCbvSrvUavDescriptorHeapGpu().Allocate((int)LightData::DESC_TABLE_CONST::COUNT);

	CreateHDRLightAccumTex(data);

	// sun shadow
	data.SunShadowPass.Init();

	// compositing
	data.CompositingPass.Init();

	// sky dome
	data.SkyDomePass.Init(LightData::HDR_LIGHT_ACCUM_FORMAT);

	// inscattering + sku-view lut
	data.SkyPass.Init(LightData::SKY_LUT_WIDTH, LightData::SKY_LUT_HEIGHT, settings.Inscattering);

	// descriptors
	Direct3DHelper::CreateTexture2DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT),
		data.GpuDescTable.CPUHandle((int)LightData::DESC_TABLE_CONST::ENV_MAP_SRV));

	if (settings.Inscattering)
	{
		Direct3DHelper::CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
			data.GpuDescTable.CPUHandle((int)LightData::DESC_TABLE_CONST::INSCATTERING_SRV));
	}
}

void Light::CreateHDRLightAccumTex(LightData& data) noexcept
{
	auto& renderer = App::GetRenderer();
	auto* device = renderer.GetDevice();
	auto& gpuMem = renderer.GetGpuMemory();
	const int width = renderer.GetRenderWidth();
	const int height = renderer.GetRenderHeight();

	D3D12_CLEAR_VALUE clearValue = {};
	memset(clearValue.Color, 0, sizeof(float) * 4);
	clearValue.Format = LightData::HDR_LIGHT_ACCUM_FORMAT;

	data.HdrLightAccumTex = ZetaMove(gpuMem.GetTexture2D("Light/HDRLightAccum",
		width, height,
		LightData::HDR_LIGHT_ACCUM_FORMAT,
		D3D12_RESOURCE_STATE_COMMON,
		TEXTURE_FLAGS::ALLOW_RENDER_TARGET | TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS, 1, &clearValue));

	// RTV
	Direct3DHelper::CreateRTV(data.HdrLightAccumTex, data.HdrLightAccumRTV.CPUHandle(0));

	// UAV
	Direct3DHelper::CreateTexture2DUAV(data.HdrLightAccumTex, data.GpuDescTable.CPUHandle((int)LightData::DESC_TABLE_CONST::HDR_LIGHT_ACCUM_UAV));
}

void Light::OnWindowSizeChanged(const RenderSettings& settings, LightData& data) noexcept
{
	Light::CreateHDRLightAccumTex(data);

	if (data.SunShadowPass.IsInitialized())
		data.SunShadowPass.OnWindowResized();
}

void Light::Shutdown(LightData& data) noexcept
{
	//data.AnalyticalAliasTableBuff.Reset();
	//data.AnalyticalLightBuff.Reset();
	//data.AnalyticalLightSources.free();
	//data.EmissiveAliasTable.Reset();
	//data.EmissiveTrianglesBuff.Reset();
	//data.EmissiveTriangles.free_memory();
	data.HdrLightAccumRTV.Reset();
	data.GpuDescTable.Reset();
	data.HdrLightAccumTex.Reset();
	data.CompositingPass.Reset();
	data.SunShadowPass.Reset();
	data.SkyDomePass.Reset();
	data.SkyPass.Reset();
}

void Light::Update(const RenderSettings& settings, LightData& data, const GBufferData& gbuffData,
	const RayTracerData& rayTracerData) noexcept
{
	if (settings.Inscattering && !data.SkyPass.IsInscatteringEnabled())
	{
		data.SkyPass.SetInscatteringEnablement(true);

		Direct3DHelper::CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
			data.GpuDescTable.CPUHandle((int)LightData::DESC_TABLE_CONST::INSCATTERING_SRV));
	}
	else if (!settings.Inscattering && data.SkyPass.IsInscatteringEnabled())
		data.SkyPass.SetInscatteringEnablement(false);

	const int currOutIdx = App::GetRenderer().CurrOutIdx();

	// dsv changes every frame
	data.SkyDomePass.SetDescriptor(SkyDome::SHADER_IN_DESC::RTV, data.HdrLightAccumRTV.CPUHandle(0));
	data.SkyDomePass.SetDescriptor(SkyDome::SHADER_IN_DESC::DEPTH_BUFFER,
		gbuffData.DSVDescTable[currOutIdx].CPUHandle(0));

	data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::HDR_LIGHT_ACCUM,
		data.GpuDescTable.GPUDesciptorHeapIndex((int)LightData::DESC_TABLE_CONST::HDR_LIGHT_ACCUM_UAV));

	auto& tlas = const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS();

	if (tlas.IsInitialized())
	{
		data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::RESERVOIR_A,
			rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::SPATIAL_RESERVOIR_A));

		data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::RESERVOIR_B,
			rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::SPATIAL_RESERVOIR_B));

		// sun shadow temporal cache changes every frame
		data.SunShadowGpuDescTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((int)LightData::DESC_TABLE_PER_FRAME::COUNT);

		Direct3DHelper::CreateTexture2DSRV(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::TEMPORAL_CACHE_OUT_POST),
			data.SunShadowGpuDescTable.CPUHandle((int)LightData::DESC_TABLE_PER_FRAME::DENOISED_SHADOW_MASK));

		Direct3DHelper::CreateTexture2DSRV(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::RAW_SHADOW_MASK),
			data.SunShadowGpuDescTable.CPUHandle((int)LightData::DESC_TABLE_PER_FRAME::RAW_SHADOW_MASK));

//		data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::SUN_SHADOW,
//			data.SunShadowGpuDescTable.GPUDesciptorHeapIndex(0));
		data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::SUN_SHADOW,
			data.SunShadowGpuDescTable.GPUDesciptorHeapIndex((int)LightData::DESC_TABLE_PER_FRAME::DENOISED_SHADOW_MASK));

		if (settings.Inscattering)
		{
			data.CompositingPass.SetInscatteringEnablement(true);

			const float p = data.SkyPass.GetVoxelGridMappingExp();
			float zNear;
			float zFar;
			data.SkyPass.GetVoxelGridDepth(zNear, zFar);

			data.CompositingPass.SetVoxelGridMappingExp(p);
			data.CompositingPass.SetVoxelGridDepth(zNear, zFar);
			data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::INSCATTERING,
				data.GpuDescTable.GPUDesciptorHeapIndex((int)LightData::DESC_TABLE_CONST::INSCATTERING_SRV));
		}
		else
			data.CompositingPass.SetInscatteringEnablement(false);

		if (settings.IndirectDiffuseDenoiser != Settings::DENOISER::NONE)
		{
			data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::DENOISED_L_IND,
				rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::STAD_TEMPORAL_CACHE));
		}
	}
}

void Light::Register(const RenderSettings& settings, LightData& data, const RayTracerData& rayTracerData,
	RenderGraph& renderGraph) noexcept
{
	renderGraph.RegisterResource(data.HdrLightAccumTex.GetResource(), data.HdrLightAccumTex.GetPathID());
	auto& tlas = const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS();

	// sky view lut + inscattering
	if (tlas.IsInitialized())
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SkyPass,
			&Sky::Render);
		data.SkyHandle = renderGraph.RegisterRenderPass("Sky", RENDER_NODE_TYPE::ASYNC_COMPUTE, dlg);

		if (settings.Inscattering)
		{
			auto& voxelGrid = data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING);
			renderGraph.RegisterResource(voxelGrid.GetResource(), voxelGrid.GetPathID(), D3D12_RESOURCE_STATE_COMMON, false);
		}
	}

	auto& skyviewLUT = data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT);
	renderGraph.RegisterResource(skyviewLUT.GetResource(), skyviewLUT.GetPathID(), D3D12_RESOURCE_STATE_COMMON, false);

	// sun shadow
	if (tlas.IsInitialized())
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SunShadowPass,
			&SunShadow::Render);
		data.SunShadowHandle = renderGraph.RegisterRenderPass("SunShadow", RENDER_NODE_TYPE::COMPUTE, dlg);

		Texture& mask = const_cast<Texture&>(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::RAW_SHADOW_MASK));
		renderGraph.RegisterResource(mask.GetResource(), mask.GetPathID());

		Texture& tcA = const_cast<Texture&>(data.SunShadowPass.GetInput(SunShadow::SHADER_IN_RES::TEMPORAL_CACHE_IN));
		renderGraph.RegisterResource(tcA.GetResource(), tcA.GetPathID());

		Texture& tcB = const_cast<Texture&>(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::TEMPORAL_CACHE_OUT_PRE));
		renderGraph.RegisterResource(tcB.GetResource(), tcB.GetPathID());
	}

	// skydome
	if(tlas.IsInitialized())
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SkyDomePass,
			&SkyDome::Render);
		data.SkyDomeHandle = renderGraph.RegisterRenderPass("SkyDome", RENDER_NODE_TYPE::RENDER, dlg);
	}

	// compositing
	fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.CompositingPass,
		&Compositing::Render);
	data.CompositingHandle = renderGraph.RegisterRenderPass("Compositing", RENDER_NODE_TYPE::COMPUTE, dlg);
}

void Light::DeclareAdjacencies(const RenderSettings& settings, LightData& lightData, const GBufferData& gbuffData,
	const RayTracerData& rayTracerData, RenderGraph& renderGraph) noexcept
{
	const int outIdx = App::GetRenderer().CurrOutIdx();
	auto& tlas = const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS();

	// inscattering + sky-view lut
	if (settings.Inscattering && tlas.IsInitialized())
	{
		// RT-AS
		renderGraph.AddInput(lightData.SkyHandle,
			const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS().GetPathID(),
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		renderGraph.AddOutput(lightData.SkyHandle,
			lightData.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		renderGraph.AddOutput(lightData.SkyHandle,
			lightData.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	// sun shadow
	if (tlas.IsInitialized())
	{
		// RT-AS
		renderGraph.AddInput(lightData.SunShadowHandle,
			tlas.GetPathID(),
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		// make sure it runs post gbuffer
		renderGraph.AddInput(lightData.SunShadowHandle,
			gbuffData.DepthBuffer[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(lightData.SunShadowHandle,
			gbuffData.DepthBuffer[1 - outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(lightData.SunShadowHandle,
			gbuffData.Normal[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(lightData.SunShadowHandle,
			gbuffData.MotionVec.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddOutput(lightData.SunShadowHandle,
			lightData.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::RAW_SHADOW_MASK).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		renderGraph.AddInput(lightData.SunShadowHandle,
			lightData.SunShadowPass.GetInput(SunShadow::SHADER_IN_RES::TEMPORAL_CACHE_IN).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddOutput(lightData.SunShadowHandle,
			lightData.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::TEMPORAL_CACHE_OUT_PRE).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	// sky dome
	if (tlas.IsInitialized())
	{
		// make sure it runs post gbuffer
		renderGraph.AddInput(lightData.SkyDomeHandle, 
			gbuffData.Normal[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(lightData.SkyDomeHandle,
			lightData.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddOutput(lightData.SkyDomeHandle,
			gbuffData.DepthBuffer[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE);

		renderGraph.AddOutput(lightData.SkyDomeHandle,
			lightData.HdrLightAccumTex.GetPathID(),
			D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	// compositing
	renderGraph.AddInput(lightData.CompositingHandle,
		gbuffData.BaseColor[outIdx].GetPathID(),
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

	renderGraph.AddInput(lightData.CompositingHandle,
		gbuffData.Normal[outIdx].GetPathID(),
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

	renderGraph.AddInput(lightData.CompositingHandle,
		gbuffData.DepthBuffer[outIdx].GetPathID(),
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

	renderGraph.AddInput(lightData.CompositingHandle,
		gbuffData.MetalnessRoughness[outIdx].GetPathID(),
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

	if (tlas.IsInitialized())
	{
		renderGraph.AddInput(lightData.CompositingHandle,
			lightData.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::TEMPORAL_CACHE_OUT_POST).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(lightData.CompositingHandle,
			lightData.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::RAW_SHADOW_MASK).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(lightData.CompositingHandle,
			rayTracerData.ReSTIR_GIPass.GetOutput(ReSTIR_GI::SHADER_OUT_RES::SPATIAL_RESERVOIR_A).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(lightData.CompositingHandle,
			rayTracerData.ReSTIR_GIPass.GetOutput(ReSTIR_GI::SHADER_OUT_RES::SPATIAL_RESERVOIR_B).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		// inscattering
		if (settings.Inscattering)
		{
			renderGraph.AddInput(lightData.CompositingHandle,
				lightData.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING).GetPathID(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}

		// denoised indirect diffuse
		if (settings.IndirectDiffuseDenoiser == Settings::DENOISER::STAD)
		{
			renderGraph.AddInput(lightData.CompositingHandle,
				rayTracerData.StadPass.GetOutput(STAD::SHADER_OUT_RES::SPATIAL_FILTER_OUT).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}
	}

	renderGraph.AddOutput(lightData.CompositingHandle, 
		lightData.HdrLightAccumTex.GetPathID(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}


