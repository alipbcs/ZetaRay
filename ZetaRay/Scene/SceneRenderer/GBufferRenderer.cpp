#include "SceneRendererImpl.h"
#include "../../Core/Direct3DHelpers.h"
#include "../../Core/CommandList.h"
#include "../../App/App.h"

using namespace ZetaRay::Math;
using namespace ZetaRay::Model;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Scene::Render;
using namespace ZetaRay::Core;
using namespace ZetaRay::Util;

void GBuffer::Init(const RenderSettings& settings, GBufferData& data) noexcept
{
	for (int i = 0; i < 2; i++)
	{
		data.RTVDescTable[i] = App::GetRenderer().GetRtvDescriptorHeap().Allocate(GBufferData::COUNT);
		data.SRVDescTable[i] = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(GBufferData::COUNT);
		data.DSVDescTable[i] = App::GetRenderer().GetDsvDescriptorHeap().Allocate(1);
	}

	CreateGBuffers(data);

	// initialize one render pass
	const int NUM_RTVs = GBufferData::GBUFFER::COUNT - 1;

	DXGI_FORMAT rtvFormats[NUM_RTVs] = {
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_BASE_COLOR],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_NORMAL],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_METALNESS_ROUGHNESS],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_MOTION_VECTOR],
		GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_EMISSIVE_COLOR] };

	D3D12_INPUT_ELEMENT_DESC inputElements[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXUV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_INPUT_LAYOUT_DESC inputLayout = D3D12_INPUT_LAYOUT_DESC{ .pInputElementDescs = inputElements, .NumElements = ZetaArrayLen(inputElements) };

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DHelper::GetPSODesc(&inputLayout,
		NUM_RTVs,
		rtvFormats,
		RendererConstants::DEPTH_BUFFER_FORMAT);

	// reverse z
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;

	data.GBuffPass.Init(ZetaMove(psoDesc));
}

