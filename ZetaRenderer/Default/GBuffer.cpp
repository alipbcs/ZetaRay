#include "DefaultRendererImpl.h"
#include <Core/CommandList.h>
#include <App/App.h>

using namespace ZetaRay::Math;
using namespace ZetaRay::Model;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Util;
using namespace ZetaRay::DefaultRenderer;

void GBuffer::Init(const RenderSettings& settings, GBufferData& data)
{
	for (int i = 0; i < 2; i++)
	{
		data.SrvDescTable[i] = App::GetRenderer().GetGpuDescriptorHeap().Allocate(GBufferData::COUNT);

#if RT_GBUFFER == 1
		data.UavDescTable[i] = App::GetRenderer().GetGpuDescriptorHeap().Allocate(GBufferData::COUNT);
#else
		data.RTVDescTable[i] = App::GetRenderer().GetRtvDescriptorHeap().Allocate(GBufferData::COUNT);
		data.DSVDescTable[i] = App::GetRenderer().GetDsvDescriptorHeap().Allocate(1);
#endif
	}

	CreateGBuffers(data);

#if RT_GBUFFER == 1
	data.GBufferPass.Init();
#else
	constexpr int NUM_RTVs = GBufferData::GBUFFER::COUNT - 1;

	DXGI_FORMAT rtvFormats[NUM_RTVs] = {
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::BASE_COLOR],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::NORMAL],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::METALLIC_ROUGHNESS],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::MOTION_VECTOR],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::EMISSIVE_COLOR],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::CURVATURE] };

	data.GBufferPass.Init(rtvFormats);
#endif
}

void GBuffer::CreateGBuffers(GBufferData& data)
{
	auto* device = App::GetRenderer().GetDevice();
	const int width = App::GetRenderer().GetRenderWidth();
	const int height = App::GetRenderer().GetRenderHeight();

#if RT_GBUFFER == 1
	const auto texFlags = CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS;
	const auto texFlagsDepth = CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS;

	D3D12_CLEAR_VALUE* clearValuePtr = nullptr;
	D3D12_CLEAR_VALUE* clearValuePtrDepth = nullptr;

	const D3D12_RESOURCE_STATES depthInitState = D3D12_RESOURCE_STATE_COMMON;
#else
	const auto texFlags = CREATE_TEXTURE_FLAGS::ALLOW_RENDER_TARGET;
	const auto texFlagsDepth = CREATE_TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clearValue = {};
	memset(clearValue.Color, 0, sizeof(float) * 4);

	D3D12_CLEAR_VALUE clearValueDepth = {};
	clearValueDepth.Format = Constants::DEPTH_BUFFER_FORMAT;
	clearValueDepth.DepthStencil.Depth = 0.0f;
	clearValueDepth.DepthStencil.Stencil = 0;

	D3D12_CLEAR_VALUE* clearValuePtr = &clearValue;
	D3D12_CLEAR_VALUE* clearValuePtrDepth = &clearValueDepth;

	const D3D12_RESOURCE_STATES depthInitState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
#endif

	// base color	
	{
		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "GBuffer_BaseColor_%d", i);

#if RT_GBUFFER == 0
			clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::BASE_COLOR];
#endif

			data.BaseColor[i] = ZetaMove(GpuMemory::GetTexture2D(name,
				width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::BASE_COLOR],
				D3D12_RESOURCE_STATE_COMMON,
				texFlags,
				1,
				clearValuePtr));

#if RT_GBUFFER == 1
			// UAV
			Direct3DUtil::CreateTexture2DUAV(data.BaseColor[i], data.UavDescTable[i].CPUHandle(GBufferData::GBUFFER::BASE_COLOR));
#else
			// RTV
			Direct3DUtil::CreateRTV(data.BaseColor[i], data.RTVDescTable[i].CPUHandle(GBufferData::GBUFFER::BASE_COLOR));
#endif
			// SRV
			Direct3DUtil::CreateTexture2DSRV(data.BaseColor[i], data.SrvDescTable[i].CPUHandle(GBufferData::GBUFFER::BASE_COLOR));
		}
	}

	// normal
	{
		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "GBuffer_Normal_%d", i);

#if RT_GBUFFER == 0
			clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::NORMAL];
#endif
			data.Normal[i] = ZetaMove(GpuMemory::GetTexture2D(name, 
				width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::NORMAL],
				D3D12_RESOURCE_STATE_COMMON,
				texFlags,
				1,
				clearValuePtr));

