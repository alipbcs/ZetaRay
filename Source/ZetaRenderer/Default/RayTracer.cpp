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
	// allocate descriptor tables
	data.WndConstDescTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate(
		(int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::COUNT);
	data.ConstDescTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate(
		(int)RayTracerData::DESC_TABLE_CONST::COUNT);

	// init samplers (async)
	// data.RtSampler.InitLowDiscrepancyBlueNoise32();

	// sun shadow
	data.SunShadowPass.Init();

	Direct3DUtil::CreateTexture2DSRV(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::DENOISED),
		data.WndConstDescTable.CPUHandle((int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SUN_SHADOW_DENOISED));

	// inscattering + sku-view lut
	data.SkyPass.Init(RayTracerData::SKY_LUT_WIDTH, RayTracerData::SKY_LUT_HEIGHT, settings.Inscattering);

	Direct3DUtil::CreateTexture2DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT),
		data.ConstDescTable.CPUHandle((int)RayTracerData::DESC_TABLE_CONST::ENV_MAP_SRV));

	if (settings.Inscattering)
	{
		Direct3DUtil::CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
			data.ConstDescTable.CPUHandle((int)RayTracerData::DESC_TABLE_CONST::INSCATTERING_SRV));
	}

	data.PreLightingPass.Init();
	
	if (settings.EmissiveLighting)
	{
		data.DirecLightingPass.Init();

		const Texture& t = data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED);
		Direct3DUtil::CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle(
			(int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::DIRECT_LIGHITNG_DENOISED));
	}

	if (settings.SkyIllumination)
	{
		data.SkyDI_Pass.Init();

		const Texture& denoised = data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED);
		Direct3DUtil::CreateTexture2DSRV(denoised, data.WndConstDescTable.CPUHandle(
			(int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SKY_DI_DENOISED));
	}
}

void RayTracer::OnWindowSizeChanged(const RenderSettings& settings, RayTracerData& data)
{
	// GPU is flushed after resize, safe to reuse descriptors

	if (data.SunShadowPass.IsInitialized())
	{
		data.SunShadowPass.OnWindowResized();

		Direct3DUtil::CreateTexture2DSRV(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::DENOISED),
			data.WndConstDescTable.CPUHandle((int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SUN_SHADOW_DENOISED));
	}

	data.PreLightingPass.OnWindowResized();

	if (settings.EmissiveLighting && App::GetScene().NumEmissiveInstances())
	{
		data.DirecLightingPass.OnWindowResized();

		const Texture& t = data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED);
		Direct3DUtil::CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle(
			(int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::DIRECT_LIGHITNG_DENOISED));
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
	data.ConstDescTable.Reset();
	data.WndConstDescTable.Reset();
	data.RtAS.Clear();
	data.RtSampler.Clear();
	data.SkyDI_Pass.Reset();
	data.SunShadowPass.Reset();
	data.SkyPass.Reset();
	data.DirecLightingPass.Reset();
	data.PreLightingPass.Reset();
}

void RayTracer::Update(const RenderSettings& settings, Core::RenderGraph& renderGraph, RayTracerData& data)
{
	if (settings.Inscattering && !data.SkyPass.IsInscatteringEnabled())
	{
		data.SkyPass.SetInscatteringEnablement(true);

		Direct3DUtil::CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
			data.ConstDescTable.CPUHandle((int)RayTracerData::DESC_TABLE_CONST::INSCATTERING_SRV));
	}
	else if (!settings.Inscattering && data.SkyPass.IsInscatteringEnabled())
		data.SkyPass.SetInscatteringEnablement(false);

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

	data.RtAS.BuildStaticBLASTransforms();
	data.RtAS.BuildFrameMeshInstanceData();

	// TODO doesn't need to be called each frame
	auto& r = App::GetRenderer().GetSharedShaderResources();
	r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_SCENE_BVH, data.RtAS.GetTLAS());

	data.PreLightingPass.Update(settings.EmissiveLighting);
	const bool lightPresampling = data.PreLightingPass.IsPresamplingEnabled();

	// recompute alias table only if there are stale emissives
	if (settings.EmissiveLighting && App::GetScene().NumEmissiveInstances())
	{
		if (!data.DirecLightingPass.IsInitialized())
		{
			data.DirecLightingPass.Init();

			const Texture& t = data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED);
			Direct3DUtil::CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle(
				(int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::DIRECT_LIGHITNG_DENOISED));
		}

		data.DirecLightingPass.SetLightPresamplingEnabled(data.PreLightingPass.IsPresamplingEnabled(),
			data.PreLightingPass.GetNumSampleSets(), data.PreLightingPass.GetSampleSetSize());

		if (App::GetScene().AreEmissivesStale())
		{
			auto& readback = data.PreLightingPass.GetLumenReadbackBuffer();
			data.EmissiveAliasTable.Update(&readback);
			data.EmissiveAliasTable.SetRelaseBuffersDlg(data.PreLightingPass.GetReleaseBuffersDlg());
		}
	}
}

