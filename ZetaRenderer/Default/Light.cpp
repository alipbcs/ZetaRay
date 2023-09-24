#include "DefaultRendererImpl.h"
#include <App/App.h>
#include <App/Timer.h>
#include <Core/SharedShaderResources.h>

using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::DefaultRenderer;
using namespace ZetaRay::Util;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Scene;

void Light::Init(const RenderSettings& settings, LightData& data)
{
	auto& renderer = App::GetRenderer();
	data.HdrLightAccumRTV = renderer.GetRtvDescriptorHeap().Allocate(1);
	
	// descriptor talbes
	data.ConstDescTable = renderer.GetGpuDescriptorHeap().Allocate((int)LightData::DESC_TABLE_CONST::COUNT);
	data.WndConstDescTable = renderer.GetGpuDescriptorHeap().Allocate((int)LightData::DESC_TABLE_WND_SIZE_CONST::COUNT);
	data.PerFrameDescTable = renderer.GetGpuDescriptorHeap().Allocate((int)LightData::DESC_TABLE_PER_FRAME::COUNT);

	// sun shadow
	data.SunShadowPass.Init();

	// compositing
	data.CompositingPass.Init(settings.SkyIllumination);
	const Texture& lightAccum = data.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED);
	// RTV
	Direct3DUtil::CreateRTV(lightAccum, data.HdrLightAccumRTV.CPUHandle(0));

	if (settings.Inscattering)
	{
		Direct3DUtil::CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
			data.ConstDescTable.CPUHandle((int)LightData::DESC_TABLE_CONST::INSCATTERING_SRV));
	}

	// sky dome
	data.SkyDomePass.Init(lightAccum.Desc().Format);

	// inscattering + sku-view lut
	data.SkyPass.Init(LightData::SKY_LUT_WIDTH, LightData::SKY_LUT_HEIGHT, settings.Inscattering);
	Direct3DUtil::CreateTexture2DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT),
		data.ConstDescTable.CPUHandle((int)LightData::DESC_TABLE_CONST::ENV_MAP_SRV));

	// direct lighting
	if (settings.EmissiveLighting)
	{
		data.EmissiveTriLumen.Init();
		data.DirecLightingPass.Init();

		const Texture& t = data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED);
		Direct3DUtil::CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle((int)LightData::DESC_TABLE_WND_SIZE_CONST::DENOISED_DIRECT_LIGHITNG));
	}
}

void Light::OnWindowSizeChanged(const RenderSettings& settings, LightData& data)
{
	data.CompositingPass.OnWindowResized();

	// RTV
	const Texture& lightAccum = data.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED);
	Direct3DUtil::CreateRTV(lightAccum, data.HdrLightAccumRTV.CPUHandle(0));

	if (data.SunShadowPass.IsInitialized())
		data.SunShadowPass.OnWindowResized();

	if (settings.EmissiveLighting && App::GetScene().NumEmissiveInstances())
	{
		data.DirecLightingPass.OnWindowResized();

		const Texture& t = data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED);
		Direct3DUtil::CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle((int)LightData::DESC_TABLE_WND_SIZE_CONST::DENOISED_DIRECT_LIGHITNG));
	}
}

void Light::Shutdown(LightData& data)
{
	data.HdrLightAccumRTV.Reset();
	data.ConstDescTable.Reset();
	data.WndConstDescTable.Reset();
	data.PerFrameDescTable.Reset();
	data.CompositingPass.Reset();
	data.SunShadowPass.Reset();
	data.SkyDomePass.Reset();
	data.SkyPass.Reset();
	data.DirecLightingPass.Reset();
}

