#include "DefaultRendererImpl.h"
#include <Core/Direct3DHelpers.h>
#include <Core/CommandList.h>
#include <App/App.h>

using namespace ZetaRay::Math;
using namespace ZetaRay::Model;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Core;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Util;
using namespace ZetaRay::DefaultRenderer;

void GBuffer::Init(const RenderSettings& settings, GBufferData& data) noexcept
{
	for (int i = 0; i < 2; i++)
	{
		data.RTVDescTable[i] = App::GetRenderer().GetRtvDescriptorHeap().Allocate(GBufferData::COUNT);
		data.SRVDescTable[i] = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(GBufferData::COUNT);
		data.DSVDescTable[i] = App::GetRenderer().GetDsvDescriptorHeap().Allocate(1);
	}

	CreateGBuffers(data);

	constexpr int NUM_RTVs = GBufferData::GBUFFER::COUNT - 1;

	DXGI_FORMAT rtvFormats[NUM_RTVs] = {
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_BASE_COLOR],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_NORMAL],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_METALNESS_ROUGHNESS],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_MOTION_VECTOR],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_EMISSIVE_COLOR] };

	data.GBuffPass.Init(rtvFormats);
}

void GBuffer::CreateGBuffers(GBufferData& data) noexcept
{
	auto* device = App::GetRenderer().GetDevice();
	auto& gpuMem = App::GetRenderer().GetGpuMemory();
	const int width = App::GetRenderer().GetRenderWidth();
	const int height = App::GetRenderer().GetRenderHeight();

	// base-color	
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_BASE_COLOR];

		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "GBuffer_BaseColor_%d", i);

			data.BaseColor[i] = ZetaMove(gpuMem.GetTexture2D(name, 
				width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_BASE_COLOR],
				D3D12_RESOURCE_STATE_COMMON,
				TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
				1,
				&clearValue));

			// RTV
			Direct3DHelper::CreateRTV(data.BaseColor[i], data.RTVDescTable[i].CPUHandle(GBufferData::GBUFFER_BASE_COLOR));

			// SRV
			Direct3DHelper::CreateTexture2DSRV(data.BaseColor[i], data.SRVDescTable[i].CPUHandle(GBufferData::GBUFFER_BASE_COLOR));
		}
	}

	// normal
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_NORMAL];

		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "GBuffer_Normal_%d", i);

			data.Normal[i] = ZetaMove(gpuMem.GetTexture2D(name, width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_NORMAL],
				D3D12_RESOURCE_STATE_COMMON,
				TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
				1,
				&clearValue));

			// RTV
			Direct3DHelper::CreateRTV(data.Normal[i], data.RTVDescTable[i].CPUHandle(GBufferData::GBUFFER_NORMAL));

			// SRV
			Direct3DHelper::CreateTexture2DSRV(data.Normal[i], data.SRVDescTable[i].CPUHandle(GBufferData::GBUFFER_NORMAL));
		}
	}

	// metalness-roughness
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_METALNESS_ROUGHNESS];

		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "GBuffer_Metalness-Roughness_%d", i);

			data.MetalnessRoughness[i] = ZetaMove(gpuMem.GetTexture2D(name, width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_METALNESS_ROUGHNESS],
				D3D12_RESOURCE_STATE_COMMON,
				TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
				1,
				&clearValue));

			// RTV
			Direct3DHelper::CreateRTV(data.MetalnessRoughness[i], data.RTVDescTable[i].CPUHandle(GBufferData::GBUFFER_METALNESS_ROUGHNESS));

			// SRV
			Direct3DHelper::CreateTexture2DSRV(data.MetalnessRoughness[i], data.SRVDescTable[i].CPUHandle(GBufferData::GBUFFER_METALNESS_ROUGHNESS));
		}
	}

	// motion vector
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_MOTION_VECTOR];

		data.MotionVec = ZetaMove(gpuMem.GetTexture2D("GBuffer_MotionVec", width, height,
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_MOTION_VECTOR],
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
			1,
			&clearValue));

		// RTV
		Direct3DHelper::CreateRTV(data.MotionVec, data.RTVDescTable[0].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));
		Direct3DHelper::CreateRTV(data.MotionVec, data.RTVDescTable[1].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));

		// SRV
		Direct3DHelper::CreateTexture2DSRV(data.MotionVec, data.SRVDescTable[0].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));
		Direct3DHelper::CreateTexture2DSRV(data.MotionVec, data.SRVDescTable[1].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));
	}

	// emissive color	
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_BASE_COLOR];

		data.EmissiveColor = ZetaMove(gpuMem.GetTexture2D("GBuffer_EmissiveColor", width, height,
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_EMISSIVE_COLOR],
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
			1,
			&clearValue));

		// RTV
		Direct3DHelper::CreateRTV(data.EmissiveColor, data.RTVDescTable[0].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));
		Direct3DHelper::CreateRTV(data.EmissiveColor, data.RTVDescTable[1].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));

		// SRV
		Direct3DHelper::CreateTexture2DSRV(data.EmissiveColor, data.SRVDescTable[0].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));
		Direct3DHelper::CreateTexture2DSRV(data.EmissiveColor, data.SRVDescTable[1].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));
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

			data.DepthBuffer[i] = ZetaMove(gpuMem.GetTexture2D(name, width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_DEPTH],
				D3D12_RESOURCE_STATE_DEPTH_WRITE,
				TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL,
				1,
				&clearValueDepth));
			
			// DSV
			device->CreateDepthStencilView(data.DepthBuffer[i].GetResource(), &desc,
				data.DSVDescTable[i].CPUHandle(0));

			// SRV
			Direct3DHelper::CreateTexture2DSRV(data.DepthBuffer[i], data.SRVDescTable[i].CPUHandle(GBufferData::GBUFFER_DEPTH), 
				DXGI_FORMAT_R32_FLOAT);
		}
	}
}