#if RT_GBUFFER == 1
			// UAV
			Direct3DUtil::CreateTexture2DUAV(data.Normal[i], data.UavDescTable[i].CPUHandle(GBufferData::GBUFFER::NORMAL));
#else
			// RTV
			Direct3DUtil::CreateRTV(data.Normal[i], data.RTVDescTable[i].CPUHandle(GBufferData::GBUFFER::NORMAL));
#endif
			// SRV
			Direct3DUtil::CreateTexture2DSRV(data.Normal[i], data.SrvDescTable[i].CPUHandle(GBufferData::GBUFFER::NORMAL));
		}
	}

	// metallic-roughness
	{
		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "GBuffer_MR_%d", i);

#if RT_GBUFFER == 0
			clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::METALLIC_ROUGHNESS];
#endif
			data.MetallicRoughness[i] = ZetaMove(GpuMemory::GetTexture2D(name, 
				width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::METALLIC_ROUGHNESS],
				D3D12_RESOURCE_STATE_COMMON,
				texFlags,
				1,
				clearValuePtr));

#if RT_GBUFFER == 1
			// UAV
			Direct3DUtil::CreateTexture2DUAV(data.MetallicRoughness[i], data.UavDescTable[i].CPUHandle(
				GBufferData::GBUFFER::METALLIC_ROUGHNESS));
#else
			// RTV
			Direct3DUtil::CreateRTV(data.MetallicRoughness[i], data.RTVDescTable[i].CPUHandle(
				GBufferData::GBUFFER::METALLIC_ROUGHNESS));
#endif
			// SRV
			Direct3DUtil::CreateTexture2DSRV(data.MetallicRoughness[i], data.SrvDescTable[i].CPUHandle(
				GBufferData::GBUFFER::METALLIC_ROUGHNESS));
		}
	}

	// motion vector
	{
#if RT_GBUFFER == 0
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::MOTION_VECTOR];
#endif
		data.MotionVec = ZetaMove(GpuMemory::GetTexture2D("GBuffer_MV", 
			width, height,
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::MOTION_VECTOR],
			D3D12_RESOURCE_STATE_COMMON,
			texFlags,
			1,
			clearValuePtr));

#if RT_GBUFFER == 1
		//UAV
		Direct3DUtil::CreateTexture2DUAV(data.MotionVec, data.UavDescTable[0].CPUHandle(GBufferData::GBUFFER::MOTION_VECTOR));
		Direct3DUtil::CreateTexture2DUAV(data.MotionVec, data.UavDescTable[1].CPUHandle(GBufferData::GBUFFER::MOTION_VECTOR));
#else
		// RTV
		Direct3DUtil::CreateRTV(data.MotionVec, data.RTVDescTable[0].CPUHandle(GBufferData::GBUFFER::MOTION_VECTOR));
		Direct3DUtil::CreateRTV(data.MotionVec, data.RTVDescTable[1].CPUHandle(GBufferData::GBUFFER::MOTION_VECTOR));
#endif
		// SRV
		Direct3DUtil::CreateTexture2DSRV(data.MotionVec, data.SrvDescTable[0].CPUHandle(GBufferData::GBUFFER::MOTION_VECTOR));
		Direct3DUtil::CreateTexture2DSRV(data.MotionVec, data.SrvDescTable[1].CPUHandle(GBufferData::GBUFFER::MOTION_VECTOR));
	}

	// emissive color
	{
#if RT_GBUFFER == 0
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::EMISSIVE_COLOR];
#endif
		data.EmissiveColor = ZetaMove(GpuMemory::GetTexture2D("GBuffer_Emissive", 
			width, height,
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::EMISSIVE_COLOR],
			D3D12_RESOURCE_STATE_COMMON,
			texFlags,
			1,
			clearValuePtr));

#if RT_GBUFFER == 1
		//UAV
		Direct3DUtil::CreateTexture2DUAV(data.EmissiveColor, data.UavDescTable[0].CPUHandle(GBufferData::GBUFFER::EMISSIVE_COLOR));
		Direct3DUtil::CreateTexture2DUAV(data.EmissiveColor, data.UavDescTable[1].CPUHandle(GBufferData::GBUFFER::EMISSIVE_COLOR));
#else
		// RTV
		Direct3DUtil::CreateRTV(data.EmissiveColor, data.RTVDescTable[0].CPUHandle(GBufferData::GBUFFER::EMISSIVE_COLOR));
		Direct3DUtil::CreateRTV(data.EmissiveColor, data.RTVDescTable[1].CPUHandle(GBufferData::GBUFFER::EMISSIVE_COLOR));