void RayTracer::Register(const RenderSettings& settings, RayTracerData& data, RenderGraph& renderGraph)
{
	// Acceleration-structure rebuild/update
	{
		fastdelegate::FastDelegate1<CommandList&> dlg1 = fastdelegate::MakeDelegate(&data.RtAS, &TLAS::Render);
		data.RtASBuildHandle = renderGraph.RegisterRenderPass("RT_AS_Build", RENDER_NODE_TYPE::COMPUTE, dlg1);
	}

	const bool tlasReady = data.RtAS.IsReady();

	// sky view lut + inscattering
	if (tlasReady)
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SkyPass, &Sky::Render);
		data.SkyHandle = renderGraph.RegisterRenderPass("Sky", RENDER_NODE_TYPE::COMPUTE, dlg);

		auto& skyviewLUT = const_cast<Texture&>(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT));
		renderGraph.RegisterResource(skyviewLUT.Resource(), skyviewLUT.ID(), D3D12_RESOURCE_STATE_COMMON, false);

		if (settings.Inscattering)
		{
			auto& voxelGrid = const_cast<Texture&>(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING));
			renderGraph.RegisterResource(voxelGrid.Resource(), voxelGrid.ID(), D3D12_RESOURCE_STATE_COMMON, false);
		}
	}

	if (tlasReady)
	{
		auto& tlas = const_cast<DefaultHeapBuffer&>(data.RtAS.GetTLAS());
		renderGraph.RegisterResource(tlas.Resource(), tlas.ID(), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			false);
	}

	// sun shadow
	if (tlasReady)
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SunShadowPass,
			&SunShadow::Render);
		data.SunShadowHandle = renderGraph.RegisterRenderPass("SunShadow", RENDER_NODE_TYPE::COMPUTE, dlg);

		Texture& denoised = const_cast<Texture&>(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::DENOISED));
		renderGraph.RegisterResource(denoised.Resource(), denoised.ID());
	}

	// pre lighting
	fastdelegate::FastDelegate1<CommandList&> dlg1 = fastdelegate::MakeDelegate(&data.PreLightingPass,
		&PreLighting::Render);
	data.PreLightingPassHandle = renderGraph.RegisterRenderPass("PreLighting", RENDER_NODE_TYPE::COMPUTE, dlg1);

	// direct lighting
	if (settings.EmissiveLighting && App::GetScene().NumEmissiveInstances())
	{
		// read back emissive lumens and compute the alias table on CPU
		if (App::GetScene().AreEmissivesStale())
		{
			auto& triLumenBuff = data.PreLightingPass.GetLumenBuffer();
			renderGraph.RegisterResource(const_cast<DefaultHeapBuffer&>(triLumenBuff).Resource(), triLumenBuff.ID(), 
				D3D12_RESOURCE_STATE_COPY_SOURCE, false);

			// make sure to use a seperate command list
			fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.EmissiveAliasTable,
				&EmissiveTriangleAliasTable::Render);
			data.EmissiveAliasTableHandle = renderGraph.RegisterRenderPass("EmissiveAliasTable", RENDER_NODE_TYPE::COMPUTE, dlg2, true);

			auto& aliasTable = data.EmissiveAliasTable.GetOutput(EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE);
			renderGraph.RegisterResource(aliasTable.Resource(), aliasTable.ID(), D3D12_RESOURCE_STATE_COMMON, false);

			data.EmissiveAliasTable.SetEmissiveTriPassHandle(data.PreLightingPassHandle);
		}

		if (tlasReady)
		{
			fastdelegate::FastDelegate1<CommandList&> dlg3 = fastdelegate::MakeDelegate(&data.DirecLightingPass,
				&DirectLighting::Render);
			data.DirecLightingHandle = renderGraph.RegisterRenderPass("DirectLighting", RENDER_NODE_TYPE::COMPUTE, dlg3);

			Texture& t = const_cast<Texture&>(data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED));
			renderGraph.RegisterResource(t.Resource(), t.ID());
		}
	}

	// sky DI
	if (settings.SkyIllumination && tlasReady)
	{
		fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.SkyDI_Pass, &SkyDI::Render);
		data.SkyDI_Handle = renderGraph.RegisterRenderPass("SkyDI", RENDER_NODE_TYPE::COMPUTE, dlg2);

		Texture& t = const_cast<Texture&>(data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED));
		renderGraph.RegisterResource(t.Resource(), t.ID());
	}
}

