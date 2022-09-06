#include "SceneRendererImpl.h"
#include "../../Win32/App.h"
#include "../../Win32/Timer.h"
#include "../../Core/Direct3DHelpers.h"
#include "../../Core/SharedShaderResources.h"

using namespace ZetaRay;
using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Win32;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Util;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DHelper;
using namespace ZetaRay::Scene::Settings;

void LightManager::Init(const RenderSettings& settings, LightManagerData& data) noexcept
{
	auto& renderer = App::GetRenderer();
	data.HdrLightAccumRTV = renderer.GetRtvDescriptorHeap().Allocate(1);
	data.GpuDescTable = renderer.GetCbvSrvUavDescriptorHeapGpu().Allocate(LightManagerData::DESC_TABLE::COUNT);

	CreateHDRLightAccumTex(data);

	DXGI_FORMAT rtvFormats[1] = { LightManagerData::HDR_LIGHT_ACCUM_FORMAT };
	
	// sun light
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
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DHelper::GetPSODesc(&VertexPosNormalTexTangent::InputLayout,
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
	data.SkyPass.Init(LightManagerData::SKY_LUT_WIDTH, LightManagerData::SKY_LUT_HEIGHT, settings.Inscattering);

	// descriptors
	CreateTexture2DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT), 
		data.GpuDescTable.CPUHandle(LightManagerData::DESC_TABLE::ENV_MAP_SRV));

	if (settings.Inscattering)
	{
		CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
			data.GpuDescTable.CPUHandle(LightManagerData::DESC_TABLE::INSCATTERING_SRV));
	}
}

void LightManager::CreateHDRLightAccumTex(LightManagerData& data) noexcept
{
	auto& renderer = App::GetRenderer();
	auto* device = renderer.GetDevice();
	auto& gpuMem = renderer.GetGpuMemory();
	const int width = renderer.GetRenderWidth();
	const int height = renderer.GetRenderHeight();

	D3D12_CLEAR_VALUE clearValue = {};
	memset(clearValue.Color, 0, sizeof(float) * 4);
	clearValue.Format = LightManagerData::HDR_LIGHT_ACCUM_FORMAT;

	data.HdrLightAccumTex = ZetaMove(gpuMem.GetTexture2D("LightManager/HDRLightAccum",
		width, height,
		LightManagerData::HDR_LIGHT_ACCUM_FORMAT,
		D3D12_RESOURCE_STATE_COMMON,
		TEXTURE_FLAGS::ALLOW_RENDER_TARGET | TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS, 1, &clearValue));

	// RTV
	CreateRTV(data.HdrLightAccumTex, data.HdrLightAccumRTV.CPUHandle(0));

	// UAV
	CreateTexture2DUAV(data.HdrLightAccumTex, data.GpuDescTable.CPUHandle(LightManagerData::DESC_TABLE::HDR_LIGHT_ACCUM_UAV));
}

void LightManager::OnWindowSizeChanged(const RenderSettings& settings, LightManagerData& data) noexcept
{
	CreateHDRLightAccumTex(data);
}

void LightManager::Shutdown(LightManagerData& data) noexcept
{
	//data.AnalyticalAliasTableBuff.Reset();
	//data.AnalyticalLightBuff.Reset();
	//data.AnalyticalLightSources.free();
	data.EmissiveAliasTable.Reset();
	data.EmissiveTrianglesBuff.Reset();
	data.EmissiveTriangles.free();
	//data.EnvLightTex.Reset();
	//data.EnvMapAliasTableBuffer.Reset();
	//data.EnvMapPatchBuffer.Reset();
	data.HdrLightAccumRTV.Reset();
	data.GpuDescTable.Reset();
	//data.EnvMapDescTable.Reset();
	//data.HdrLightAccumUAV.Reset();
	data.HdrLightAccumTex.Reset();
	data.CompositingPass.Reset();
	data.SunLightPass.Reset();
	data.SkyDomePass.Reset();
	data.SkyPass.Reset();
}