void GBuffer::CreateGBuffers(GBufferData& data) noexcept
{
	auto* device = App::GetRenderer().GetDevice();
	auto& gpuMem = App::GetRenderer().GetGpuMemory();
	const int width = App::GetRenderer().GetRenderWidth();
	const int height = App::GetRenderer().GetRenderHeight();

	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	rtvDesc.Texture2D.MipSlice = 0;
	rtvDesc.Texture2D.PlaneSlice = 0;

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.PlaneSlice = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

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

			rtvDesc.Format = GBufferData::GBUFFER_FORMAT[::GBufferData::GBUFFER_BASE_COLOR];

			// RTVs
			device->CreateRenderTargetView(data.BaseColor[i].GetResource(),
				&rtvDesc,
				data.RTVDescTable[i].CPUHandle(GBufferData::GBUFFER_BASE_COLOR));

			// SRVs
			srvDesc.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::GBUFFER_BASE_COLOR];

			device->CreateShaderResourceView(data.BaseColor[i].GetResource(),
				&srvDesc,
				data.SRVDescTable[i].CPUHandle(GBufferData::GBUFFER_BASE_COLOR));
		}
	}

	// normal
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_NORMAL];

		rtvDesc.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_NORMAL];
		srvDesc.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_NORMAL];

		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "GBuffer_Normal_%d", i);

			data.Normal[i] = ZetaMove(gpuMem.GetTexture2D(name, width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_NORMAL],
				D3D12_RESOURCE_STATE_COMMON,
				TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
				1,
				&clearValue));

			device->CreateRenderTargetView(data.Normal[i].GetResource(),
				&rtvDesc,
				data.RTVDescTable[i].CPUHandle(GBufferData::GBUFFER_NORMAL));

			device->CreateShaderResourceView(data.Normal[i].GetResource(),
				&srvDesc,
				data.SRVDescTable[i].CPUHandle(GBufferData::GBUFFER_NORMAL));
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

			// RTVs
			rtvDesc.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_METALNESS_ROUGHNESS];

			device->CreateRenderTargetView(data.MetalnessRoughness[i].GetResource(),
				&rtvDesc,
				data.RTVDescTable[i].CPUHandle(GBufferData::GBUFFER_METALNESS_ROUGHNESS));

			// SRVs
			srvDesc.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_METALNESS_ROUGHNESS];

			device->CreateShaderResourceView(data.MetalnessRoughness[i].GetResource(),
				&srvDesc,
				data.SRVDescTable[i].CPUHandle(GBufferData::GBUFFER_METALNESS_ROUGHNESS));
		}
	}

	// motion-vector
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

		// RTVs
		rtvDesc.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_MOTION_VECTOR];

		device->CreateRenderTargetView(data.MotionVec.GetResource(),
			&rtvDesc,
			data.RTVDescTable[0].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));

		device->CreateRenderTargetView(data.MotionVec.GetResource(),
			&rtvDesc,
			data.RTVDescTable[1].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));

		// SRVs
		srvDesc.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_MOTION_VECTOR];

		device->CreateShaderResourceView(data.MotionVec.GetResource(),
			&srvDesc,
			data.SRVDescTable[0].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));

		device->CreateShaderResourceView(data.MotionVec.GetResource(),
			&srvDesc,
			data.SRVDescTable[1].CPUHandle(GBufferData::GBUFFER_MOTION_VECTOR));
	}

	// emissive-color	
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

		// RTVs
		rtvDesc.Format = GBufferData::GBUFFER_FORMAT[::GBufferData::GBUFFER_EMISSIVE_COLOR];

		device->CreateRenderTargetView(data.EmissiveColor.GetResource(),
			&rtvDesc,
			data.RTVDescTable[0].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));

		device->CreateRenderTargetView(data.EmissiveColor.GetResource(),
			&rtvDesc,
			data.RTVDescTable[1].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));

		// SRVs
		srvDesc.Format = GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER::GBUFFER_EMISSIVE_COLOR];

		device->CreateShaderResourceView(data.EmissiveColor.GetResource(),
			&srvDesc,
			data.SRVDescTable[0].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));

		device->CreateShaderResourceView(data.EmissiveColor.GetResource(),
			&srvDesc,
			data.SRVDescTable[1].CPUHandle(GBufferData::GBUFFER_EMISSIVE_COLOR));
	}

	// depth
	{
		D3D12_CLEAR_VALUE clearValueDepth = {};
		clearValueDepth.Format = RendererConstants::DEPTH_BUFFER_FORMAT;
		clearValueDepth.DepthStencil.Depth = RendererConstants::USE_REVERSE_Z ? 0.0f : 1.0f;
		clearValueDepth.DepthStencil.Stencil = 0;

		D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
		desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		desc.Flags = D3D12_DSV_FLAG_NONE;
		desc.Format = RendererConstants::DEPTH_BUFFER_FORMAT;
		desc.Texture2D.MipSlice = 0;

		srvDesc.Format = DXGI_FORMAT_R32_FLOAT;

		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "DepthBuffer_%d", i);

			data.DepthBuffer[i] = ZetaMove(gpuMem.GetTexture2D(name, width, height,
				GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_DEPTH],
				D3D12_RESOURCE_STATE_DEPTH_WRITE,
				TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL,
				1,
				&clearValueDepth));

			device->CreateDepthStencilView(data.DepthBuffer[i].GetResource(), &desc,
				data.DSVDescTable[i].CPUHandle(0));

			device->CreateShaderResourceView(data.DepthBuffer[i].GetResource(), &srvDesc,
				data.SRVDescTable[i].CPUHandle(GBufferData::GBUFFER_DEPTH));
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

