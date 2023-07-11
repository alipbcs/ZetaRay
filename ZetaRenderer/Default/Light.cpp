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

void Light::Init(const RenderSettings& settings, LightData& data) noexcept
{
	auto& renderer = App::GetRenderer();
	data.HdrLightAccumRTV = renderer.GetRtvDescriptorHeap().Allocate(1);
	data.GpuDescTable = renderer.GetCbvSrvUavDescriptorHeapGpu().Allocate((int)LightData::DESC_TABLE_CONST::COUNT);

	// sun shadow
	data.SunShadowPass.Init();

	// compositing
	data.CompositingPass.Init(settings.DoF, settings.SkyIllumination, settings.FireflyFilter);
	const Core::Texture& lightAccum = data.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED_DEFAULT);

	// sky dome
	data.SkyDomePass.Init(lightAccum.GetDesc().Format);

	// inscattering + sku-view lut
	data.SkyPass.Init(LightData::SKY_LUT_WIDTH, LightData::SKY_LUT_HEIGHT, settings.Inscattering);

	// descriptors
	Direct3DHelper::CreateTexture2DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT),
		data.GpuDescTable.CPUHandle((int)LightData::DESC_TABLE_CONST::ENV_MAP_SRV));

	// RTV
	Direct3DHelper::CreateRTV(lightAccum, data.HdrLightAccumRTV.CPUHandle(0));

	if (settings.Inscattering)
	{
		Direct3DHelper::CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
			data.GpuDescTable.CPUHandle((int)LightData::DESC_TABLE_CONST::INSCATTERING_SRV));
	}
}

void Light::OnWindowSizeChanged(const RenderSettings& settings, LightData& data) noexcept
{
	data.CompositingPass.OnWindowResized();

	// RTV
	const Core::Texture& lightAccum = data.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED_DEFAULT);
	Direct3DHelper::CreateRTV(lightAccum, data.HdrLightAccumRTV.CPUHandle(0));

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

	const int currOutIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

	// dsv changes every frame
	data.SkyDomePass.SetDescriptor(SkyDome::SHADER_IN_DESC::RTV, data.HdrLightAccumRTV.CPUHandle(0));
	data.SkyDomePass.SetDescriptor(SkyDome::SHADER_IN_DESC::DEPTH_BUFFER,
		gbuffData.DSVDescTable[currOutIdx].CPUHandle(0));

	auto& tlas = const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS();

	if (tlas.IsInitialized())
	{
		// indirect diffuse
		data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::DIFFUSE_DNSR_CACHE,
			rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::DIFFUSE_DNSR_TEMPORAL_CACHE));

		// indirect specular
		data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::SPECULAR_DNSR_CACHE,
			rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::SPECULAR_DNSR_TEMPORAL_CACHE));

		// sky DI
		if (settings.SkyIllumination)
		{
			data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::DIRECT_DNSR_CACHE,
				rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::SKY_DNSR_TEMPORAL_CACHE));
		}

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

		// make sure compistor and indirect specular have matching roughness cutoffs
		data.CompositingPass.SetRoughnessCutoff(rayTracerData.ReSTIR_GI_SpecularPass.GetRoughnessCutoff());

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
	}
}

void Light::Register(const RenderSettings& settings, LightData& data, const RayTracerData& rayTracerData,
	RenderGraph& renderGraph) noexcept
{
	Core::Texture& lightAccum = const_cast<Core::Texture&>(data.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED_DEFAULT));
	renderGraph.RegisterResource(lightAccum.GetResource(), lightAccum.GetPathID());

	// dof
	if (settings.DoF || settings.FireflyFilter)
	{
		Core::Texture& filtered = const_cast<Core::Texture&>(data.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED_FILTERED));
		renderGraph.RegisterResource(filtered.GetResource(), filtered.GetPathID());
	}

	auto& tlas = const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS();
	const bool isTLASbuilt = tlas.IsInitialized();

	// sky view lut + inscattering
	if (isTLASbuilt)
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SkyPass, &Sky::Render);
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
	if (isTLASbuilt)
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
	if(isTLASbuilt)
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SkyDomePass, &SkyDome::Render);
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
	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();
	auto& tlas = const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS();
	const bool isTLASbuilt = tlas.IsInitialized();

	// inscattering + sky-view lut
	if (settings.Inscattering && isTLASbuilt)
	{
		// RT_AS
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
	if (isTLASbuilt)
	{
		// RT_AS
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
	if (isTLASbuilt)
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
			lightData.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED_DEFAULT).GetPathID(),
			D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	// compositing
	renderGraph.AddInput(lightData.CompositingHandle,
		gbuffData.BaseColor.GetPathID(),
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

	if (isTLASbuilt)
	{
		// sun shadows
		renderGraph.AddInput(lightData.CompositingHandle,
			lightData.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::TEMPORAL_CACHE_OUT_POST).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(lightData.CompositingHandle,
			lightData.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::RAW_SHADOW_MASK).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		// indirect diffuse
		renderGraph.AddInput(lightData.CompositingHandle,
			rayTracerData.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::DNSR_TEMPORAL_CACHE_POST_SPATIAL).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		// indirect specular
		renderGraph.AddInput(lightData.CompositingHandle,
			rayTracerData.ReSTIR_GI_SpecularPass.GetOutput(ReSTIR_GI_Specular::SHADER_OUT_RES::CURR_DNSR_CACHE).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		// sky di
		if (settings.SkyIllumination)
		{
			renderGraph.AddInput(lightData.CompositingHandle,
				rayTracerData.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}

		// inscattering
		if (settings.Inscattering)
		{
			renderGraph.AddInput(lightData.CompositingHandle,
				lightData.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING).GetPathID(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
	}

	renderGraph.AddOutput(lightData.CompositingHandle, 
		lightData.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED_DEFAULT).GetPathID(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	if (settings.DoF || settings.FireflyFilter)
	{
		renderGraph.AddOutput(lightData.CompositingHandle,
			lightData.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED_FILTERED).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
}