void LightManager::SetEnvMap(LightManagerData& data, const Filesystem::Path& pathToEnvLight, 
	const Filesystem::Path& pathToPatches) noexcept
{
	/*
	auto& renderer = App::GetRenderer();
	auto& gpuMem = renderer.GetGpuMemory();

	// load the texture
	data.EnvLightTex = renderer.GetGpuMemory().GetTextureFromDisk(pathToEnvLight.Get());

	// create the descriptor
	CreateTexture2DSRV(data.EnvLightTex, data.GpuDescTable.CPUHandle(LightManagerData::DESC_TABLE::ENV_MAP_SRV));

	//
	// read in the patch info
	//
	SmallVector<float> patchProbs;

	{
		SmallVector<uint8_t> patcheFile;
		Filesystem::LoadFromFile(pathToPatches.Get(), patcheFile);

		// header
		const char header[] = "EnvMapPatches";
		Check(patcheFile.size() > sizeof(header), "Invalid patch file.");
		Check(strcmp(header, (char*)patcheFile.data()) == 0, "Invalid header");

		uint8_t* ptr = patcheFile.data() + sizeof(header) + 1;

		// patch metadata
		memcpy(&data.EnvLightDesc.NumPatches, ptr, sizeof(int));
		ptr += sizeof(int);

		memcpy(&data.EnvLightDesc.Pdf, ptr, sizeof(float));
		ptr += sizeof(float);

		memcpy(&data.EnvLightDesc.dPhi, ptr, sizeof(float));
		ptr += sizeof(float);

		Check(data.EnvLightDesc.NumPatches < LightManagerData::MAX_NUM_ENV_LIGHT_PATCHES,
			"Number of patches exceeded maximum allowed.");

		// patch array
		EnvMapPatch patches[LightManagerData::MAX_NUM_ENV_LIGHT_PATCHES];
		memcpy(patches, ptr, data.EnvLightDesc.NumPatches * sizeof(EnvMapPatch));
		patchProbs.resize(data.EnvLightDesc.NumPatches);

		for (uint32_t i = 0; i < data.EnvLightDesc.NumPatches; i++)
		{
			patchProbs[i] = patches[i].Prob;
		}

#ifdef _DEBUG
		float cdf = 0.0f;

		for (uint32_t i = 0; i < data.EnvLightDesc.NumPatches; i++)
		{
			cdf += patches[i].Prob;
		}

		Assert(fabs(1.0f - cdf) < 1e-6f, "Invalid probability distribution function.");
#endif // _DEBUG

		// copy the patch data to a GPU buffer
		data.EnvMapPatchBuffer = gpuMem.GetDefaultHeapBufferAndInit(SceneRenderer::ENV_LIGHT_PATCH_BUFFER,
			sizeof(EnvMapPatch) * data.EnvLightDesc.NumPatches,
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, false, patches);
	}

	// alias table
	{
		//Vector<AliasTableEntry> aliasTable;
		//Math::BuildAliasTableNormalized(ZetaMove(patchProbs), aliasTable);

		//data.EnvMapAliasTableBuffer = gpuMem.GetDefaultHeapBufferAndInit(SceneRenderer::ENV_MAP_ALIAS_TABLE,
		//	sizeof(float) * data.EnvLightDesc.NumPatches,
		//	D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, false, aliasTable.begin());
	}

	auto& r = App::GetRenderer().GetSharedShaderResources();

	r.InsertOrAssignDefaultHeapBuffer(SceneRenderer::ENV_LIGHT_PATCH_BUFFER, data.EnvMapPatchBuffer);
	r.InsertOrAssignDefaultHeapBuffer(SceneRenderer::ENV_MAP_ALIAS_TABLE, data.EnvMapAliasTableBuffer);
	*/
}

//void LightManager::AddEmissiveTriangle(LightManagerData& data, uint64_t instanceID, Vector<float, 32>&& lumen) noexcept
//{
//	data.EmissiveUpdateBatch.emplace_back(LightManagerData::EmissiveTriUpdateInstance{ 
//		.InstanceID = instanceID,
//		.Lumen = ZetaMove(lumen) });
//}