void Light::Update(const RenderSettings& settings, LightData& data, const GBufferData& gbuffData,
	const RayTracerData& rayTracerData)
{
	if (settings.Inscattering && !data.SkyPass.IsInscatteringEnabled())
	{
		data.SkyPass.SetInscatteringEnablement(true);

		Direct3DUtil::CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
			data.ConstDescTable.CPUHandle((int)LightData::DESC_TABLE_CONST::INSCATTERING_SRV));
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
		// diffuse indirect
		data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::DIFFUSE_DNSR_CACHE,
			rayTracerData.PerFrameDescTable.GPUDesciptorHeapIndex((int)RayTracerData::DESC_TABLE_PER_FRAME::DIFFUSE_INDIRECT_DENOISED));

		// specular indirect
		data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::SPECULAR_DNSR_CACHE,
			rayTracerData.WndConstDescTable.GPUDesciptorHeapIndex((int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SPECULAR_INDIRECT_DENOISED));

		// sky DI
		if (settings.SkyIllumination)
		{
			data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::SKY_DI_DENOISED,
				rayTracerData.WndConstDescTable.GPUDesciptorHeapIndex((int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SKY_DI_DENOISED));
		}

		// sun shadow temporal cache changes every frame
		data.PerFrameDescTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate((int)LightData::DESC_TABLE_PER_FRAME::COUNT);

		Direct3DUtil::CreateTexture2DSRV(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::TEMPORAL_CACHE_OUT_POST),
			data.PerFrameDescTable.CPUHandle((int)LightData::DESC_TABLE_PER_FRAME::DENOISED_SHADOW_MASK));

		data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::SUN_SHADOW,
			data.PerFrameDescTable.GPUDesciptorHeapIndex((int)LightData::DESC_TABLE_PER_FRAME::DENOISED_SHADOW_MASK));

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
				data.ConstDescTable.GPUDesciptorHeapIndex((int)LightData::DESC_TABLE_CONST::INSCATTERING_SRV));
		}
		else
			data.CompositingPass.SetInscatteringEnablement(false);
	}

	// recompute alias table only if there are stale emissives
	if (settings.EmissiveLighting && App::GetScene().NumEmissiveInstances())
	{
		if (!data.DirecLightingPass.IsInitialized())
		{
			data.DirecLightingPass.Init();

			const Texture& t = data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED);
			Direct3DUtil::CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle((int)LightData::DESC_TABLE_WND_SIZE_CONST::DENOISED_DIRECT_LIGHITNG));
		}

		data.EmissiveTriLumen.Update();
		data.DirecLightingPass.Update();

		if (App::GetScene().AreEmissivesStale())
		{
			auto& readback = data.EmissiveTriLumen.GetReadbackBuffer();
			data.EmissiveAliasTable.Update(&readback);
		}

		data.CompositingPass.SetGpuDescriptor(Compositing::SHADER_IN_GPU_DESC::EMISSIVE_DI_DENOISED,
			data.WndConstDescTable.GPUDesciptorHeapIndex((int)LightData::DESC_TABLE_WND_SIZE_CONST::DENOISED_DIRECT_LIGHITNG));
	}
}

