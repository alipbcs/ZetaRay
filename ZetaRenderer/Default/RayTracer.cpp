#include "DefaultRendererImpl.h"
#include <Core/CommandList.h>
#include <Core/SharedShaderResources.h>
#include <App/App.h>

using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::DefaultRenderer;
using namespace ZetaRay::DefaultRenderer::Settings;
using namespace ZetaRay::Util;
using namespace ZetaRay::RT;
using namespace ZetaRay::Core;
using namespace ZetaRay::Scene;

//--------------------------------------------------------------------------------------
// RayTracer
//--------------------------------------------------------------------------------------

void RayTracer::Init(const RenderSettings& settings, RayTracerData& data) noexcept
{
	//data.DescTableAll = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(RayTracerData::DESC_TABLE::COUNT);

	// init sampler (async)
	data.RtSampler.InitLowDiscrepancyBlueNoise();
}

void RayTracer::OnWindowSizeChanged(const RenderSettings& settings, RayTracerData& data) noexcept
{
	if(data.ReSTIR_GI_DiffusePass.IsInitialized())
		data.ReSTIR_GI_DiffusePass.OnWindowResized();
	if(data.DiffuseDNSRPass.IsInitialized())
		data.DiffuseDNSRPass.OnWindowResized();
}

void RayTracer::Shutdown(RayTracerData& data) noexcept
{
	data.DescTableAll.Reset();
	data.RtAS.Clear();
	data.RtSampler.Clear();
	data.ReSTIR_GI_DiffusePass.Reset();
	data.DiffuseDNSRPass.Reset();
}

void RayTracer::UpdatePasses(const RenderSettings& settings, RayTracerData& data) noexcept
{
	if(!data.ReSTIR_GI_DiffusePass.IsInitialized())
		data.ReSTIR_GI_DiffusePass.Init();

	if (settings.IndirectDiffuseDenoiser == DENOISER::NONE)
		data.DiffuseDNSRPass.Reset();
	else if(settings.IndirectDiffuseDenoiser == DENOISER::STAD)
	{
		if (!data.DiffuseDNSRPass.IsInitialized())
			data.DiffuseDNSRPass.Init();
	}
}

void RayTracer::UpdateDescriptors(const RenderSettings& settings, RayTracerData& data) noexcept
{
	data.DescTableAll = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(RayTracerData::DESC_TABLE::COUNT);

	auto func = [&data](ReSTIR_GI_Diffuse::SHADER_OUT_RES r, RayTracerData::DESC_TABLE d)
	{
		const Texture& t = data.ReSTIR_GI_DiffusePass.GetOutput(r);
		Direct3DHelper::CreateTexture2DSRV(t, data.DescTableAll.CPUHandle(d));
	};

	func(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_A, RayTracerData::DESC_TABLE::TEMPORAL_RESERVOIR_A);
	func(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_B, RayTracerData::DESC_TABLE::TEMPORAL_RESERVOIR_B);
	func(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_C, RayTracerData::DESC_TABLE::TEMPORAL_RESERVOIR_C);
	func(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_A, RayTracerData::DESC_TABLE::SPATIAL_RESERVOIR_A);
	func(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_B, RayTracerData::DESC_TABLE::SPATIAL_RESERVOIR_B);
	func(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_C, RayTracerData::DESC_TABLE::SPATIAL_RESERVOIR_C);
	
	if (settings.IndirectDiffuseDenoiser == DENOISER::STAD)
	{
		// temporal cache changes every frame due to ping-ponging
		const Texture& temporalCache = data.DiffuseDNSRPass.GetOutput(DiffuseDNSR::SHADER_OUT_RES::SPATIAL_FILTER_OUT);
		Direct3DHelper::CreateTexture2DSRV(temporalCache, data.DescTableAll.CPUHandle(RayTracerData::DESC_TABLE::STAD_TEMPORAL_CACHE));
	}
}

void RayTracer::Update(const RenderSettings& settings, RayTracerData& data) noexcept
{
	UpdatePasses(settings, data);
	UpdateDescriptors(settings, data);

	data.RtAS.BuildFrameMeshInstanceData();

	if (settings.IndirectDiffuseDenoiser == DENOISER::STAD)
	{
		data.DiffuseDNSRPass.SetDescriptor(DiffuseDNSR::SHADER_IN_RES::RESTIR_GI_RESERVOIR_A,
			data.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::SPATIAL_RESERVOIR_A));
		data.DiffuseDNSRPass.SetDescriptor(DiffuseDNSR::SHADER_IN_RES::RESTIR_GI_RESERVOIR_B,
			data.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::SPATIAL_RESERVOIR_B));
	}

	// TODO doesn't need to be called each frame
	auto& r = App::GetRenderer().GetSharedShaderResources();
	r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_SCENE_BVH, data.RtAS.GetTLAS());
}