void LightManager::Update(const RenderSettings& settings, const GBufferRendererData& gbuffData, 
	const RayTracerData& rayTracerData, LightManagerData& data) noexcept
{
	//UpdateEmissiveTriangleBuffers(lightManagerData);
	//UpdateAnalyticalLightBuffers(data);

	if (settings.Inscattering && !data.SkyPass.IsInscatteringEnabled())
	{
		data.SkyPass.SetInscatteringEnablement(true);

		CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
			data.GpuDescTable.CPUHandle(LightManagerData::DESC_TABLE::INSCATTERING_SRV));
	}
	else if (!settings.Inscattering && data.SkyPass.IsInscatteringEnabled())
	{
		data.SkyPass.SetInscatteringEnablement(false);
	}

	const int currOutIdx = App::GetRenderer().CurrOutIdx();

	data.SunLightPass.SetCPUDescriptor(SunLight::RTV, data.HdrLightAccumRTV.CPUHandle(0));
	data.SunLightPass.SetCPUDescriptor(SunLight::DEPTH_BUFFER,
		gbuffData.DSVDescTable[currOutIdx].CPUHandle(0));
	
	// dsv changes every frame
	data.SkyDomePass.SetDescriptor(SkyDome::SHADER_IN_DESC::RTV, data.HdrLightAccumRTV.CPUHandle(0));
	data.SkyDomePass.SetDescriptor(SkyDome::SHADER_IN_DESC::DEPTH_BUFFER,
		gbuffData.DSVDescTable[currOutIdx].CPUHandle(0));

	auto& tlas = const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS();

	data.CompositingPass.SetGPUDescriptor(Compositing::SHADER_IN_GPU_DESC::HDR_LIGHT_ACCUM,
		data.GpuDescTable.GPUDesciptorHeapIndex(LightManagerData::DESC_TABLE::HDR_LIGHT_ACCUM_UAV));

	if (settings.Inscattering && tlas.IsInitialized())
	{
		data.CompositingPass.SetInscatteringEnablement(true);

		const float p = data.SkyPass.GetVoxelGridMappingExp();
		float zNear;
		float zFar;
		data.SkyPass.GetVoxelGridDepth(zNear, zFar);

		data.CompositingPass.SetVoxelGridMappingExp(p);
		data.CompositingPass.SetVoxelGridDepth(zNear, zFar);
		data.CompositingPass.SetGPUDescriptor(Compositing::SHADER_IN_GPU_DESC::INSCATTERING,
			data.GpuDescTable.GPUDesciptorHeapIndex(LightManagerData::DESC_TABLE::INSCATTERING_SRV));
	}
	else
		data.CompositingPass.SetInscatteringEnablement(false);

	data.CompositingPass.SetIndirectDiffusDenoiser(settings.IndirectDiffuseDenoiser);
	
	if (settings.IndirectDiffuseDenoiser != DENOISER::NONE)
	{
		data.CompositingPass.SetGPUDescriptor(Compositing::SHADER_IN_GPU_DESC::DENOISED_L_IND,
			rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::TEMPORAL_CACHE));
	}
}

void LightManager::Register(const RenderSettings& settings, const RayTracerData& rayTracerData, 
	LightManagerData& data, RenderGraph& renderGraph) noexcept
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

