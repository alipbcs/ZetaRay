#include "SceneRendererImpl.h"
#include "../../App/App.h"
#include "../../App/Timer.h"
#include "../../Core/Direct3DHelpers.h"
#include "../../Core/SharedShaderResources.h"

using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Scene::Render;
using namespace ZetaRay::Util;
using namespace ZetaRay::Core;
using namespace ZetaRay::Scene::Settings;

void Light::Init(const RenderSettings& settings, LightData& data) noexcept
{
	auto& renderer = App::GetRenderer();
	data.HdrLightAccumRTV = renderer.GetRtvDescriptorHeap().Allocate(1);
	data.GpuDescTable = renderer.GetCbvSrvUavDescriptorHeapGpu().Allocate(LightData::DESC_TABLE::COUNT);

	CreateHDRLightAccumTex(data);

	DXGI_FORMAT rtvFormats[1] = { LightData::HDR_LIGHT_ACCUM_FORMAT };
	
	// sun
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DHelper::GetPSODesc(nullptr,
			1,
			rtvFormats,
			RendererConstants::DEPTH_BUFFER_FORMAT);

		// disable depth-testing, but not writing
		psoDesc.DepthStencilState.DepthEnable = false;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		// disable triangle culling
		psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

		data.SunLightPass.Init(psoDesc);
	}

	data.CompositingPass.Init();

	// sky dome
	{
		D3D12_INPUT_ELEMENT_DESC inputElements[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXUV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		D3D12_INPUT_LAYOUT_DESC inputLayout = D3D12_INPUT_LAYOUT_DESC{ .pInputElementDescs = inputElements, 
			.NumElements = ZetaArrayLen(inputElements) };

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DHelper::GetPSODesc(&inputLayout,
			1,
			rtvFormats,
			RendererConstants::DEPTH_BUFFER_FORMAT);

		psoDesc.DepthStencilState.DepthEnable = true;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;		// we're inside the sphere
		//psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

		data.SkyDomePass.Init(psoDesc);
	}

	// inscattering + sku-view lut
	data.SkyPass.Init(LightData::SKY_LUT_WIDTH, LightData::SKY_LUT_HEIGHT, settings.Inscattering);

	// descriptors
	Direct3DHelper::CreateTexture2DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT),
		data.GpuDescTable.CPUHandle(LightData::DESC_TABLE::ENV_MAP_SRV));

	if (settings.Inscattering)
	{
		Direct3DHelper::CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
			data.GpuDescTable.CPUHandle(LightData::DESC_TABLE::INSCATTERING_SRV));
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
	Direct3DHelper::CreateTexture2DUAV(data.HdrLightAccumTex, data.GpuDescTable.CPUHandle(LightData::DESC_TABLE::HDR_LIGHT_ACCUM_UAV));
}

void Light::OnWindowSizeChanged(const RenderSettings& settings, LightData& data) noexcept
{
	Light::CreateHDRLightAccumTex(data);
}

void Light::Shutdown(LightData& data) noexcept
{
	//data.AnalyticalAliasTableBuff.Reset();
	//data.AnalyticalLightBuff.Reset();
	//data.AnalyticalLightSources.free();
	data.EmissiveAliasTable.Reset();
	data.EmissiveTrianglesBuff.Reset();
	data.EmissiveTriangles.free_memory();
	data.HdrLightAccumRTV.Reset();
	data.GpuDescTable.Reset();
	data.HdrLightAccumTex.Reset();
	data.CompositingPass.Reset();
	data.SunLightPass.Reset();
	data.SkyDomePass.Reset();
	data.SkyPass.Reset();
}

void Light::Update(const RenderSettings& settings, const GBufferData& gbuffData, 
	const RayTracerData& rayTracerData, LightData& data) noexcept
{
	if (settings.Inscattering && !data.SkyPass.IsInscatteringEnabled())
	{
		data.SkyPass.SetInscatteringEnablement(true);

		Direct3DHelper::CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
			data.GpuDescTable.CPUHandle(LightData::DESC_TABLE::INSCATTERING_SRV));
	}
	else if (!settings.Inscattering && data.SkyPass.IsInscatteringEnabled())
		data.SkyPass.SetInscatteringEnablement(false);

	const int currOutIdx = App::GetRenderer().CurrOutIdx();

	data.SunLightPass.SetCPUDescriptor(SunLight::RTV, data.HdrLightAccumRTV.CPUHandle(0));
	data.SunLightPass.SetCPUDescriptor(SunLight::DEPTH_BUFFER,
		gbuffData.DSVDescTable[currOutIdx].CPUHandle(0));
	
	// dsv changes every frame
	data.SkyDomePass.SetDescriptor(SkyDome::SHADER_IN_DESC::RTV, data.HdrLightAccumRTV.CPUHandle(0));
	data.SkyDomePass.SetDescriptor(SkyDome::SHADER_IN_DESC::DEPTH_BUFFER,
		gbuffData.DSVDescTable[currOutIdx].CPUHandle(0));

	auto& tlas = const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS();

	data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::HDR_LIGHT_ACCUM,
		data.GpuDescTable.GPUDesciptorHeapIndex(LightData::DESC_TABLE::HDR_LIGHT_ACCUM_UAV));

	data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::RESERVOIR_A,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::SPATIAL_RESERVOIR_A));

	data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::RESERVOIR_B,
		rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::SPATIAL_RESERVOIR_B));

	if (settings.Inscattering && tlas.IsInitialized())
	{
		data.CompositingPass.SetInscatteringEnablement(true);

		const float p = data.SkyPass.GetVoxelGridMappingExp();
		float zNear;
		float zFar;
		data.SkyPass.GetVoxelGridDepth(zNear, zFar);

		data.CompositingPass.SetVoxelGridMappingExp(p);
		data.CompositingPass.SetVoxelGridDepth(zNear, zFar);
		data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::INSCATTERING,
			data.GpuDescTable.GPUDesciptorHeapIndex(LightData::DESC_TABLE::INSCATTERING_SRV));
	}
	else
		data.CompositingPass.SetInscatteringEnablement(false);

	if (settings.IndirectDiffuseDenoiser != DENOISER::NONE)
	{
		data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::DENOISED_L_IND,
			rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::STAD_TEMPORAL_CACHE));
	}
}