void RayTracer::Register(const RenderSettings& settings, RayTracerData& data, RenderGraph& renderGraph) noexcept
{
	// Acceleration-structure build/rebuild/update
	fastdelegate::FastDelegate1<CommandList&> dlg1 = fastdelegate::MakeDelegate(&data.RtAS, &TLAS::Render);
	data.RtASBuildHandle = renderGraph.RegisterRenderPass("RT_AS_Build", RENDER_NODE_TYPE::ASYNC_COMPUTE, dlg1);

	auto& tlas = data.RtAS.GetTLAS();
	
	if (tlas.IsInitialized())
	{
		renderGraph.RegisterResource(tlas.GetResource(), tlas.GetPathID(), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			false);

		// indirect diffuse
		fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.ReSTIR_GI_DiffusePass,
			&ReSTIR_GI_Diffuse::Render);
		data.ReSTIR_GIHandle = renderGraph.RegisterRenderPass("ReSTIR_GI_Diffuse", RENDER_NODE_TYPE::COMPUTE, dlg2);

		auto registerOutputs = [&data, &renderGraph](ReSTIR_GI_Diffuse::SHADER_OUT_RES r)
		{
			// Direct3D api doesn't accept const pointers
			Texture& t = const_cast<Texture&>(data.ReSTIR_GI_DiffusePass.GetOutput(r));
			renderGraph.RegisterResource(t.GetResource(), t.GetPathID());
		};

		auto registerInputs = [&data, &renderGraph](ReSTIR_GI_Diffuse::SHADER_IN_RES r)
		{
			Texture& t = const_cast<Texture&>(data.ReSTIR_GI_DiffusePass.GetInput(r));
			renderGraph.RegisterResource(t.GetResource(), t.GetPathID());
		};

		registerInputs(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_A);
		registerInputs(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_B);
		registerInputs(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_C);
		registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_A);
		registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_B);
		registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_C);
		registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_A);
		registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_B);
		registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_C);

		// STAD
		if (settings.IndirectDiffuseDenoiser == DENOISER::STAD)
		{
			fastdelegate::FastDelegate1<CommandList&> dlg3 = fastdelegate::MakeDelegate(&data.DiffuseDNSRPass, &DiffuseDNSR::Render);
			data.DiffuseDNSRHandle = renderGraph.RegisterRenderPass("DiffuseDNSR", RENDER_NODE_TYPE::COMPUTE, dlg3);

			// Direct3D api doesn't accept const pointers
			Texture& temporalCacheIn = const_cast<Texture&>(data.DiffuseDNSRPass.GetOutput(DiffuseDNSR::SHADER_OUT_RES::TEMPORAL_CACHE_IN));
			Texture& temporalCacheOut = const_cast<Texture&>(data.DiffuseDNSRPass.GetOutput(DiffuseDNSR::SHADER_OUT_RES::TEMPORAL_CACHE_OUT));
			renderGraph.RegisterResource(temporalCacheIn.GetResource(), temporalCacheIn.GetPathID());
			renderGraph.RegisterResource(temporalCacheOut.GetResource(), temporalCacheOut.GetPathID());
		}
	}
}

void RayTracer::DeclareAdjacencies(const RenderSettings& settings, RayTracerData& data, const GBufferData& gbuffData, RenderGraph& renderGraph) noexcept
{
	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();
	auto& tlas = data.RtAS.GetTLAS();

	if (tlas.IsInitialized())
	{
		renderGraph.AddOutput(data.RtASBuildHandle,
			tlas.GetPathID(),
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
		
		// indirect diffuse

		// RT-AS
		renderGraph.AddInput(data.ReSTIR_GIHandle,
			data.RtAS.GetTLAS().GetPathID(),
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		// current gbuffers
		renderGraph.AddInput(data.ReSTIR_GIHandle,
			gbuffData.Normal[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.ReSTIR_GIHandle,
			gbuffData.MetalnessRoughness[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.ReSTIR_GIHandle,
			gbuffData.EmissiveColor.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.ReSTIR_GIHandle,
			gbuffData.DepthBuffer[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		// prev. temporal reservoirs
		renderGraph.AddInput(data.ReSTIR_GIHandle,
			data.ReSTIR_GI_DiffusePass.GetInput(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_A).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.ReSTIR_GIHandle,
			data.ReSTIR_GI_DiffusePass.GetInput(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_B).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.ReSTIR_GIHandle,
			data.ReSTIR_GI_DiffusePass.GetInput(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_C).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		// current temporal reservoirs
		renderGraph.AddOutput(data.ReSTIR_GIHandle,
			data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_A).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		renderGraph.AddOutput(data.ReSTIR_GIHandle,
			data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_B).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		renderGraph.AddOutput(data.ReSTIR_GIHandle,
			data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_C).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		// current spatial reservoirs
		renderGraph.AddOutput(data.ReSTIR_GIHandle,
			data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_A).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		renderGraph.AddOutput(data.ReSTIR_GIHandle,
			data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_B).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		renderGraph.AddOutput(data.ReSTIR_GIHandle,
			data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_C).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		// STAD
		if (settings.IndirectDiffuseDenoiser == DENOISER::STAD)
		{
			renderGraph.AddInput(data.DiffuseDNSRHandle,
				gbuffData.Normal[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DiffuseDNSRHandle,
				gbuffData.DepthBuffer[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DiffuseDNSRHandle,
				gbuffData.Normal[1 - outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DiffuseDNSRHandle,
				gbuffData.DepthBuffer[1 - outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DiffuseDNSRHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_A).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DiffuseDNSRHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_B).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DiffuseDNSRHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_C).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DiffuseDNSRHandle,
				data.DiffuseDNSRPass.GetOutput(DiffuseDNSR::SHADER_OUT_RES::TEMPORAL_CACHE_IN).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddOutput(data.DiffuseDNSRHandle,
				data.DiffuseDNSRPass.GetOutput(DiffuseDNSR::SHADER_OUT_RES::TEMPORAL_CACHE_OUT).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			renderGraph.AddOutput(data.DiffuseDNSRHandle,
				data.DiffuseDNSRPass.GetOutput(DiffuseDNSR::SHADER_OUT_RES::SPATIAL_FILTER_OUT).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}
	}
}


