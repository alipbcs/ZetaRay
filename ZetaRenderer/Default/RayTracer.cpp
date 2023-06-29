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

	// init samplers (async)
	data.RtSampler.InitLowDiscrepancyBlueNoise();

	data.ReSTIR_GI_DiffusePass.Init();
	data.ReSTIR_GI_SpecularPass.Init();

	if(settings.SkyIllumination)
		data.ReSTIR_DI_Pass.Init();
}

void RayTracer::OnWindowSizeChanged(const RenderSettings& settings, RayTracerData& data) noexcept
{
	if (data.ReSTIR_GI_DiffusePass.IsInitialized())
		data.ReSTIR_GI_DiffusePass.OnWindowResized();
	if (data.ReSTIR_GI_SpecularPass.IsInitialized())
		data.ReSTIR_GI_SpecularPass.OnWindowResized();
	if (data.ReSTIR_DI_Pass.IsInitialized())
		data.ReSTIR_DI_Pass.OnWindowResized();
}

void RayTracer::Shutdown(RayTracerData& data) noexcept
{
	data.DescTableAll.Reset();
	data.RtAS.Clear();
	data.RtSampler.Clear();
	data.ReSTIR_GI_DiffusePass.Reset();
	data.ReSTIR_GI_SpecularPass.Reset();
	data.ReSTIR_DI_Pass.Reset();
}

void RayTracer::UpdateDescriptors(const RenderSettings& settings, RayTracerData& data) noexcept
{
	data.DescTableAll = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(RayTracerData::DESC_TABLE::COUNT);

	auto funcDiffuse = [&data](ReSTIR_GI_Diffuse::SHADER_OUT_RES r, RayTracerData::DESC_TABLE d)
	{
		const Texture& t = data.ReSTIR_GI_DiffusePass.GetOutput(r);
		Direct3DHelper::CreateTexture2DSRV(t, data.DescTableAll.CPUHandle(d));
	};

	funcDiffuse(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_A, RayTracerData::DESC_TABLE::DIFFUSE_TEMPORAL_RESERVOIR_A);
	funcDiffuse(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_B, RayTracerData::DESC_TABLE::DIFFUSE_TEMPORAL_RESERVOIR_B);
	funcDiffuse(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_C, RayTracerData::DESC_TABLE::DIFFUSE_TEMPORAL_RESERVOIR_C);
	funcDiffuse(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_A, RayTracerData::DESC_TABLE::DIFFUSE_SPATIAL_RESERVOIR_A);
	funcDiffuse(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_B, RayTracerData::DESC_TABLE::DIFFUSE_SPATIAL_RESERVOIR_B);
	funcDiffuse(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_C, RayTracerData::DESC_TABLE::DIFFUSE_SPATIAL_RESERVOIR_C);
	// temporal cache changes every frame due to ping-ponging
	funcDiffuse(ReSTIR_GI_Diffuse::SHADER_OUT_RES::DNSR_TEMPORAL_CACHE_POST_SPATIAL, RayTracerData::DESC_TABLE::DIFFUSE_DNSR_TEMPORAL_CACHE);

	auto funcSpecular = [&data](ReSTIR_GI_Specular::SHADER_OUT_RES r, RayTracerData::DESC_TABLE d)
	{
		const Texture& t = data.ReSTIR_GI_SpecularPass.GetOutput(r);
		Direct3DHelper::CreateTexture2DSRV(t, data.DescTableAll.CPUHandle(d));
	};

	// temporal cache changes every frame due to ping-ponging
	funcSpecular(ReSTIR_GI_Specular::SHADER_OUT_RES::CURR_DNSR_CACHE, RayTracerData::DESC_TABLE::SPECULAR_DNSR_TEMPORAL_CACHE);

	if (settings.SkyIllumination)
	{
		auto funcDI = [&data](ReSTIR_DI::SHADER_OUT_RES r, RayTracerData::DESC_TABLE d)
		{
			const Texture& t = data.ReSTIR_DI_Pass.GetOutput(r);
			Direct3DHelper::CreateTexture2DSRV(t, data.DescTableAll.CPUHandle(d));
		};

		funcDI(ReSTIR_DI::SHADER_OUT_RES::DENOISED, RayTracerData::DESC_TABLE::DIRECT_DNSR_TEMPORAL_CACHE);
	}
}

