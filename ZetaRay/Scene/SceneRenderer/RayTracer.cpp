#include "SceneRendererImpl.h"
#include "../../Core/CommandList.h"
#include "../../Win32/App.h"
#include "../../Core/SharedShaderResources.h"

using namespace ZetaRay;
using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Scene::Settings;
using namespace ZetaRay::Util;
using namespace ZetaRay::RT;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DHelper;

//--------------------------------------------------------------------------------------
// RayTracer
//--------------------------------------------------------------------------------------

void RayTracer::Init(const RenderSettings& settings, RayTracerData& data) noexcept
{
	data.DescTableAll = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(RayTracerData::DESC_TABLE::COUNT);

	// init sampler (async)
	data.RtSampler.InitLowDiscrepancyBlueNoise();
}

void RayTracer::OnWindowSizeChanged(const RenderSettings& settings, RayTracerData& data) noexcept
{
	if(data.IndirectDiffusePass.IsInitialized())
		data.IndirectDiffusePass.OnWindowResized();
	if(data.SvgfPass.IsInitialized())
		data.SvgfPass.OnWindowResized();
	if(data.StadPass.IsInitialized())
		data.StadPass.OnWindowResized();
}

void RayTracer::Shutdown(RayTracerData& data) noexcept
{
	data.DescTableAll.Reset();
	data.RtAS.Clear();
	data.RtSampler.Clear();
	data.IndirectDiffusePass.Reset();
	data.SvgfPass.Reset();
	data.StadPass.Reset();
}

void RayTracer::UpdatePasses(const RenderSettings& settings, RayTracerData& data) noexcept
{
	if (!settings.RTIndirectDiffuse)
		data.IndirectDiffusePass.Reset();
	else if (settings.RTIndirectDiffuse && !data.IndirectDiffusePass.IsInitialized())
	{
		data.IndirectDiffusePass.Init();

		const Texture& indirectDiffuseLiTex = data.IndirectDiffusePass.GetOutput(IndirectDiffuse::SHADER_OUT_RES::INDIRECT_LI);
		CreateTexture2DSRV(indirectDiffuseLiTex, data.DescTableAll.CPUHandle(RayTracerData::DESC_TABLE::INDIRECT_LI));
	}

	if (settings.IndirectDiffuseDenoiser == DENOISER::NONE)
	{
		data.SvgfPass.Reset();
		data.StadPass.Reset();
	}
	else if (settings.IndirectDiffuseDenoiser == DENOISER::SVGF)
	{
		data.StadPass.Reset();

		if (!data.SvgfPass.IsInitialized())
			data.SvgfPass.Init();
	}
	else if(settings.IndirectDiffuseDenoiser == DENOISER::STAD)
	{
		data.SvgfPass.Reset();

		if (!data.StadPass.IsInitialized())
			data.StadPass.Init();
	}
}

void RayTracer::UpdateDescriptors(const RenderSettings& settings, RayTracerData& data) noexcept
{
	if (settings.RTIndirectDiffuse)
	{
		const Texture& indirectDiffuseLiTex = data.IndirectDiffusePass.GetOutput(IndirectDiffuse::SHADER_OUT_RES::INDIRECT_LI);
		CreateTexture2DSRV(indirectDiffuseLiTex, data.DescTableAll.CPUHandle(RayTracerData::DESC_TABLE::INDIRECT_LI));
	}

	const int outIdx = App::GetRenderer().CurrOutIdx();
	const auto denoiser = settings.IndirectDiffuseDenoiser;

	if (denoiser == DENOISER::SVGF)
	{
		// temporal cache changes every frame due to ping-ponging
		const SVGF::SHADER_OUT_RES temporalCacheIdx = outIdx == 0 ?
			SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_B :
			SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_A;

		const Texture& temporalCache = data.SvgfPass.GetOutput(temporalCacheIdx);
		CreateTexture2DSRV(temporalCache, data.DescTableAll.CPUHandle(RayTracerData::DESC_TABLE::TEMPORAL_CACHE));

		const Texture& spatialVar = data.SvgfPass.GetOutput(SVGF::SHADER_OUT_RES::SPATIAL_VAR);
		CreateTexture2DSRV(spatialVar, data.DescTableAll.CPUHandle(RayTracerData::DESC_TABLE::SPATIAL_VAR));
	}
	else if (denoiser == DENOISER::STAD)
	{
		// temporal cache changes every frame due to ping-ponging
		const Texture& temporalCache = data.StadPass.GetOutput(STAD::SHADER_OUT_RES::TEMPORAL_CACHE_POST_OUT);
		CreateTexture2DSRV(temporalCache, data.DescTableAll.CPUHandle(RayTracerData::DESC_TABLE::TEMPORAL_CACHE));
	}
}

