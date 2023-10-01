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
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Scene;

//--------------------------------------------------------------------------------------
// RayTracer
//--------------------------------------------------------------------------------------

void RayTracer::Init(const RenderSettings& settings, RayTracerData& data)
{
	data.WndConstDescTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate(
		(int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::COUNT);
	data.PerFrameDescTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate(
		(int)RayTracerData::DESC_TABLE_PER_FRAME::COUNT);

	// init samplers (async)
	data.RtSampler.InitLowDiscrepancyBlueNoise32();

	data.ReSTIR_GI_DiffusePass.Init();

	// specular indirect
	data.ReSTIR_GI_SpecularPass.Init();

	const Texture& specularDnsrTex = data.ReSTIR_GI_SpecularPass.GetOutput(ReSTIR_GI_Specular::SHADER_OUT_RES::CURR_DNSR_CACHE);
	Direct3DUtil::CreateTexture2DSRV(specularDnsrTex, data.WndConstDescTable.CPUHandle(
		(int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SPECULAR_INDIRECT_DENOISED));

	if (settings.SkyIllumination)
	{
		data.SkyDI_Pass.Init();

		const Texture& skyDIDnsrTex = data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED);
		Direct3DUtil::CreateTexture2DSRV(skyDIDnsrTex, data.WndConstDescTable.CPUHandle(
			(int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SKY_DI_DENOISED));
	}
}

void RayTracer::OnWindowSizeChanged(const RenderSettings& settings, RayTracerData& data)
{
	if (data.ReSTIR_GI_DiffusePass.IsInitialized())
		data.ReSTIR_GI_DiffusePass.OnWindowResized();
	if (data.ReSTIR_GI_SpecularPass.IsInitialized())
	{
		data.ReSTIR_GI_SpecularPass.OnWindowResized();

		const Texture& t = data.ReSTIR_GI_SpecularPass.GetOutput(ReSTIR_GI_Specular::SHADER_OUT_RES::CURR_DNSR_CACHE);
		Direct3DUtil::CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle(
			(int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SPECULAR_INDIRECT_DENOISED));
	}
	if (data.SkyDI_Pass.IsInitialized())
	{
		data.SkyDI_Pass.OnWindowResized();

		const Texture& t = data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED);
		Direct3DUtil::CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle(
			(int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SKY_DI_DENOISED));
	}
}

void RayTracer::Shutdown(RayTracerData& data)
{
	data.WndConstDescTable.Reset();
	data.PerFrameDescTable.Reset();
	data.RtAS.Clear();
	data.RtSampler.Clear();
	data.ReSTIR_GI_DiffusePass.Reset();
	data.ReSTIR_GI_SpecularPass.Reset();
	data.SkyDI_Pass.Reset();
}

void RayTracer::UpdateDescriptors(const RenderSettings& settings, RayTracerData& data)
{
	data.PerFrameDescTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate(
		(int)RayTracerData::DESC_TABLE_PER_FRAME::COUNT);

	auto funcDiffuse = [&data](ReSTIR_GI_Diffuse::SHADER_OUT_RES r, RayTracerData::DESC_TABLE_PER_FRAME d)
	{
		const Texture& t = data.ReSTIR_GI_DiffusePass.GetOutput(r);
		Direct3DUtil::CreateTexture2DSRV(t, data.PerFrameDescTable.CPUHandle((int)d));
	};

	// temporal cache changes every frame due to ping-ponging
	funcDiffuse(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_A, RayTracerData::DESC_TABLE_PER_FRAME::DIFFUSE_TEMPORAL_RESERVOIR_A);
	funcDiffuse(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_B, RayTracerData::DESC_TABLE_PER_FRAME::DIFFUSE_TEMPORAL_RESERVOIR_B);
	funcDiffuse(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_A, RayTracerData::DESC_TABLE_PER_FRAME::DIFFUSE_SPATIAL_RESERVOIR_A);
	funcDiffuse(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_B, RayTracerData::DESC_TABLE_PER_FRAME::DIFFUSE_SPATIAL_RESERVOIR_B);
	funcDiffuse(ReSTIR_GI_Diffuse::SHADER_OUT_RES::DNSR_TEMPORAL_CACHE_POST_SPATIAL, RayTracerData::DESC_TABLE_PER_FRAME::DIFFUSE_INDIRECT_DENOISED);
}

void RayTracer::Update(const RenderSettings& settings, Core::RenderGraph& renderGraph, RayTracerData& data)
{
	if (settings.SkyIllumination && !data.SkyDI_Pass.IsInitialized())
	{
		data.SkyDI_Pass.Init();

		const Texture& skyDIDnsrTex = data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED);
		Direct3DUtil::CreateTexture2DSRV(skyDIDnsrTex, data.WndConstDescTable.CPUHandle(
			(int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SKY_DI_DENOISED));
	}
	else if (!settings.SkyIllumination && data.SkyDI_Pass.IsInitialized())
	{
		uint64_t id = data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED).ID();
		renderGraph.RemoveResource(id);

		data.SkyDI_Pass.Reset();
	}

	UpdateDescriptors(settings, data);

	data.RtAS.BuildStaticBLASTransforms();
	data.RtAS.BuildFrameMeshInstanceData();

	// TODO doesn't need to be called each frame
	auto& r = App::GetRenderer().GetSharedShaderResources();
	r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_SCENE_BVH, data.RtAS.GetTLAS());
}