void Light::Register(const RenderSettings& settings, const RayTracerData& rayTracerData, 
	LightData& data, RenderGraph& renderGraph) noexcept
{
	renderGraph.RegisterResource(data.HdrLightAccumTex.GetResource(), data.HdrLightAccumTex.GetPathID());
	auto& tlas = const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS();

	// sky view lut + inscattering
	if (tlas.IsInitialized())
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SkyPass,
			&Sky::Render);
		data.SkyHandle = renderGraph.RegisterRenderPass("Sky",
			RENDER_NODE_TYPE::ASYNC_COMPUTE, dlg);

		if (settings.Inscattering)
		{
			auto& voxelGrid = data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING);
			renderGraph.RegisterResource(voxelGrid.GetResource(), voxelGrid.GetPathID(), D3D12_RESOURCE_STATE_COMMON, false);
		}
	}

	auto& skyviewLUT = data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT);
	renderGraph.RegisterResource(skyviewLUT.GetResource(), skyviewLUT.GetPathID(), D3D12_RESOURCE_STATE_COMMON, false);

	// sun light
	if (settings.SunLighting && tlas.IsInitialized())
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SunLightPass,
			&SunLight::Render);
		data.SunLightHandle = renderGraph.RegisterRenderPass("SunLight",
			RENDER_NODE_TYPE::RENDER, dlg);
	}

	// skydome
	if(tlas.IsInitialized())
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SkyDomePass,
			&SkyDome::Render);
		data.SkyDomeHandle = renderGraph.RegisterRenderPass("SkyDome",
			RENDER_NODE_TYPE::RENDER, dlg);
	}

	// compositing
	fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.CompositingPass,
		&Compositing::Render);
	data.CompositingHandle = renderGraph.RegisterRenderPass("Compositing",
		RENDER_NODE_TYPE::COMPUTE, dlg);

	// when more than one RenderPass writes to one resource, it's unclear which one should run first.
	// add a made-up resource so that Post-processing runs after Compositing
	renderGraph.RegisterResource(nullptr, RenderGraph::DUMMY_RES::RES_2);
}

void Light::DeclareAdjacencies(const RenderSettings& settings, const GBufferData& gbuffData, 
	const RayTracerData& rayTracerData, LightData& lightData, RenderGraph& renderGraph) noexcept
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

	// sun light
	if (settings.SunLighting && tlas.IsInitialized())
	{
		// RT-AS
		renderGraph.AddInput(lightData.SunLightHandle,
			tlas.GetPathID(),
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		// make sure it runs post gbuffer
		renderGraph.AddInput(lightData.SunLightHandle,
			gbuffData.BaseColor[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		// other gbuffers are implied

		renderGraph.AddOutput(lightData.SunLightHandle,
			lightData.HdrLightAccumTex.GetPathID(),
			D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	// sky dome
	// make sure it runs post gbuffer
	if (tlas.IsInitialized())
	{
		renderGraph.AddInput(lightData.SkyDomeHandle,
			gbuffData.BaseColor[outIdx].GetPathID(),
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
	if (tlas.IsInitialized())
	{
		// sky
		if (settings.Inscattering)
		{
			renderGraph.AddInput(lightData.CompositingHandle,
				lightData.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}

		if (settings.IndirectDiffuseDenoiser == DENOISER::STAD)
		{
			renderGraph.AddInput(lightData.CompositingHandle,
				rayTracerData.StadPass.GetOutput(STAD::SHADER_OUT_RES::SPATIAL_FILTER_OUT).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}
	}

	renderGraph.AddInput(lightData.CompositingHandle, lightData.HdrLightAccumTex.GetPathID(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	if (settings.RTIndirectDiffuse && const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS().IsInitialized())
	{
		renderGraph.AddInput(lightData.CompositingHandle,
			rayTracerData.ReSTIR_GIPass.GetOutput(ReSTIR_GI::SHADER_OUT_RES::SPATIAL_RESERVOIR_A).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(lightData.CompositingHandle,
			rayTracerData.ReSTIR_GIPass.GetOutput(ReSTIR_GI::SHADER_OUT_RES::SPATIAL_RESERVOIR_B).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
	}

	renderGraph.AddOutput(lightData.CompositingHandle, lightData.HdrLightAccumTex.GetPathID(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// [hack] use D3D12_RESOURCE_STATE_UNORDERED_ACCESS, which can be considered as both readable and 
	// writable to avoid a resource transition
	renderGraph.AddOutput(lightData.CompositingHandle, RenderGraph::DUMMY_RES::RES_2,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}


