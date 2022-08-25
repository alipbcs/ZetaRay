#include "SceneRendererImpl.h"
#include "../../Core/CommandList.h"
#include "../../Win32/App.h"
#include "../../Core/SharedShaderResources.h"

using namespace ZetaRay;
using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Scene;
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
	RayTracer::CreateDescriptors(settings, data);

	// init sampler (async)
	data.RtSampler.InitLowDiscrepancyBlueNoise();
}

void RayTracer::CreateDescriptors(const RenderSettings& settings, RayTracerData& data) noexcept
{
	if (data.IndirectDiffusePass.IsInitialized() && settings.RTIndirectDiffuse)
	{
		Assert(!data.DescTableAll.IsEmpty(), "descriptor table hasn't been allocated yet.");

		// Direct3D api doesn't accept const pointers
		const Texture& indirectDiffuseLiTex = data.IndirectDiffusePass.GetOutput(IndirectDiffuse::SHADER_OUT_RES::INDIRECT_LI);
		CreateTexture2DSRV(indirectDiffuseLiTex, data.DescTableAll.CPUHandle(RayTracerData::DESC_TABLE::INDIRECT_LI));
	}

	if (data.IndirectDiffusePass.IsInitialized() && settings.DenoiseIndirectDiffuseLi)
	{
		Assert(!data.DescTableAll.IsEmpty(), "descriptor table hasn't been allocated yet.");

		// no need to create temporal cache srv since it's recreated every frame

		// Linear-depth grad SRV
		const Texture& linearDepthGradTex = data.LinearDepthGradPass.GetOutput(LinearDepthGradient::GRADIENT);
		CreateTexture2DSRV(linearDepthGradTex, data.DescTableAll.CPUHandle(RayTracerData::DESC_TABLE::LINEAR_DEPTH_GRAD));
	}
}

void RayTracer::OnWindowSizeChanged(const RenderSettings& settings, RayTracerData& data) noexcept
{
	if(data.LinearDepthGradPass.IsInitialized())
		data.LinearDepthGradPass.OnWindowResized();
	if(data.IndirectDiffusePass.IsInitialized())
		data.IndirectDiffusePass.OnWindowResized();
	if(data.SVGF_Pass.IsInitialized())
		data.SVGF_Pass.OnWindowResized();

	RayTracer::CreateDescriptors(settings, data);
}

void RayTracer::Shutdown(RayTracerData& data) noexcept
{
	data.DescTableAll.Reset();
	data.RtAS.Clear();
	data.RtSampler.Clear();
	data.LinearDepthGradPass.Reset();
	data.IndirectDiffusePass.Reset();
	//data.ReSTIR_Pass.Reset();
	data.SVGF_Pass.Reset();
}

void RayTracer::Update(const RenderSettings& settings, RayTracerData& data) noexcept
{
	data.RtAS.BuildFrameMeshInstanceData();

	const int outIdx = App::GetRenderer().CurrOutIdx();

	if (settings.RTIndirectDiffuse && !data.IndirectDiffusePass.IsInitialized())
	{
		data.IndirectDiffusePass.Init();

		const Texture& indirectDiffuseLiTex = data.IndirectDiffusePass.GetOutput(IndirectDiffuse::SHADER_OUT_RES::INDIRECT_LI);
		CreateTexture2DSRV(indirectDiffuseLiTex, data.DescTableAll.CPUHandle(RayTracerData::DESC_TABLE::INDIRECT_LI));
	}

	if (settings.DenoiseIndirectDiffuseLi)
	{
		if (!data.SVGF_Pass.IsInitialized())
		{
			data.SVGF_Pass.Init();
			data.LinearDepthGradPass.Init();

			// Linear-depth grad SRV
			const Texture& linearDepthGradTex = data.LinearDepthGradPass.GetOutput(LinearDepthGradient::GRADIENT);
			CreateTexture2DSRV(linearDepthGradTex, data.DescTableAll.CPUHandle(RayTracerData::DESC_TABLE::LINEAR_DEPTH_GRAD));
		}

		// temporal cache changes every frame due to ping-ponging
		const SVGF::SHADER_OUT_RES temporalCacheIdx = outIdx == 0 ?
			SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_B :
			SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_A;

		const Texture& temporalCache = data.SVGF_Pass.GetOutput(temporalCacheIdx);
		CreateTexture2DSRV(temporalCache, data.DescTableAll.CPUHandle(RayTracerData::DESC_TABLE::TEMPORAL_CACHE));

		data.SVGF_Pass.SetDescriptor(SVGF::SHADER_IN_RES::LINEAR_DEPTH_GRAD, 
			data.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::LINEAR_DEPTH_GRAD));
		data.SVGF_Pass.SetDescriptor(SVGF::SHADER_IN_RES::INDIRECT_LI, 
			data.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::INDIRECT_LI));

		const Texture& spatialVar = data.SVGF_Pass.GetOutput(SVGF::SHADER_OUT_RES::SPATIAL_VAR);
		CreateTexture2DSRV(spatialVar, data.DescTableAll.CPUHandle(RayTracerData::DESC_TABLE::SPATIAL_VAR));
	}

	// TODO doesn't need to be called each frame
	auto& r = App::GetRenderer().GetSharedShaderResources();
	r.InsertOrAssignDefaultHeapBuffer(SceneRenderer::RT_SCENE_BVH, data.RtAS.GetTLAS());
}