void RayTracer::DeclareAdjacencies(const RenderSettings& settings, RayTracerData& data, const GBufferData& gbuffData, 
	RenderGraph& renderGraph)
{
	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();
	const bool tlasReady = data.RtAS.IsReady();
	const auto tlasID = tlasReady ? data.RtAS.GetTLAS().ID() : -1;
	const auto numEmissives = App::GetScene().NumEmissiveInstances();

	// RT_AS
	if (tlasReady)
	{
		renderGraph.AddOutput(data.RtASBuildHandle,
			tlasID,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	}

	// inscattering + sky-view lut
	if (tlasReady)
	{
		// RT_AS
		renderGraph.AddInput(data.SkyHandle,
			tlasID,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		renderGraph.AddOutput(data.SkyHandle,
			data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT).ID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		if (settings.Inscattering)
		{
			renderGraph.AddOutput(data.SkyHandle,
				data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING).ID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}
	}

	// direct lighting
	if (settings.EmissiveLighting && numEmissives)
	{
		if (App::GetScene().AreEmissivesStale())
		{
			const auto& triLumenBuff = data.PreLightingPass.GetLumenBuffer();

			renderGraph.AddOutput(data.PreLightingPassHandle,
				triLumenBuff.ID(),
				D3D12_RESOURCE_STATE_COPY_SOURCE);

			renderGraph.AddInput(data.EmissiveAliasTableHandle,
				triLumenBuff.ID(),
				D3D12_RESOURCE_STATE_COPY_SOURCE);

			renderGraph.AddOutput(data.EmissiveAliasTableHandle,
				data.EmissiveAliasTable.GetOutput(EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE).ID(),
				D3D12_RESOURCE_STATE_COPY_DEST);
		}

		if (tlasReady)
		{
			if (App::GetScene().AreEmissivesStale())
			{
				renderGraph.AddInput(data.DirecLightingHandle,
					data.EmissiveAliasTable.GetOutput(EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE).ID(),
					D3D12_RESOURCE_STATE_COPY_DEST);
			}

			// RT-AS
			renderGraph.AddInput(data.DirecLightingHandle,
				tlasID,
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

			// prev gbuffers
			renderGraph.AddInput(data.DirecLightingHandle,
				gbuffData.DepthBuffer[1 - outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DirecLightingHandle,
				gbuffData.Normal[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DirecLightingHandle,
				gbuffData.Normal[1 - outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DirecLightingHandle,
				gbuffData.MetallicRoughness[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DirecLightingHandle,
				gbuffData.MetallicRoughness[1 - outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DirecLightingHandle,
				gbuffData.DepthBuffer[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DirecLightingHandle,
				gbuffData.MotionVec.ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DirecLightingHandle,
				gbuffData.BaseColor[outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.DirecLightingHandle,
				gbuffData.BaseColor[1 - outIdx].ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// output
			renderGraph.AddOutput(data.DirecLightingHandle,
				data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED).ID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}
	}

	// sun shadow
	if (tlasReady)
	{
		// RT_AS
		renderGraph.AddInput(data.SunShadowHandle,
			tlasID,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		// make sure it runs post gbuffer
		renderGraph.AddInput(data.SunShadowHandle,
			gbuffData.DepthBuffer[outIdx].ID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.SunShadowHandle,
			gbuffData.DepthBuffer[1 - outIdx].ID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.SunShadowHandle,
			gbuffData.Normal[outIdx].ID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.SunShadowHandle,
			gbuffData.MotionVec.ID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddOutput(data.SunShadowHandle,
			data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::DENOISED).ID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	// sky DI
	if (settings.SkyIllumination && tlasReady)
	{
		// RT-AS
		renderGraph.AddInput(data.SkyDI_Handle,
			tlasID,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		// prev g-buffers
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

		// denoised output
		renderGraph.AddOutput(data.SkyDI_Handle,
			data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED).ID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
}
