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
		data.RTVDescTable[i] = App::GetRenderer().GetRtvDescriptorHeap().Allocate(GBufferData::COUNT);
		data.SRVDescTable[i] = App::GetRenderer().GetGpuDescriptorHeap().Allocate(GBufferData::COUNT);
		data.DSVDescTable[i] = App::GetRenderer().GetDsvDescriptorHeap().Allocate(1);
	}

	CreateGBuffers(data);

	constexpr int NUM_RTVs = GBufferData::GBUFFER::COUNT - 1;

	DXGI_FORMAT rtvFormats[NUM_RTVs] = {
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_BASE_COLOR],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_NORMAL],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_METALLIC_ROUGHNESS],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_MOTION_VECTOR],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_EMISSIVE_COLOR],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_CURVATURE] };

	data.GBuffPass.Init(rtvFormats);
}

void GBuffer::CreateGBuffers(GBufferData& data)
{
	auto* device = App::GetRenderer().GetDevice();
	const int width = App::GetRenderer().GetRenderWidth();
	const int height = App::GetRenderer().GetRenderHeight();

	// base color	
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_BASE_COLOR];

		data.BaseColor = ZetaMove(GpuMemory::GetTexture2D("GBuffer_BaseColor",
			width, height,
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_BASE_COLOR],
			D3D12_RESOURCE_STATE_COMMON,
			CREATE_TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
			1,
			&clearValue));

		// RTV
		Direct3DUtil::CreateRTV(data.BaseColor, data.RTVDescTable[0].CPUHandle(GBufferData::GBUFFER_BASE_COLOR));
		Direct3DUtil::CreateRTV(data.BaseColor, data.RTVDescTable[1].CPUHandle(GBufferData::GBUFFER_BASE_COLOR));

		// SRV
		Direct3DUtil::CreateTexture2DSRV(data.BaseColor, data.SRVDescTable[0].CPUHandle(GBufferData::GBUFFER_BASE_COLOR));
		Direct3DUtil::CreateTexture2DSRV(data.BaseColor, data.SRVDescTable[1].CPUHandle(GBufferData::GBUFFER_BASE_COLOR));
	}

	// normal
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_NORMAL];

		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "GBuffer_Normal_%d", i);

			data.Normal[i] = ZetaMove(GpuMemory::GetTexture2D(name, width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_NORMAL],
				D3D12_RESOURCE_STATE_COMMON,
				CREATE_TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
				1,
				&clearValue));

			// RTV
			Direct3DUtil::CreateRTV(data.Normal[i], data.RTVDescTable[i].CPUHandle(GBufferData::GBUFFER_NORMAL));

			// SRV
			Direct3DUtil::CreateTexture2DSRV(data.Normal[i], data.SRVDescTable[i].CPUHandle(GBufferData::GBUFFER_NORMAL));
		}
	}

	// metallic-roughness
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_METALLIC_ROUGHNESS];

		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "GBuffer_Metallic_Roughness_%d", i);

			data.MetallicRoughness[i] = ZetaMove(GpuMemory::GetTexture2D(name, width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_METALLIC_ROUGHNESS],
				D3D12_RESOURCE_STATE_COMMON,
				CREATE_TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
				1,
				&clearValue));

			// RTV
			Direct3DUtil::CreateRTV(data.MetallicRoughness[i], data.RTVDescTable[i].CPUHandle(GBufferData::GBUFFER_METALLIC_ROUGHNESS));

			// SRV
			Direct3DUtil::CreateTexture2DSRV(data.MetallicRoughness[i], data.SRVDescTable[i].CPUHandle(GBufferData::GBUFFER_METALLIC_ROUGHNESS));
		}
	}

	// motion vector
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_MOTION_VECTOR];

		data.MotionVec = ZetaMove(GpuMemory::GetTexture2D("GBuffer_MotionVec", width, height,
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_MOTION_VECTOR],
			D3D12_RESOURCE_STATE_COMMON,
			CREATE_TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
			1,
			&clearValue));

		// RTV
		Direct3DUtil::CreateRTV(data.MotionVec, data.RTVDescTable[0].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));
		Direct3DUtil::CreateRTV(data.MotionVec, data.RTVDescTable[1].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));

		// SRV
		Direct3DUtil::CreateTexture2DSRV(data.MotionVec, data.SRVDescTable[0].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));
		Direct3DUtil::CreateTexture2DSRV(data.MotionVec, data.SRVDescTable[1].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));
	}

	// emissive color	
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_EMISSIVE_COLOR];

		data.EmissiveColor = ZetaMove(GpuMemory::GetTexture2D("GBuffer_EmissiveColor", width, height,
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_EMISSIVE_COLOR],
			D3D12_RESOURCE_STATE_COMMON,
			CREATE_TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
			1,
			&clearValue));

		// RTV
		Direct3DUtil::CreateRTV(data.EmissiveColor, data.RTVDescTable[0].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));
		Direct3DUtil::CreateRTV(data.EmissiveColor, data.RTVDescTable[1].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));

		// SRV
		Direct3DUtil::CreateTexture2DSRV(data.EmissiveColor, data.SRVDescTable[0].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));
		Direct3DUtil::CreateTexture2DSRV(data.EmissiveColor, data.SRVDescTable[1].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));
	}

	// depth
	{
		D3D12_CLEAR_VALUE clearValueDepth = {};
		clearValueDepth.Format = Constants::DEPTH_BUFFER_FORMAT;
		clearValueDepth.DepthStencil.Depth = 0.0f;
		clearValueDepth.DepthStencil.Stencil = 0;

		D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
		desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		desc.Flags = D3D12_DSV_FLAG_NONE;
		desc.Format = Constants::DEPTH_BUFFER_FORMAT;
		desc.Texture2D.MipSlice = 0;

		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "DepthBuffer_%d", i);

			data.DepthBuffer[i] = ZetaMove(GpuMemory::GetTexture2D(name, width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_DEPTH],
				D3D12_RESOURCE_STATE_DEPTH_WRITE,
				CREATE_TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL,
				1,
				&clearValueDepth));
			
			// DSV
			device->CreateDepthStencilView(data.DepthBuffer[i].Resource(), &desc,
				data.DSVDescTable[i].CPUHandle(0));

			// SRV
			Direct3DUtil::CreateTexture2DSRV(data.DepthBuffer[i], data.SRVDescTable[i].CPUHandle(GBufferData::GBUFFER_DEPTH), 
				DXGI_FORMAT_R32_FLOAT);
		}
	}

	// curvature
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_CURVATURE];

		data.Curvature = ZetaMove(GpuMemory::GetTexture2D("GBuffer_Curvature", width, height,
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_CURVATURE],
			D3D12_RESOURCE_STATE_COMMON,
			CREATE_TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
			1,
			&clearValue));

		// RTV
		Direct3DUtil::CreateRTV(data.Curvature, data.RTVDescTable[0].CPUHandle(GBufferData::GBUFFER_CURVATURE));
		Direct3DUtil::CreateRTV(data.Curvature, data.RTVDescTable[1].CPUHandle(GBufferData::GBUFFER_CURVATURE));

		// SRV
		Direct3DUtil::CreateTexture2DSRV(data.Curvature, data.SRVDescTable[0].CPUHandle(GBufferData::GBUFFER_CURVATURE));
		Direct3DUtil::CreateTexture2DSRV(data.Curvature, data.SRVDescTable[1].CPUHandle(GBufferData::GBUFFER_CURVATURE));
	}
}