#endif
		// SRV
		Direct3DUtil::CreateTexture2DSRV(data.EmissiveColor, data.SrvDescTable[0].CPUHandle(GBufferData::GBUFFER::EMISSIVE_COLOR));
		Direct3DUtil::CreateTexture2DSRV(data.EmissiveColor, data.SrvDescTable[1].CPUHandle(GBufferData::GBUFFER::EMISSIVE_COLOR));
	}

	// depth
	{
		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "Depth_%d", i);

			data.DepthBuffer[i] = ZetaMove(GpuMemory::GetTexture2D(name, 
				width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::DEPTH],
				depthInitState,
				texFlagsDepth,
				1,
				clearValuePtrDepth));
			
#if RT_GBUFFER == 1
			// UAV
			Direct3DUtil::CreateTexture2DUAV(data.DepthBuffer[i], data.UavDescTable[i].CPUHandle(GBufferData::GBUFFER::DEPTH));
#else
			// DSV
			D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
			desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			desc.Flags = D3D12_DSV_FLAG_NONE;
			desc.Format = Constants::DEPTH_BUFFER_FORMAT;
			desc.Texture2D.MipSlice = 0;

			device->CreateDepthStencilView(data.DepthBuffer[i].Resource(), &desc,
				data.DSVDescTable[i].CPUHandle(0));
#endif
			// SRV
			Direct3DUtil::CreateTexture2DSRV(data.DepthBuffer[i], data.SrvDescTable[i].CPUHandle(GBufferData::GBUFFER::DEPTH), 
				DXGI_FORMAT_R32_FLOAT);
		}
	}

	// curvature
	{
#if RT_GBUFFER == 0
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::CURVATURE];
#endif
		data.Curvature = ZetaMove(GpuMemory::GetTexture2D("GBuffer_Curvature", 
			width, height,
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::CURVATURE],
			D3D12_RESOURCE_STATE_COMMON,
			texFlags,
			1,
			clearValuePtr));

#if RT_GBUFFER == 1
		//UAV
		Direct3DUtil::CreateTexture2DUAV(data.Curvature, data.UavDescTable[0].CPUHandle(GBufferData::GBUFFER::CURVATURE));
		Direct3DUtil::CreateTexture2DUAV(data.Curvature, data.UavDescTable[1].CPUHandle(GBufferData::GBUFFER::CURVATURE));
#else
		// RTV
		Direct3DUtil::CreateRTV(data.Curvature, data.RTVDescTable[0].CPUHandle(GBufferData::GBUFFER::CURVATURE));
		Direct3DUtil::CreateRTV(data.Curvature, data.RTVDescTable[1].CPUHandle(GBufferData::GBUFFER::CURVATURE));
#endif
		// SRV
		Direct3DUtil::CreateTexture2DSRV(data.Curvature, data.SrvDescTable[0].CPUHandle(GBufferData::GBUFFER::CURVATURE));
		Direct3DUtil::CreateTexture2DSRV(data.Curvature, data.SrvDescTable[1].CPUHandle(GBufferData::GBUFFER::CURVATURE));
	}
}

void GBuffer::OnWindowSizeChanged(const RenderSettings& settings, GBufferData& data)
{
	GBuffer::CreateGBuffers(data);

#if RT_GBUFFER == 0
	data.GBufferPass.OnWindowResized();
#endif
}

void GBuffer::Shutdown(GBufferData& data)
{
	data.GBufferPass.Reset();

	for (int i = 0; i < 2; i++)
	{
		data.BaseColor[i].Reset();
		data.Normal[i].Reset();
		data.DepthBuffer[i].Reset();
		data.MetallicRoughness[i].Reset();
		data.SrvDescTable[i].Reset();
#if RT_GBUFFER == 0
		data.RTVDescTable[i].Reset();
		data.DSVDescTable[i].Reset();
#endif
	}
	
	data.EmissiveColor.Reset();
	data.MotionVec.Reset();
	data.Curvature.Reset();
}

void GBuffer::Update(GBufferData& gbufferData)
{
	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

#if RT_GBUFFER == 1
	gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::BASE_COLOR_UAV,
		gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::BASE_COLOR));
	gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::NORMAL_UAV,
		gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::NORMAL));
	gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::METALLIC_ROUGHNESS_UAV,
		gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::METALLIC_ROUGHNESS));
	gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::MOTION_VECTOR_UAV,
		gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::MOTION_VECTOR));
	gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::EMISSIVE_COLOR_UAV,
		gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::EMISSIVE_COLOR));
	gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::DEPTH_UAV,
		gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::DEPTH));
	//gbufferData.GBufferPass.SetGpuDescriptor(GBufferRT::SHADER_IN_GPU_DESC::CURVATURE_UAV,
	//	gbufferData.UavDescTable[outIdx].GPUDesciptorHeapIndex(GBufferData::GBUFFER::CURVATURE));