// Assigns meshes to GBufferRenderPass instances and prepares draw call arguments
void GBuffer::Update(GBufferData& gbuffData, const LightData& lightData) noexcept
{
	const int outIdx = App::GetRenderer().CurrOutIdx();
	SceneCore& scene = App::GetScene();
	Span<uint64_t> frameInstances = scene.GetFrameInstances();

	if (frameInstances.size() && !gbuffData.GBuffPass.IsInitialized())
	{
		D3D12_INPUT_ELEMENT_DESC inputElements[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXUV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		D3D12_INPUT_LAYOUT_DESC inputLayout = D3D12_INPUT_LAYOUT_DESC{ .pInputElementDescs = inputElements,
			.NumElements = ZetaArrayLen(inputElements) };

		// exclude the depth-buffer
		const int NUM_RTVs = GBufferData::GBUFFER::COUNT - 1;

		DXGI_FORMAT rtvFormats[NUM_RTVs] = {
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_BASE_COLOR],
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_NORMAL],
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_METALNESS_ROUGHNESS],
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_MOTION_VECTOR],
			GBufferData::GBUFFER_FORMAT[GBufferData::GBUFFER_EMISSIVE_COLOR] };

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DHelper::GetPSODesc(&inputLayout,
			NUM_RTVs,
			rtvFormats,
			RendererConstants::DEPTH_BUFFER_FORMAT);

		// reverse z
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;

		gbuffData.GBuffPass.Init(ZetaMove(psoDesc));
	}

	// fill in the draw arguments
	SmallVector<GBufferPass::InstanceData, App::FrameAllocator> instances;
	instances.resize(frameInstances.size());

	size_t currInstance = 0;

	for (auto instanceID : frameInstances)
	{
		const uint64_t meshID = scene.GetMeshIDForInstance(instanceID);
		const TriangleMesh mesh = scene.GetMesh(meshID);
		const Material mat = scene.GetMaterial(mesh.m_materialID);

		instances[currInstance].VertexCount = mesh.m_numVertices;
		instances[currInstance].VBStartOffsetInBytes = mesh.m_vtxBuffStartOffset * sizeof(Vertex);
		instances[currInstance].IndexCount = mesh.m_numIndices;
		instances[currInstance].IBStartOffsetInBytes = mesh.m_idxBuffStartOffset * sizeof(uint32_t);
		instances[currInstance].IdxInMatBuff = mat.GpuBufferIndex();
		instances[currInstance].PrevToWorld = scene.GetPrevToWorld(instanceID);
		instances[currInstance].CurrToWorld = scene.GetToWorld(instanceID);
		instances[currInstance].InstanceID = instanceID;

		currInstance++;
	}

	// these change every frame
	gbuffData.GBuffPass.SetDescriptor(GBufferPass::SHADER_IN_DESC::RTV,
		gbuffData.RTVDescTable[outIdx].CPUHandle(0));
	gbuffData.GBuffPass.SetDescriptor(GBufferPass::SHADER_IN_DESC::DEPTH_BUFFER,
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
	gbuffData.ClearPass.SetDescriptor(GBufferPass::SHADER_IN_DESC::DEPTH_BUFFER,
		gbuffData.DSVDescTable[outIdx].CPUHandle(0));

	// additionally clear the HDR light accumulation texture (if initialized)
	if (!lightData.HdrLightAccumRTV.IsEmpty())
	{
		gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::HDR_LIGHT_ACCUM,
			lightData.HdrLightAccumRTV.CPUHandle(0));
	}
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
	const int outIdx = App::GetRenderer().CurrOutIdx();

	// [hack] use D3D12_RESOURCE_STATE_UNORDERED_ACCESS, which can be considered as both readable and 
	// writable to avoid a Transition

	renderGraph.AddOutput(data.ClearHandle, data.BaseColor[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.Normal[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.MotionVec.GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.MetalnessRoughness[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.EmissiveColor.GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.ClearHandle, data.DepthBuffer[outIdx].GetPathID(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	renderGraph.AddOutput(data.ClearHandle, RenderGraph::DUMMY_RES::RES_0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	if (!lightData.HdrLightAccumRTV.IsEmpty())
		renderGraph.AddOutput(data.ClearHandle, lightData.HdrLightAccumTex.GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);

	// make the GBufferPass dependant on Clear
	renderGraph.AddInput(data.GBuffPassHandle, RenderGraph::DUMMY_RES::RES_0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	renderGraph.AddOutput(data.GBuffPassHandle, data.BaseColor[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.Normal[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.MetalnessRoughness[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.MotionVec.GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.EmissiveColor.GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderGraph.AddOutput(data.GBuffPassHandle, data.DepthBuffer[outIdx].GetPathID(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

}