void GBuffer::OnWindowSizeChanged(const RenderSettings& settings, GBufferData& data)
{
	GBuffer::CreateGBuffers(data);
	data.GBuffPass.OnWindowResized();
}

void GBuffer::Shutdown(GBufferData& data)
{
	data.GBuffPass.Reset();

	for (int i = 0; i < 2; i++)
	{
		data.Normal[i].Reset();
		data.DepthBuffer[i].Reset();
		data.MetallicRoughness[i].Reset();
		data.RTVDescTable[i].Reset();
		data.SRVDescTable[i].Reset();
		data.DSVDescTable[i].Reset();
	}
	
	data.BaseColor.Reset();
	data.EmissiveColor.Reset();
	data.MotionVec.Reset();
	data.Curvature.Reset();
}

void GBuffer::Update(GBufferData& gbuffData)
{
	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();
	SceneCore& scene = App::GetScene();
	Span<Math::BVH::BVHInput> frameInstances = scene.GetFrameInstances();

	SmallVector<MeshInstance, App::FrameAllocator> gbuffInstances;
	gbuffInstances.resize(frameInstances.size());

	{
		size_t currInstance = 0;

		for (const auto& instance : frameInstances)
		{
			gbuffInstances[currInstance].PrevWorld = float3x4(scene.GetPrevToWorld(instance.ID));
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

			TriangleMesh* mesh = scene.GetMesh(meshID);
			Assert(mesh, "mesh with id %llu was not found", meshID);

			Material* mat = scene.GetMaterial(mesh->m_materialID);
			Assert(mat, "material with id %llu was not found", mesh->m_materialID);

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
	gbuffData.GBuffPass.SetDescriptor(GBufferPass::SHADER_IN_DESC::GBUFFERS_RTV,
		gbuffData.RTVDescTable[outIdx].CPUHandle(0));
	gbuffData.GBuffPass.SetDescriptor(GBufferPass::SHADER_IN_DESC::CURR_DEPTH_BUFFER_DSV,
		gbuffData.DSVDescTable[outIdx].CPUHandle(0));

	gbuffData.GBuffPass.Update(gbuffInstances, gbuffData.DepthBuffer[outIdx].Resource());

	// clear the gbuffers
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::BASE_COLOR,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER_BASE_COLOR));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::NORMAL,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER_NORMAL));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::METALLIC_ROUGHNESS,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER_METALLIC_ROUGHNESS));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::MOTION_VECTOR,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::EMISSIVE_COLOR,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::DEPTH_BUFFER,
		gbuffData.DSVDescTable[outIdx].CPUHandle(0));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::CURVATURE,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER_CURVATURE));
}