#else
	SceneCore& scene = App::GetScene();
	Span<Math::BVH::BVHInput> frameInstances = scene.GetFrameInstances();

	SmallVector<MeshInstance, App::FrameAllocator> gbuffInstances;
	gbuffInstances.resize(frameInstances.size());

	{
		size_t currInstance = 0;

		for (const auto& instance : frameInstances)
		{
			const float4x3& M_prev = *scene.GetPrevToWorld(instance.ID).value();
			gbuffInstances[currInstance].PrevWorld = float3x4(M_prev);
			gbuffInstances[currInstance].CurrWorld = float3x4(scene.GetToWorld(instance.ID));
			gbuffInstances[currInstance].BoundingBox.Center = instance.AABB.Center;
			gbuffInstances[currInstance].BoundingBox.Extents = instance.AABB.Extents;

			currInstance++;
		}
	}

	{
		size_t currInstance = 0;

		for (const auto& instance : frameInstances)
		{
			const uint64_t meshID = scene.GetInstanceMeshID(instance.ID);
			if (meshID == SceneCore::NULL_MESH)
				continue;

			TriangleMesh* mesh = scene.GetMesh(meshID).value();
			Material* mat = scene.GetMaterial(mesh->m_materialID).value();

			gbuffInstances[currInstance].IndexCount = mesh->m_numIndices;
			gbuffInstances[currInstance].BaseVtxOffset = (uint32_t)mesh->m_vtxBuffStartOffset;
			gbuffInstances[currInstance].BaseIdxOffset = (uint32_t)mesh->m_idxBuffStartOffset;
			gbuffInstances[currInstance].IdxInMatBuff = (uint16_t)mat->GpuBufferIndex();
			gbuffInstances[currInstance].IsDoubleSided = mat->IsDoubleSided();

			currInstance++;
		}
	}

	{
		size_t currInstance = 0;

		for (const auto& instance : frameInstances)
		{
			const uint32_t visIdx = scene.GetInstanceVisibilityIndex(instance.ID);
			gbuffInstances[currInstance].VisibilityIdx = visIdx;

			currInstance++;
		}
	}

	// these change every frame
	gbufferData.GBufferPass.SetDescriptor(GBufferPass::SHADER_IN_DESC::GBUFFERS_RTV,
		gbufferData.RTVDescTable[outIdx].CPUHandle(0));
	gbufferData.GBufferPass.SetDescriptor(GBufferPass::SHADER_IN_DESC::CURR_DEPTH_BUFFER_DSV,
		gbufferData.DSVDescTable[outIdx].CPUHandle(0));

	gbufferData.GBufferPass.Update(gbuffInstances, gbufferData.DepthBuffer[outIdx].Resource());

	// clear the gbuffers
	gbufferData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::BASE_COLOR,
		gbufferData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER::BASE_COLOR));
	gbufferData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::NORMAL,
		gbufferData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER::NORMAL));
	gbufferData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::METALLIC_ROUGHNESS,
		gbufferData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER::METALLIC_ROUGHNESS));
	gbufferData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::MOTION_VECTOR,
		gbufferData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER::MOTION_VECTOR));
	gbufferData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::EMISSIVE_COLOR,
		gbufferData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER::EMISSIVE_COLOR));
	gbufferData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::DEPTH_BUFFER,
		gbufferData.DSVDescTable[outIdx].CPUHandle(0));
	gbufferData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::CURVATURE,
		gbufferData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER::CURVATURE));
#endif
}

void GBuffer::Register(GBufferData& data, const RayTracerData& rayTracerData, RenderGraph& renderGraph)
{
#if RT_GBUFFER == 1
	const bool tlasReady = rayTracerData.RtAS.IsReady();
	if (!tlasReady)
		return;

	// GBuffer
	fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.GBufferPass, &GBufferRT::Render);
	data.GBufferPassHandle = renderGraph.RegisterRenderPass("GBuffer", RENDER_NODE_TYPE::COMPUTE, dlg);

	const D3D12_RESOURCE_STATES initDepthState = D3D12_RESOURCE_STATE_COMMON;