void GBuffer::OnWindowSizeChanged(const RenderSettings& settings, GBufferData& data) noexcept
{
	GBuffer::CreateGBuffers(data);
}

void GBuffer::Shutdown(GBufferData& data) noexcept
{
	data.GBuffPass.Reset();

	for (int i = 0; i < 2; i++)
	{
		data.BaseColor[i].Reset();
		data.Normal[i].Reset();
		data.DepthBuffer[i].Reset();
		data.MetalnessRoughness[i].Reset();
		data.RTVDescTable[i].Reset();
		data.SRVDescTable[i].Reset();
		data.DSVDescTable[i].Reset();
	}
	
	data.EmissiveColor.Reset();
	data.MotionVec.Reset();
}

void GBuffer::Update(GBufferData& gbuffData) noexcept
{
	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();
	SceneCore& scene = App::GetScene();
	Span<uint64_t> frameInstances = scene.GetFrameInstances();

	SmallVector<MeshInstance, App::FrameAllocator> instances;
	instances.resize(frameInstances.size());

	{
		size_t currInstance = 0;

		for (auto instanceID : frameInstances)
		{
			instances[currInstance].PrevWorld = float3x4(scene.GetPrevToWorld(instanceID));
			instances[currInstance].CurrWorld = float3x4(scene.GetToWorld(instanceID));

			currInstance++;
		}
	}

	{
		size_t currInstance = 0;

		for (auto instanceID : frameInstances)
		{
			const uint64_t meshID = scene.GetMeshIDForInstance(instanceID);
			const TriangleMesh mesh = scene.GetMesh(meshID);
			const Material mat = scene.GetMaterial(mesh.m_materialID);

			instances[currInstance].IndexCount = mesh.m_numIndices;
			instances[currInstance].BaseVtxOffset = (uint32_t)mesh.m_vtxBuffStartOffset;
			instances[currInstance].BaseIdxOffset = (uint32_t)mesh.m_idxBuffStartOffset;
			instances[currInstance].IdxInMatBuff = (uint16_t)mat.GpuBufferIndex();
			instances[currInstance].IsDoubleSided = mat.IsDoubleSided();

			currInstance++;
		}
	}

	// these change every frame
	gbuffData.GBuffPass.SetDescriptor(GBufferPass::SHADER_IN_DESC::GBUFFERS_RTV,
		gbuffData.RTVDescTable[outIdx].CPUHandle(0));
	gbuffData.GBuffPass.SetDescriptor(GBufferPass::SHADER_IN_DESC::CURR_DEPTH_BUFFER_DSV,
		gbuffData.DSVDescTable[outIdx].CPUHandle(0));

	gbuffData.GBuffPass.SetInstances(instances);

	// clear the gbuffers
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::BASE_COLOR,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER_BASE_COLOR));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::NORMAL,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER_NORMAL));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::METALNESS_ROUGHNESS,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER_METALNESS_ROUGHNESS));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::MOTION_VECTOR,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::EMISSIVE_COLOR,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::DEPTH_BUFFER,
		gbuffData.DSVDescTable[outIdx].CPUHandle(0));
}

void GBuffer::Register(GBufferData& data, RenderGraph& renderGraph) noexcept
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
		renderGraph.RegisterResource(data.BaseColor[i].GetResource(), data.BaseColor[i].GetPathID());
		renderGraph.RegisterResource(data.Normal[i].GetResource(), data.Normal[i].GetPathID());
		renderGraph.RegisterResource(data.MetalnessRoughness[i].GetResource(), data.MetalnessRoughness[i].GetPathID());
		renderGraph.RegisterResource(data.DepthBuffer[i].GetResource(), data.DepthBuffer[i].GetPathID(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}

	renderGraph.RegisterResource(data.MotionVec.GetResource(), data.MotionVec.GetPathID());
	renderGraph.RegisterResource(data.EmissiveColor.GetResource(), data.EmissiveColor.GetPathID());

	// when more than one RenderPass outputs one resource, it's unclear which one should run first.
	// add a made-up resource so that GBufferPass runs after Clear
	renderGraph.RegisterResource(nullptr, RenderGraph::DUMMY_RES::RES_0);
}

void GBuffer::DeclareAdjacencies(GBufferData& data, const LightData& lightData, RenderGraph& renderGraph) noexcept
{
	const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();

	// [hack] use D3D12_RESOURCE_STATE_UNORDERED_ACCESS, which can be considered as both readable and 
	// writable to avoid a Transition

	renderGraph.AddOutput(data.ClearHandle, data.BaseColor[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.Normal[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.MotionVec.GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.MetalnessRoughness[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.EmissiveColor.GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.DepthBuffer[outIdx].GetPathID(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	renderGraph.AddOutput(data.ClearHandle, RenderGraph::DUMMY_RES::RES_0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// make the GBufferPass dependant on Clear
	renderGraph.AddInput(data.GBuffPassHandle, RenderGraph::DUMMY_RES::RES_0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	renderGraph.AddOutput(data.GBuffPassHandle, data.BaseColor[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.Normal[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.MetalnessRoughness[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.MotionVec.GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.EmissiveColor.GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.DepthBuffer[outIdx].GetPathID(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
}