void RayTracer::Update(const RenderSettings& settings, Core::RenderGraph& renderGraph, RayTracerData& data) noexcept
{
	if (settings.SkyIllumination && !data.ReSTIR_DI_Pass.IsInitialized())
		data.ReSTIR_DI_Pass.Init();
	else if (!settings.SkyIllumination && data.ReSTIR_DI_Pass.IsInitialized())
	{
		uint64_t id = data.ReSTIR_DI_Pass.GetOutput(ReSTIR_DI::SHADER_OUT_RES::DENOISED).GetPathID();
		renderGraph.RemoveResource(id);

		data.ReSTIR_DI_Pass.Reset();
	}

	UpdateDescriptors(settings, data);

	data.RtAS.BuildStaticBLASTransforms();
	data.RtAS.BuildFrameMeshInstanceData();

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
		{
			fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.ReSTIR_GI_DiffusePass,
				&ReSTIR_GI_Diffuse::Render);
			data.ReSTIR_GI_DiffuseHandle = renderGraph.RegisterRenderPass("ReSTIR_GI_Diffuse", RENDER_NODE_TYPE::COMPUTE, dlg2);

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
			registerInputs(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_DNSR_TEMPORAL_CACHE);
			registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_A);
			registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_B);
			registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_C);
			registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_A);
			registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_B);
			registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_C);
			registerOutputs(ReSTIR_GI_Diffuse::SHADER_OUT_RES::DNSR_TEMPORAL_CACHE_PRE_SPATIAL);
		}

		// indirect specular
		{
			fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.ReSTIR_GI_SpecularPass,
				&ReSTIR_GI_Specular::Render);
			data.ReSTIR_GI_SpecularHandle = renderGraph.RegisterRenderPass("ReSTIR_GI_Specular", RENDER_NODE_TYPE::COMPUTE, dlg2);

			auto registerOutputs = [&data, &renderGraph](ReSTIR_GI_Specular::SHADER_OUT_RES r)
			{
				// Direct3D api doesn't accept const pointers
				Texture& t = const_cast<Texture&>(data.ReSTIR_GI_SpecularPass.GetOutput(r));
				renderGraph.RegisterResource(t.GetResource(), t.GetPathID());
			};

			auto registerInputs = [&data, &renderGraph](ReSTIR_GI_Specular::SHADER_IN_RES r)
			{
				Texture& t = const_cast<Texture&>(data.ReSTIR_GI_SpecularPass.GetInput(r));
				renderGraph.RegisterResource(t.GetResource(), t.GetPathID());
			};

			registerInputs(ReSTIR_GI_Specular::SHADER_IN_RES::PREV_DNSR_CACHE);
			registerOutputs(ReSTIR_GI_Specular::SHADER_OUT_RES::CURR_DNSR_CACHE);
		}

		// restir DI
		if (settings.SkyIllumination)
		{
			auto registerOutputs = [&data, &renderGraph](ReSTIR_DI::SHADER_OUT_RES r)
			{
				// Direct3D api doesn't accept const pointers
				Texture& t = const_cast<Texture&>(data.ReSTIR_DI_Pass.GetOutput(r));
				renderGraph.RegisterResource(t.GetResource(), t.GetPathID());
			};

			fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.ReSTIR_DI_Pass, &ReSTIR_DI::Render);
			data.ReSTIR_DI_Handle = renderGraph.RegisterRenderPass("ReSTIR_DI", RENDER_NODE_TYPE::COMPUTE, dlg2);

			registerOutputs(ReSTIR_DI::SHADER_OUT_RES::DENOISED);
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
		{
			// RT-AS
			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				data.RtAS.GetTLAS().GetPathID(),
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

			// prev gbuffers
			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.DepthBuffer[1 - outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.Normal[1 - outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// current gbuffers
			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.Normal[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.MetalnessRoughness[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.DepthBuffer[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.MotionVec.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// prev. temporal reservoirs
			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetInput(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_A).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetInput(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_B).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetInput(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_C).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// denoiser temporal cache
			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetInput(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_DNSR_TEMPORAL_CACHE).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// current temporal reservoirs
			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_A).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_B).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_C).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// current spatial reservoirs
			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_A).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_B).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_C).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// denoiser output
			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::DNSR_TEMPORAL_CACHE_PRE_SPATIAL).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}

		// indirect specular
		{
			// RT-AS
			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				data.RtAS.GetTLAS().GetPathID(),
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

			// prev gbuffers
			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.DepthBuffer[1 - outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.Normal[1 - outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.MetalnessRoughness[1 - outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// current gbuffers
			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				gbuffData.Normal[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				gbuffData.MetalnessRoughness[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				gbuffData.DepthBuffer[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				gbuffData.MotionVec.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				gbuffData.BaseColor.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				gbuffData.Curvature.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// dnsr
			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				data.ReSTIR_GI_SpecularPass.GetInput(ReSTIR_GI_Specular::SHADER_IN_RES::PREV_DNSR_CACHE).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddOutput(data.ReSTIR_GI_SpecularHandle,
				data.ReSTIR_GI_SpecularPass.GetOutput(ReSTIR_GI_Specular::SHADER_OUT_RES::CURR_DNSR_CACHE).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}

		// restir DI
		if (settings.SkyIllumination)
		{
			// RT-AS
			renderGraph.AddInput(data.ReSTIR_DI_Handle,
				data.RtAS.GetTLAS().GetPathID(),
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

			// prev gbuffers
			renderGraph.AddInput(data.ReSTIR_DI_Handle,
				gbuffData.DepthBuffer[1 - outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_DI_Handle,
				gbuffData.Normal[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_DI_Handle,
				gbuffData.MetalnessRoughness[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_DI_Handle,
				gbuffData.DepthBuffer[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_DI_Handle,
				gbuffData.MotionVec.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_DI_Handle,
				gbuffData.BaseColor.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_DI_Handle,
				gbuffData.Curvature.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// output reservoirs
			renderGraph.AddOutput(data.ReSTIR_DI_Handle,
				data.ReSTIR_DI_Pass.GetOutput(ReSTIR_DI::SHADER_OUT_RES::DENOISED).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}
	}
}