void RayTracer::Update(const RenderSettings& settings, RayTracerData& data) noexcept
{
	UpdatePasses(settings, data);
	UpdateDescriptors(settings, data);

	data.RtAS.BuildFrameMeshInstanceData();

	const auto denoiser = settings.IndirectDiffuseDenoiser;

	if (denoiser == DENOISER::SVGF)
	{
		data.SvgfPass.SetDescriptor(SVGF::SHADER_IN_RES::INDIRECT_LI,
			data.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::INDIRECT_LI));
	}
	else if (denoiser == DENOISER::STAD)
	{
		data.StadPass.SetDescriptor(STAD::SHADER_IN_RES::INDIRECT_LI,
			data.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::INDIRECT_LI));
	}

	// TODO doesn't need to be called each frame
	auto& r = App::GetRenderer().GetSharedShaderResources();
	r.InsertOrAssignDefaultHeapBuffer(SceneRenderer::RT_SCENE_BVH, data.RtAS.GetTLAS());
}

void RayTracer::Register(const RenderSettings& settings, RayTracerData& data, RenderGraph& renderGraph) noexcept
{
	// Acceleration-structure build/rebuild/update
	fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.RtAS, &TLAS::Render);
	data.RtASBuildHandle = renderGraph.RegisterRenderPass("RT_AS_Build", RENDER_NODE_TYPE::ASYNC_COMPUTE, dlg);

	auto& tlas = data.RtAS.GetTLAS();
	
	if (tlas.IsInitialized())
	{
		renderGraph.RegisterResource(tlas.GetResource(), tlas.GetPathID(), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			false);

		// IndirectDiffuse
		if (settings.RTIndirectDiffuse)
		{
			fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.IndirectDiffusePass,
				&IndirectDiffuse::Render);
			data.IndirectDiffuseHandle = renderGraph.RegisterRenderPass("IndirectDiffuse",
				RENDER_NODE_TYPE::COMPUTE, dlg);

			// Direct3D api functions don't accept const pointers
			Texture& outLo = const_cast<Texture&>(data.IndirectDiffusePass.GetOutput(IndirectDiffuse::SHADER_OUT_RES::INDIRECT_LI));
			renderGraph.RegisterResource(outLo.GetResource(), outLo.GetPathID());

			// SVGF
			if (settings.IndirectDiffuseDenoiser == DENOISER::SVGF)
			{
				fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SvgfPass, &SVGF::Render);
				data.SvgfHandle = renderGraph.RegisterRenderPass("SVGF", RENDER_NODE_TYPE::COMPUTE, dlg);

				// Direct3D api doesn't accept const pointers
				Texture& temporalCacheIn = const_cast<Texture&>(data.SvgfPass.GetOutput(SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_A));
				Texture& temporalCacheOut = const_cast<Texture&>(data.SvgfPass.GetOutput(SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_B));
				renderGraph.RegisterResource(temporalCacheIn.GetResource(), temporalCacheIn.GetPathID());
				renderGraph.RegisterResource(temporalCacheOut.GetResource(), temporalCacheOut.GetPathID());

				Texture& spatialVar = const_cast<Texture&>(data.SvgfPass.GetOutput(SVGF::SHADER_OUT_RES::SPATIAL_VAR));
				renderGraph.RegisterResource(spatialVar.GetResource(), spatialVar.GetPathID());
			}
			else if (settings.IndirectDiffuseDenoiser == DENOISER::STAD)
			{
				fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.StadPass, &STAD::Render);
				data.StadHandle = renderGraph.RegisterRenderPass("STAD", RENDER_NODE_TYPE::COMPUTE, dlg);

				// Direct3D api doesn't accept const pointers
				Texture& temporalCacheIn = const_cast<Texture&>(data.StadPass.GetOutput(STAD::SHADER_OUT_RES::TEMPORAL_CACHE_PRE_IN));
				Texture& temporalCacheOut = const_cast<Texture&>(data.StadPass.GetOutput(STAD::SHADER_OUT_RES::TEMPORAL_CACHE_PRE_OUT));
				renderGraph.RegisterResource(temporalCacheIn.GetResource(), temporalCacheIn.GetPathID());
				renderGraph.RegisterResource(temporalCacheOut.GetResource(), temporalCacheOut.GetPathID());
			}
		}
	}
}