void RayTracer::Register(const RenderSettings& settings, RayTracerData& data, RenderGraph& renderGraph) noexcept
{
	// Acceleration-structure build/rebuild/update
	fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.RtAS,
		&TLAS::Render);
	data.RtASBuildPassHandle = renderGraph.RegisterRenderPass("RT_AS_Build", RENDER_NODE_TYPE::ASYNC_COMPUTE, dlg);

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
			data.IndirectDiffusePassHandle = renderGraph.RegisterRenderPass("IndirectDiffuse",
				RENDER_NODE_TYPE::COMPUTE, dlg);

			// Direct3D api functions don't accept const pointers
			Texture& outLo = const_cast<Texture&>(data.IndirectDiffusePass.GetOutput(IndirectDiffuse::SHADER_OUT_RES::INDIRECT_LI));
			renderGraph.RegisterResource(outLo.GetResource(), outLo.GetPathID());

			// SVGF
			if (settings.DenoiseIndirectDiffuseLi)
			{
				// LinearDepthGradient
				{
					fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.LinearDepthGradPass,
						&LinearDepthGradient::Render);
					data.LinearDepthGradPassHandle = renderGraph.RegisterRenderPass("LinearDepthGradient",
						RENDER_NODE_TYPE::COMPUTE, dlg);

					Texture& out = data.LinearDepthGradPass.GetOutput(LinearDepthGradient::GRADIENT);
					renderGraph.RegisterResource(out.GetResource(), out.GetPathID());
				}

				// svgf
				{
					fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SVGF_Pass,
						&SVGF::Render);
					data.SVGF_PassHandle = renderGraph.RegisterRenderPass("SVGF",
						RENDER_NODE_TYPE::COMPUTE, dlg);

					// Direct3D api doesn't accept const pointers
					Texture& temporalCacheIn = const_cast<Texture&>(data.SVGF_Pass.GetOutput(SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_A));
					Texture& temporalCacheOut = const_cast<Texture&>(data.SVGF_Pass.GetOutput(SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_B));
					renderGraph.RegisterResource(temporalCacheIn.GetResource(), temporalCacheIn.GetPathID());
					renderGraph.RegisterResource(temporalCacheOut.GetResource(), temporalCacheOut.GetPathID());

					Texture& spatialVar = const_cast<Texture&>(data.SVGF_Pass.GetOutput(SVGF::SHADER_OUT_RES::SPATIAL_VAR));
					renderGraph.RegisterResource(spatialVar.GetResource(), spatialVar.GetPathID());
				}
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
		renderGraph.AddOutput(data.RtASBuildPassHandle,
			tlas.GetPathID(),
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
		
		// indirect-diffuse
		if (settings.RTIndirectDiffuse)
		{
			// RT-AS
			renderGraph.AddInput(data.IndirectDiffusePassHandle,
				data.RtAS.GetTLAS().GetPathID(),
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

			// Current GBuffers
			renderGraph.AddInput(data.IndirectDiffusePassHandle,
				gbuffData.Normal[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.IndirectDiffusePassHandle,
				gbuffData.MetalnessRoughness[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.IndirectDiffusePassHandle,
				gbuffData.EmissiveColor.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(data.IndirectDiffusePassHandle,
				gbuffData.DepthBuffer[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddOutput(data.IndirectDiffusePassHandle,
				data.IndirectDiffusePass.GetOutput(IndirectDiffuse::SHADER_OUT_RES::INDIRECT_LI).GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// SVGF
			// 1. Current GBuffers
			// 2. Previous GBuffers
			// 3. Linear-depth Grad
			// 4. indirect lighting
			if (settings.DenoiseIndirectDiffuseLi)
			{
				// linear-depth gradient
				renderGraph.AddInput(data.LinearDepthGradPassHandle,
					gbuffData.DepthBuffer[outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddOutput(data.LinearDepthGradPassHandle,
					data.LinearDepthGradPass.GetOutput(LinearDepthGradient::GRADIENT).GetPathID(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

				// svgf
				renderGraph.AddInput(data.SVGF_PassHandle,
					gbuffData.Normal[outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.SVGF_PassHandle,
					gbuffData.DepthBuffer[outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.SVGF_PassHandle,
					gbuffData.Normal[1 - outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.SVGF_PassHandle,
					gbuffData.DepthBuffer[1 - outIdx].GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.SVGF_PassHandle,
					data.LinearDepthGradPass.GetOutput(LinearDepthGradient::GRADIENT).GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddInput(data.SVGF_PassHandle,
					data.IndirectDiffusePass.GetOutput(IndirectDiffuse::SHADER_OUT_RES::INDIRECT_LI).GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddOutput(data.SVGF_PassHandle,
					data.SVGF_Pass.GetOutput(SVGF::SHADER_OUT_RES::SPATIAL_VAR).GetPathID(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

				const SVGF::SHADER_OUT_RES outTemporalCacheIdx = outIdx == 0 ?
					SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_B :
					SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_A;

				const SVGF::SHADER_OUT_RES inTemporalCacheIdx = outIdx == 0 ?
					SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_A :
					SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_B;

				renderGraph.AddInput(data.SVGF_PassHandle,
					data.SVGF_Pass.GetOutput(inTemporalCacheIdx).GetPathID(),
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				renderGraph.AddOutput(data.SVGF_PassHandle,
					data.SVGF_Pass.GetOutput(outTemporalCacheIdx).GetPathID(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			}
		}
	}
}