void Light::Register(const RenderSettings& settings, LightData& data, const RayTracerData& rayTracerData,
	RenderGraph& renderGraph)
{
	Texture& lightAccum = const_cast<Texture&>(data.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED));
	renderGraph.RegisterResource(lightAccum.Resource(), lightAccum.ID());

	auto& tlas = const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS();
	const bool isTLASbuilt = tlas.IsInitialized();

	// sky view lut + inscattering
	if (isTLASbuilt)
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SkyPass, &Sky::Render);
		data.SkyHandle = renderGraph.RegisterRenderPass("Sky", RENDER_NODE_TYPE::COMPUTE, dlg);

		if (settings.Inscattering)
		{
			auto& voxelGrid = data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING);
			renderGraph.RegisterResource(voxelGrid.Resource(), voxelGrid.ID(), D3D12_RESOURCE_STATE_COMMON, false);
		}
	}

	auto& skyviewLUT = data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT);
	renderGraph.RegisterResource(skyviewLUT.Resource(), skyviewLUT.ID(), D3D12_RESOURCE_STATE_COMMON, false);

	// sun shadow
	if (isTLASbuilt)
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SunShadowPass,
			&SunShadow::Render);
		data.SunShadowHandle = renderGraph.RegisterRenderPass("SunShadow", RENDER_NODE_TYPE::COMPUTE, dlg);

		Texture& mask = const_cast<Texture&>(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::RAW_SHADOW_MASK));
		renderGraph.RegisterResource(mask.Resource(), mask.ID());

		Texture& tcA = const_cast<Texture&>(data.SunShadowPass.GetInput(SunShadow::SHADER_IN_RES::TEMPORAL_CACHE_IN));
		renderGraph.RegisterResource(tcA.Resource(), tcA.ID());

		Texture& tcB = const_cast<Texture&>(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::TEMPORAL_CACHE_OUT_PRE));
		renderGraph.RegisterResource(tcB.Resource(), tcB.ID());
	}

	// skydome
	if(isTLASbuilt)
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SkyDomePass, &SkyDome::Render);
		data.SkyDomeHandle = renderGraph.RegisterRenderPass("SkyDome", RENDER_NODE_TYPE::RENDER, dlg);
	}

	// direct lighting
	if (settings.EmissiveLighting && App::GetScene().NumEmissiveInstances())
	{
		if (App::GetScene().AreEmissivesStale())
		{
			fastdelegate::FastDelegate1<CommandList&> dlg1 = fastdelegate::MakeDelegate(&data.EmissiveTriLumen,
				&EmissiveTriangleLumen::Render);
			data.EmissiveTriLumenHandle = renderGraph.RegisterRenderPass("EmissiveTriLumen", RENDER_NODE_TYPE::COMPUTE, dlg1, true);

			auto& triLumenBuff = data.EmissiveTriLumen.GetLumenBuffer();
			renderGraph.RegisterResource(triLumenBuff.Resource(), triLumenBuff.ID(), D3D12_RESOURCE_STATE_COPY_SOURCE, false);

			fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.EmissiveAliasTable,
				&EmissiveTriangleAliasTable::Render);
			data.EmissiveAliasTableHandle = renderGraph.RegisterRenderPass("EmissiveAliasTable", RENDER_NODE_TYPE::COMPUTE, dlg2, true);

			auto& aliasTable = data.EmissiveAliasTable.GetOutput(EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE);
			renderGraph.RegisterResource(aliasTable.Resource(), aliasTable.ID(), D3D12_RESOURCE_STATE_COMMON, false);

			data.EmissiveAliasTable.SetEmissiveTriPassHandle(data.EmissiveTriLumenHandle);
		}

		if (isTLASbuilt)
		{
			fastdelegate::FastDelegate1<CommandList&> dlg3 = fastdelegate::MakeDelegate(&data.DirecLightingPass,
				&DirectLighting::Render);
			data.DirecLightingHandle = renderGraph.RegisterRenderPass("DirectLighting", RENDER_NODE_TYPE::COMPUTE, dlg3);

			Texture& t = const_cast<Texture&>(data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED));
			renderGraph.RegisterResource(t.Resource(), t.ID());
		}
	}

	// compositing
	fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.CompositingPass,
		&Compositing::Render);
	data.CompositingHandle = renderGraph.RegisterRenderPass("Compositing", RENDER_NODE_TYPE::COMPUTE, dlg);
}