void RayTracer::DeclareAdjacencies(const RenderSettings& settings, const GBufferRendererData& gbuffData, 
	RayTracerData& data, RenderGraph& renderGraph) noexcept
{
	const int outIdx = App::GetRenderer().CurrOutIdx();
	auto& tlas = data.RtAS.GetTLAS();

	if (tlas.IsInitialized())
	{
		renderGraph.AddOutput(data.RtASBuildHandle,
			tlas.GetPathID(),
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
		
		// indirect-diffuse
		if (settings.RTIndirectDiffuse)
		{
			// RT-AS
			renderGraph.AddInput(data.IndirectDiffuseHandle,
				data.RtAS.GetTLAS().GetPathID(),
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

			// Current GBuffers
			renderGraph.AddInput(data.IndirectDiffuseHandle,
				gbuffData.Normal[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.IndirectDiffuseHandle,
				gbuffData.MetalnessRoughness[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.IndirectDiffuseHandle,
				gbuffData.EmissiveColor.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.IndirectDiffuseHandle,
				gbuffData.DepthBuffer[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddOutput(data.IndirectDiffuseHandle,
				data.IndirectDiffusePass.GetOutput(IndirectDiffuse::SHADER_OUT_RES::INDIRECT_LI).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// SVGF
			// 1. Current GBuffers
			// 2. Previous GBuffers
			// 3. Linear-depth Grad
			// 4. indirect lighting
			if (settings.IndirectDiffuseDenoiser == DENOISER::SVGF)
			{
				renderGraph.AddInput(data.SvgfHandle,
					gbuffData.Normal[outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.SvgfHandle,
					gbuffData.DepthBuffer[outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.SvgfHandle,
					gbuffData.Normal[1 - outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.SvgfHandle,
					gbuffData.DepthBuffer[1 - outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.SvgfHandle,
					data.IndirectDiffusePass.GetOutput(IndirectDiffuse::SHADER_OUT_RES::INDIRECT_LI).GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddOutput(data.SvgfHandle,
					data.SvgfPass.GetOutput(SVGF::SHADER_OUT_RES::SPATIAL_VAR).GetPathID(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

				const SVGF::SHADER_OUT_RES outTemporalCacheIdx = outIdx == 0 ?
					SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_B :
					SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_A;

				const SVGF::SHADER_OUT_RES inTemporalCacheIdx = outIdx == 0 ?
					SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_A :
					SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_B;

				renderGraph.AddInput(data.SvgfHandle,
					data.SvgfPass.GetOutput(inTemporalCacheIdx).GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddOutput(data.SvgfHandle,
					data.SvgfPass.GetOutput(outTemporalCacheIdx).GetPathID(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			}
			else if (settings.IndirectDiffuseDenoiser == DENOISER::STAD)
			{
				renderGraph.AddInput(data.StadHandle,
					gbuffData.Normal[outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.StadHandle,
					gbuffData.DepthBuffer[outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.StadHandle,
					gbuffData.Normal[1 - outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.StadHandle,
					gbuffData.DepthBuffer[1 - outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.StadHandle,
					data.IndirectDiffusePass.GetOutput(IndirectDiffuse::SHADER_OUT_RES::INDIRECT_LI).GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.StadHandle,
					data.StadPass.GetOutput(STAD::SHADER_OUT_RES::TEMPORAL_CACHE_PRE_IN).GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddOutput(data.StadHandle,
					data.StadPass.GetOutput(STAD::SHADER_OUT_RES::TEMPORAL_CACHE_PRE_OUT).GetPathID(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			}
		}
	}
}