void RayTracer::Register(const RenderSettings& settings, RayTracerData& data, RenderGraph& renderGraph)
{
	// Acceleration-structure build/rebuild/update
	fastdelegate::FastDelegate1<CommandList&> dlg1 = fastdelegate::MakeDelegate(&data.RtAS, &TLAS::Render);
	data.RtASBuildHandle = renderGraph.RegisterRenderPass("RT_AS_Build", RENDER_NODE_TYPE::ASYNC_COMPUTE, dlg1);

	auto& tlas = data.RtAS.GetTLAS();

	if (tlas.IsInitialized())
	{
		renderGraph.RegisterResource(tlas.Resource(), tlas.ID(), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
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
				renderGraph.RegisterResource(t.Resource(), t.ID());
			};

			auto registerInputs = [&data, &renderGraph](ReSTIR_GI_Diffuse::SHADER_IN_RES r)
			{
				Texture& t = const_cast<Texture&>(data.ReSTIR_GI_DiffusePass.GetInput(r));
				renderGraph.RegisterResource(t.Resource(), t.ID());
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
				renderGraph.RegisterResource(t.Resource(), t.ID());
			};

			auto registerInputs = [&data, &renderGraph](ReSTIR_GI_Specular::SHADER_IN_RES r)
			{
				Texture& t = const_cast<Texture&>(data.ReSTIR_GI_SpecularPass.GetInput(r));
				renderGraph.RegisterResource(t.Resource(), t.ID());
			};

			registerInputs(ReSTIR_GI_Specular::SHADER_IN_RES::PREV_DNSR_CACHE);
			registerOutputs(ReSTIR_GI_Specular::SHADER_OUT_RES::CURR_DNSR_CACHE);
		}

		// sky DI
		if (settings.SkyIllumination)
		{
			auto registerOutputs = [&data, &renderGraph](SkyDI::SHADER_OUT_RES r)
			{
				// Direct3D api doesn't accept const pointers
				Texture& t = const_cast<Texture&>(data.SkyDI_Pass.GetOutput(r));
				renderGraph.RegisterResource(t.Resource(), t.ID());
			};

			fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.SkyDI_Pass, &SkyDI::Render);
			data.SkyDI_Handle = renderGraph.RegisterRenderPass("SkyDI", RENDER_NODE_TYPE::COMPUTE, dlg2);

			registerOutputs(SkyDI::SHADER_OUT_RES::DENOISED);
		}
	}
}

void RayTracer::DeclareAdjacencies(const RenderSettings& settings, RayTracerData& data, const GBufferData& gbuffData, RenderGraph& renderGraph)
{
	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();
	auto& tlas = data.RtAS.GetTLAS();

	if (tlas.IsInitialized())
	{
		renderGraph.AddOutput(data.RtASBuildHandle,
			tlas.ID(),
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		// diffuse indirect
		{
			// RT-AS
			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				data.RtAS.GetTLAS().ID(),
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

			// prev gbuffers
			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.DepthBuffer[1 - outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.Normal[1 - outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// current gbuffers
			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.Normal[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.MetallicRoughness[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.DepthBuffer[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.MotionVec.ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// prev. temporal reservoirs
			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetInput(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_A).ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetInput(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_B).ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetInput(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_C).ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// denoiser temporal cache
			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetInput(ReSTIR_GI_Diffuse::SHADER_IN_RES::PREV_DNSR_TEMPORAL_CACHE).ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// current temporal reservoirs
			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_A).ID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_B).ID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::TEMPORAL_RESERVOIR_C).ID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// current spatial reservoirs
			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_A).ID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_B).ID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::SPATIAL_RESERVOIR_C).ID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// denoiser output
			renderGraph.AddOutput(data.ReSTIR_GI_DiffuseHandle,
				data.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::DNSR_TEMPORAL_CACHE_PRE_SPATIAL).ID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}

		// specular indirect
		{
			// RT-AS
			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				data.RtAS.GetTLAS().ID(),
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

			// prev gbuffers
			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.DepthBuffer[1 - outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.Normal[1 - outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_DiffuseHandle,
				gbuffData.MetallicRoughness[1 - outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// current gbuffers
			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				gbuffData.Normal[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				gbuffData.MetallicRoughness[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				gbuffData.DepthBuffer[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				gbuffData.MotionVec.ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				gbuffData.BaseColor[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				gbuffData.Curvature.ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// dnsr
			renderGraph.AddInput(data.ReSTIR_GI_SpecularHandle,
				data.ReSTIR_GI_SpecularPass.GetInput(ReSTIR_GI_Specular::SHADER_IN_RES::PREV_DNSR_CACHE).ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddOutput(data.ReSTIR_GI_SpecularHandle,
				data.ReSTIR_GI_SpecularPass.GetOutput(ReSTIR_GI_Specular::SHADER_OUT_RES::CURR_DNSR_CACHE).ID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}

		// sky DI
		if (settings.SkyIllumination)
		{
			// RT-AS
			renderGraph.AddInput(data.SkyDI_Handle,
				data.RtAS.GetTLAS().ID(),
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

			// prev gbuffers
			renderGraph.AddInput(data.SkyDI_Handle,
				gbuffData.DepthBuffer[1 - outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.SkyDI_Handle,
				gbuffData.Normal[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.SkyDI_Handle,
				gbuffData.MetallicRoughness[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.SkyDI_Handle,
				gbuffData.DepthBuffer[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.SkyDI_Handle,
				gbuffData.MotionVec.ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.SkyDI_Handle,
				gbuffData.BaseColor[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.SkyDI_Handle,
				gbuffData.BaseColor[1 - outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.SkyDI_Handle,
				gbuffData.Curvature.ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// output reservoirs
			renderGraph.AddOutput(data.SkyDI_Handle,
				data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED).ID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}
	}
}