void LightManager::DeclareAdjacencies(const RenderSettings& settings, const GBufferRendererData& gbuffData, 
	const RayTracerData& rayTracerData, LightManagerData& lightManagerData, RenderGraph& renderGraph) noexcept
{
	const int outIdx = App::GetRenderer().CurrOutIdx();
	auto& tlas = const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS();

	// inscattering + sky-view lut
	if (settings.Inscattering && tlas.IsInitialized())
	{
		// RT-AS
		renderGraph.AddInput(lightManagerData.SkyHandle,
			const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS().GetPathID(),
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		renderGraph.AddOutput(lightManagerData.SkyHandle,
			lightManagerData.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		renderGraph.AddOutput(lightManagerData.SkyHandle,
			lightManagerData.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT).GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	// sun light
	if (settings.SunLighting && tlas.IsInitialized())
	{
		// RT-AS
		renderGraph.AddInput(lightManagerData.SunLightHandle,
			tlas.GetPathID(),
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		// make sure it runs post gbuffer
		renderGraph.AddInput(lightManagerData.SunLightHandle,
			gbuffData.BaseColor[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		// other gbuffers are implied

		renderGraph.AddOutput(lightManagerData.SunLightHandle,
			lightManagerData.HdrLightAccumTex.GetPathID(),
			D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	// sky dome
	// make sure it runs post gbuffer
	if (tlas.IsInitialized())
	{
		renderGraph.AddInput(lightManagerData.SkyDomeHandle,
			gbuffData.BaseColor[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		renderGraph.AddInput(lightManagerData.SkyDomeHandle,
			lightManagerData.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT).GetPathID(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		renderGraph.AddOutput(lightManagerData.SkyDomeHandle,
			gbuffData.DepthBuffer[outIdx].GetPathID(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE);

		renderGraph.AddOutput(lightManagerData.SkyDomeHandle,
			lightManagerData.HdrLightAccumTex.GetPathID(),
			D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	// compositing
	if (tlas.IsInitialized())
	{
		// sky
		if (settings.Inscattering)
		{
			renderGraph.AddInput(lightManagerData.CompositingHandle,
				lightManagerData.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}

		if (settings.IndirectDiffuseDenoiser == DENOISER::STAD)
		{
			renderGraph.AddInput(lightManagerData.CompositingHandle,
				rayTracerData.StadPass.GetOutput(STAD::SHADER_OUT_RES::TEMPORAL_CACHE_PRE_OUT).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}
	}

	renderGraph.AddInput(lightManagerData.CompositingHandle,
		lightManagerData.HdrLightAccumTex.GetPathID(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	renderGraph.AddOutput(lightManagerData.CompositingHandle,
		lightManagerData.HdrLightAccumTex.GetPathID(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// [hack] use D3D12_RESOURCE_STATE_UNORDERED_ACCESS, which can be considered as both readable and 
	// writable to avoid a resource ransition
	renderGraph.AddOutput(lightManagerData.CompositingHandle, RenderGraph::DUMMY_RES::RES_2,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	/*
	else
	{
		renderGraph.AddInput(lightManagerData.CompositPassHandle,
			rayTracerData.IndirectDiffusePass.GetOutput(IndirectDiffuse::COLOR).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
	}
	*/
}
/*
void LightManager::UpdateAnalyticalLightBuffers(LightManagerData& data) noexcept
{
	auto& renderer = App::GetRenderer();
	Scene& scene = App::GetScene();

	if (data.AnalyticalLightBufferIsStale)
	{
		size_t numLights = data.AnalyticalLightSources.size();
		Assert(numLights > 0, "0 light sources");

		data.AnalyticalLightBuff = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit(
			SceneRenderer::ANALYTICAL_LIGHTS_BUFFER_NAME,
			sizeof(AnalyticalLightSource) * numLights,
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
			false,
			data.AnalyticalLightSources.begin());

		//Vector<float, 32> probs;
		//probs.resize(lights.size());

		//for (int i = 0; i < lights.size(); i++)
		//{
		//	probs[i] = lights[i].LuminousPower;
		//}

		//Vector<AliasTableEntry> aliasTable;
		//Math::BuildAliasTableUnnormalized(ZetaMove(probs), aliasTable);

		//data.AnalyticalAliasTableBuff = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit(
		//	SceneRenderer::ANALYTICAL_LIGHTS_ALIAS_TABLE_BUFFER_NAME,
		//	sizeof(AliasTableEntry) * aliasTable.size(),
		//	D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
		//	false,
		//	aliasTable.begin());

		auto& r = App::GetRenderer().GetSharedShaderResources();
		r.InsertOrAssignDefaultHeapBuffer(SceneRenderer::ANALYTICAL_LIGHTS_BUFFER_NAME, data.AnalyticalLightBuff);
		//r.InsertOrAssignDefaultHeapBuffer(SceneRenderer::ANALYTICAL_LIGHTS_ALIAS_TABLE_BUFFER_NAME, data.AnalyticalAliasTableBuff);

		data.AnalyticalLightBufferIsStale = false;
	}
}
*/

// Updates the GPU Buffer that contains all the emissive triangles
void LightManager::UpdateEmissiveTriangleBuffers(LightManagerData& data) noexcept
{
	/*
	if (data.EmissiveUpdateBatch.empty())
		return;
	
	auto& renderer = App::GetRenderer();
	Scene& scene = App::GetScene();

	int addedNumTriangles = 0;

	for (auto& e : data.EmissiveUpdateBatch)
	{
		addedNumTriangles += (int)e.Lumen.size();
	}

	const size_t oldNumEmissives = data.EmissiveTriangles.size();
	const size_t newNumEmissives = addedNumTriangles + data.EmissiveTriangles.size();
	data.EmissiveTriangles.resize(newNumEmissives);

	size_t currTri = data.EmissiveTriangles.size();

	// for every emissive mesh
	for (auto& e : data.EmissiveUpdateBatch)
	{
		uint64_t meshID = scene.GetMeshIDForInstance(e.InstanceID);
		MeshData d = scene.GetMeshData(meshID);
		Material mat = scene.GetMaterial(d.MatID);

		Assert(e.Lumen.size() == (int)(d.NumIndices / 3), "Mimatch between #triangles and corresponding triangle emissive powers");

		// for every triangle of this mesh
		for (int i = 0; i < e.Lumen.size(); i++)
		{
			data.EmissiveTriangles[currTri].DescHeapIdx = d.DescHeapIdx;
			data.EmissiveTriangles[currTri].PrimitiveIdx = i;
			data.EmissiveTriangles[currTri].EmissiveMapIdx = mat.EmissiveTexture;
			data.EmissiveTriangles[currTri].Lumen = e.Lumen[i];

			currTri++;
		}
	}

	currTri = oldNumEmissives;

	// for every emissive mesh
	for (auto& e : data.EmissiveUpdateBatch)
	{
		float4x3 W = scene.GetToWorld(e.InstanceID);
		uint32_t currDescHeapIdx = data.EmissiveTriangles[currTri].DescHeapIdx;

		// for every triangle of that mesh
		while (data.EmissiveTriangles[currTri].DescHeapIdx == currDescHeapIdx)
		{
			data.EmissiveTriangles[currTri].SR = (float3x3)W;
			data.EmissiveTriangles[currTri].Translation = W.m[3];

			currTri++;
		}
	}

	// releases the old buffer (with proper fence)
	data.EmissiveTrianglesBuff = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit(
		SceneRenderer::EMISSIVE_TRIANGLES_BUFFER_NAME,
		sizeof(EmissiveTriangle) * newNumEmissives,
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
		false,
		data.EmissiveTriangles.begin());

	data.EmissiveUpdateBatch.clear();
	//updates.shrink_to_fit();

	SmallVector<float, GetExcessSize(sizeof(float), alignof(float)), 32> probs;
	probs.resize(data.EmissiveTriangles.size());

	for (int i = 0; i < data.EmissiveTriangles.size(); i++)
	{
		probs[i] = data.EmissiveTriangles[i].Lumen;
	}

	SmallVector<AliasTableEntry> aliasTable;
	Math::BuildAliasTableUnnormalized(ZetaMove(probs), aliasTable);

	data.EmissiveAliasTable = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit(
		SceneRenderer::EMISSIVE_TRIANGLES_ALIAS_TABLE_BUFFER_NAME,
		sizeof(AliasTableEntry) * aliasTable.size(),
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
		false,
		aliasTable.begin());

	// register the shared resources
	auto& r = App::GetRenderer().GetSharedShaderResources();
	r.InsertOrAssignDefaultHeapBuffer(SceneRenderer::EMISSIVE_TRIANGLES_BUFFER_NAME, data.EmissiveTrianglesBuff);
	r.InsertOrAssignDefaultHeapBuffer(SceneRenderer::EMISSIVE_TRIANGLES_ALIAS_TABLE_BUFFER_NAME, data.EmissiveAliasTable);

	*/
}