void GBuffer::Register(GBufferData& data, RenderGraph& renderGraph)
{
	// Clear
	fastdelegate::FastDelegate1<CommandList&> clearDlg = fastdelegate::MakeDelegate(&data.ClearPass, &ClearPass::Clear);
	data.ClearHandle = renderGraph.RegisterRenderPass("Clear", RENDER_NODE_TYPE::RENDER, clearDlg);

	// GBuffer
	fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.GBuffPass, &GBufferPass::Render);
	data.GBuffPassHandle = renderGraph.RegisterRenderPass("GBuffer", RENDER_NODE_TYPE::RENDER, dlg);

	// register current and previous frame's gbuffers
	for (int i = 0; i < 2; i++)
	{
		renderGraph.RegisterResource(data.Normal[i].Resource(), data.Normal[i].ID());
		renderGraph.RegisterResource(data.DepthBuffer[i].Resource(), data.DepthBuffer[i].ID(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
		renderGraph.RegisterResource(data.MetallicRoughness[i].Resource(), data.MetallicRoughness[i].ID());
	}

	renderGraph.RegisterResource(data.BaseColor.Resource(), data.BaseColor.ID());
	renderGraph.RegisterResource(data.MotionVec.Resource(), data.MotionVec.ID());
	renderGraph.RegisterResource(data.EmissiveColor.Resource(), data.EmissiveColor.ID());
	renderGraph.RegisterResource(data.Curvature.Resource(), data.Curvature.ID());

	// when more than one RenderPass outputs one resource, it's unclear which one should run first.
	// add a made-up resource so that GBufferPass runs after Clear
	renderGraph.RegisterResource(nullptr, RenderGraph::DUMMY_RES::RES_0);
}

void GBuffer::DeclareAdjacencies(GBufferData& data, const LightData& lightData, RenderGraph& renderGraph)
{
	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

	// [hack] use D3D12_RESOURCE_STATE_UNORDERED_ACCESS, which can be considered as both readable and 
	// writable to avoid a Transition

	renderGraph.AddOutput(data.ClearHandle, data.BaseColor.ID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.Normal[outIdx].ID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.MotionVec.ID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.MetallicRoughness[outIdx].ID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.EmissiveColor.ID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.DepthBuffer[outIdx].ID(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	renderGraph.AddOutput(data.ClearHandle, data.Curvature.ID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, RenderGraph::DUMMY_RES::RES_0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// make the GBufferPass dependent on Clear
	renderGraph.AddInput(data.GBuffPassHandle, RenderGraph::DUMMY_RES::RES_0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	renderGraph.AddOutput(data.GBuffPassHandle, data.BaseColor.ID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.Normal[outIdx].ID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.MetallicRoughness[outIdx].ID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.MotionVec.ID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.EmissiveColor.ID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.DepthBuffer[outIdx].ID(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	renderGraph.AddOutput(data.GBuffPassHandle, data.Curvature.ID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
}