void Light::DeclareAdjacencies(const RenderSettings& settings, LightData& data, const GBufferData& gbuffData,
	const RayTracerData& rayTracerData, RenderGraph& renderGraph)
{
	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();
	auto& tlas = const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS();
	const bool isTLASbuilt = tlas.IsInitialized();
	const auto numEmissives = App::GetScene().NumEmissiveInstances();

	// inscattering + sky-view lut
	if (settings.Inscattering && isTLASbuilt)
	{
		// RT_AS
		renderGraph.AddInput(data.SkyHandle,
			const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS().ID(),
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		renderGraph.AddOutput(data.SkyHandle,
			data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING).ID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		renderGraph.AddOutput(data.SkyHandle,
			data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT).ID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	// direct lighting
	if (settings.EmissiveLighting && numEmissives)
	{
		if (App::GetScene().AreEmissivesStale())
		{
			const auto& triLumenBuff = data.EmissiveTriLumen.GetLumenBuffer();

			renderGraph.AddOutput(data.EmissiveTriLumenHandle,
				triLumenBuff.ID(),
				D3D12_RESOURCE_STATE_COPY_SOURCE);

			renderGraph.AddInput(data.EmissiveAliasTableHandle,
				triLumenBuff.ID(),
				D3D12_RESOURCE_STATE_COPY_SOURCE);

			renderGraph.AddOutput(data.EmissiveAliasTableHandle,
				data.EmissiveAliasTable.GetOutput(EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE).ID(),
				D3D12_RESOURCE_STATE_COPY_DEST);
		}

		if (isTLASbuilt)
		{
			if (App::GetScene().AreEmissivesStale())
			{
				renderGraph.AddInput(data.DirecLightingHandle,
					data.EmissiveAliasTable.GetOutput(EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE).ID(),
					D3D12_RESOURCE_STATE_COPY_DEST);
			}

			// RT-AS
			renderGraph.AddInput(data.DirecLightingHandle,
				const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS().ID(),
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
				gbuffData.BaseColor.ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			//renderGraph.AddInput(data.DirecLightingHandle,
			//	gbuffData.Curvature.ID(),
			//	D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// output reservoirs
			renderGraph.AddOutput(data.DirecLightingHandle,
				data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED).ID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}
	}

	// sun shadow
	if (isTLASbuilt)
	{
		// RT_AS
		renderGraph.AddInput(data.SunShadowHandle,
			tlas.ID(),
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
			data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::RAW_SHADOW_MASK).ID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		renderGraph.AddInput(data.SunShadowHandle,
			data.SunShadowPass.GetInput(SunShadow::SHADER_IN_RES::TEMPORAL_CACHE_IN).ID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddOutput(data.SunShadowHandle,
			data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::TEMPORAL_CACHE_OUT_PRE).ID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	// sky dome
	if (isTLASbuilt)
	{
		// make sure it runs post gbuffer
		renderGraph.AddInput(data.SkyDomeHandle, 
			gbuffData.Normal[outIdx].ID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.SkyDomeHandle,
			data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT).ID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddOutput(data.SkyDomeHandle,
			gbuffData.DepthBuffer[outIdx].ID(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE);

		renderGraph.AddOutput(data.SkyDomeHandle,
			data.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED).ID(),
			D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	// compositing
	renderGraph.AddInput(data.CompositingHandle,
		gbuffData.BaseColor.ID(),
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

	renderGraph.AddInput(data.CompositingHandle,
		gbuffData.Normal[outIdx].ID(),
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

	renderGraph.AddInput(data.CompositingHandle,
		gbuffData.DepthBuffer[outIdx].ID(),
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

	renderGraph.AddInput(data.CompositingHandle,
		gbuffData.MetallicRoughness[outIdx].ID(),
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

	if (isTLASbuilt)
	{
		// sun shadows
		renderGraph.AddInput(data.CompositingHandle,
			data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::TEMPORAL_CACHE_OUT_POST).ID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(data.CompositingHandle,
			data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::RAW_SHADOW_MASK).ID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		// indirect diffuse
		renderGraph.AddInput(data.CompositingHandle,
			rayTracerData.ReSTIR_GI_DiffusePass.GetOutput(ReSTIR_GI_Diffuse::SHADER_OUT_RES::DNSR_TEMPORAL_CACHE_POST_SPATIAL).ID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		// indirect specular
		renderGraph.AddInput(data.CompositingHandle,
			rayTracerData.ReSTIR_GI_SpecularPass.GetOutput(ReSTIR_GI_Specular::SHADER_OUT_RES::CURR_DNSR_CACHE).ID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		// sky di
		if (settings.SkyIllumination)
		{
			renderGraph.AddInput(data.CompositingHandle,
				rayTracerData.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED).ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}

		// emissive di
		if (settings.EmissiveLighting && numEmissives)
		{
			renderGraph.AddInput(data.CompositingHandle,
				data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED).ID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}

		// inscattering
		if (settings.Inscattering)
		{
			renderGraph.AddInput(data.CompositingHandle,
				data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING).ID(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
	}

	renderGraph.AddOutput(data.CompositingHandle, 
		data.CompositingPass.GetOutput(Compositing::SHADER_OUT_RES::COMPOSITED).ID(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