#else
	// Clear
	fastdelegate::FastDelegate1<CommandList&> clearDlg = fastdelegate::MakeDelegate(&data.ClearPass, &ClearPass::Clear);
	data.ClearHandle = renderGraph.RegisterRenderPass("Clear", RENDER_NODE_TYPE::RENDER, clearDlg);

	// GBuffer
	fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.GBufferPass, &GBufferPass::Render);
	data.GBufferPassHandle = renderGraph.RegisterRenderPass("GBuffer", RENDER_NODE_TYPE::RENDER, dlg);

	const D3D12_RESOURCE_STATES initDepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
#endif

	// register current and previous frame's gbuffers
	for (int i = 0; i < 2; i++)
	{
		renderGraph.RegisterResource(data.Normal[i].Resource(), data.Normal[i].ID());
		renderGraph.RegisterResource(data.DepthBuffer[i].Resource(), data.DepthBuffer[i].ID(), initDepthState);
		renderGraph.RegisterResource(data.MetallicRoughness[i].Resource(), data.MetallicRoughness[i].ID());
		renderGraph.RegisterResource(data.BaseColor[i].Resource(), data.BaseColor[i].ID());
	}

	renderGraph.RegisterResource(data.MotionVec.Resource(), data.MotionVec.ID());
	renderGraph.RegisterResource(data.EmissiveColor.Resource(), data.EmissiveColor.ID());
	renderGraph.RegisterResource(data.Curvature.Resource(), data.Curvature.ID());

#if RT_GBUFFER == 0
	// when more than one RenderPass outputs one resource, it's unclear which one should run first.
	// add a made-up resource so that GBufferPass runs after Clear
	renderGraph.RegisterResource(nullptr, RenderGraph::DUMMY_RES::RES_0);
#endif
}

void GBuffer::DeclareAdjacencies(GBufferData& data, const RayTracerData& rayTracerData, 
	RenderGraph& renderGraph)
{
	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

#if RT_GBUFFER == 1
	const bool tlasReady = rayTracerData.RtAS.IsReady();
	if (!tlasReady)
		return;

	const D3D12_RESOURCE_STATES gbufferOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	const D3D12_RESOURCE_STATES depthBuffOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	renderGraph.AddInput(data.GBufferPassHandle,
		rayTracerData.RtAS.GetTLAS().ID(),
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

#else
	const D3D12_RESOURCE_STATES gbufferOutState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	const D3D12_RESOURCE_STATES depthBuffOutState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

	renderGraph.AddOutput(data.ClearHandle, data.BaseColor[outIdx].ID(), gbufferOutState);
	renderGraph.AddOutput(data.ClearHandle, data.Normal[outIdx].ID(), gbufferOutState);
	renderGraph.AddOutput(data.ClearHandle, data.MotionVec.ID(), gbufferOutState);
	renderGraph.AddOutput(data.ClearHandle, data.MetallicRoughness[outIdx].ID(), gbufferOutState);
	renderGraph.AddOutput(data.ClearHandle, data.EmissiveColor.ID(), gbufferOutState);
	renderGraph.AddOutput(data.ClearHandle, data.DepthBuffer[outIdx].ID(), depthBuffOutState);
	renderGraph.AddOutput(data.ClearHandle, data.Curvature.ID(), gbufferOutState);
	// HACK use D3D12_RESOURCE_STATE_UNORDERED_ACCESS, which can be considered as both readable and 
	// writable to avoid a Transition
	renderGraph.AddOutput(data.ClearHandle, RenderGraph::DUMMY_RES::RES_0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// GBufferPass depends on Clear
	renderGraph.AddInput(data.GBufferPassHandle, RenderGraph::DUMMY_RES::RES_0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
#endif

	renderGraph.AddOutput(data.GBufferPassHandle, data.BaseColor[outIdx].ID(), gbufferOutState);
	renderGraph.AddOutput(data.GBufferPassHandle, data.Normal[outIdx].ID(), gbufferOutState);
	renderGraph.AddOutput(data.GBufferPassHandle, data.MetallicRoughness[outIdx].ID(), gbufferOutState);
	renderGraph.AddOutput(data.GBufferPassHandle, data.MotionVec.ID(), gbufferOutState);
	renderGraph.AddOutput(data.GBufferPassHandle, data.EmissiveColor.ID(), gbufferOutState);
	renderGraph.AddOutput(data.GBufferPassHandle, data.DepthBuffer[outIdx].ID(), depthBuffOutState);
	renderGraph.AddOutput(data.GBufferPassHandle, data.Curvature.ID(), gbufferOutState);
}
