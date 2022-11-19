#include "SceneRendererImpl.h"
#include "../../Core/Direct3DHelpers.h"
#include "../../Core/CommandList.h"
#include "../../Win32/App.h"

using namespace ZetaRay;
using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DHelper;
using namespace ZetaRay::Util;

void GBufferRenderer::Init(const RenderSettings& settings, GBufferRendererData& data) noexcept
{
	for (int i = 0; i < 2; i++)
	{
		data.RTVDescTable[i] = App::GetRenderer().GetRtvDescriptorHeap().Allocate(GBufferRendererData::COUNT);
		data.SRVDescTable[i] = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(GBufferRendererData::COUNT);
		data.DSVDescTable[i] = App::GetRenderer().GetDsvDescriptorHeap().Allocate(1);
	}

	CreateGBuffers(data);

	// initialize one render pass
	const int NUM_RTVs = GBufferRendererData::GBUFFER::COUNT - 1;

	DXGI_FORMAT rtvFormats[NUM_RTVs] = {
		GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_BASE_COLOR],
		GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_NORMAL_CURV],
		GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_METALNESS_ROUGHNESS],
		GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_MOTION_VECTOR],
		GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_EMISSIVE_COLOR] };

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = GetPSODesc(&VertexPosNormalTexTangent::InputLayout,
		NUM_RTVs,
		rtvFormats,
		RendererConstants::DEPTH_BUFFER_FORMAT);

	// reverse z
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;

	data.RenderPasses[0].Init(ZetaMove(psoDesc));
}

void GBufferRenderer::CreateGBuffers(GBufferRendererData& data) noexcept
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
		clearValue.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_BASE_COLOR];

		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "GBuffer_BaseColor_%d", i);

			data.BaseColor[i] = ZetaMove(gpuMem.GetTexture2D(name, 
				width, height,
				GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_BASE_COLOR],
				D3D12_RESOURCE_STATE_COMMON,
				TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
				1,
				&clearValue));

			rtvDesc.Format = GBufferRendererData::GBUFFER_FORMAT[::GBufferRendererData::GBUFFER_BASE_COLOR];

			// RTVs
			device->CreateRenderTargetView(data.BaseColor[i].GetResource(),
				&rtvDesc,
				data.RTVDescTable[i].CPUHandle(GBufferRendererData::GBUFFER_BASE_COLOR));

			// SRVs
			srvDesc.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER::GBUFFER_BASE_COLOR];

			device->CreateShaderResourceView(data.BaseColor[i].GetResource(),
				&srvDesc,
				data.SRVDescTable[i].CPUHandle(GBufferRendererData::GBUFFER_BASE_COLOR));
		}
	}

	// normal
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_NORMAL_CURV];

		rtvDesc.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_NORMAL_CURV];
		srvDesc.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_NORMAL_CURV];

		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "GBuffer_Normal_%d", i);

			data.Normal[i] = ZetaMove(gpuMem.GetTexture2D(name, width, height,
				GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_NORMAL_CURV],
				D3D12_RESOURCE_STATE_COMMON,
				TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
				1,
				&clearValue));

			device->CreateRenderTargetView(data.Normal[i].GetResource(),
				&rtvDesc,
				data.RTVDescTable[i].CPUHandle(GBufferRendererData::GBUFFER_NORMAL_CURV));

			device->CreateShaderResourceView(data.Normal[i].GetResource(),
				&srvDesc,
				data.SRVDescTable[i].CPUHandle(GBufferRendererData::GBUFFER_NORMAL_CURV));
		}
	}

	// metalness-roughness
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_METALNESS_ROUGHNESS];

		for (int i = 0; i < 2; i++)
		{
			StackStr(name, n, "GBuffer_Metalness-Roughness_%d", i);

			data.MetalnessRoughness[i] = ZetaMove(gpuMem.GetTexture2D(name, width, height,
				GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_METALNESS_ROUGHNESS],
				D3D12_RESOURCE_STATE_COMMON,
				TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
				1,
				&clearValue));

			// RTVs
			rtvDesc.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_METALNESS_ROUGHNESS];

			device->CreateRenderTargetView(data.MetalnessRoughness[i].GetResource(),
				&rtvDesc,
				data.RTVDescTable[i].CPUHandle(GBufferRendererData::GBUFFER_METALNESS_ROUGHNESS));

			// SRVs
			srvDesc.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_METALNESS_ROUGHNESS];

			device->CreateShaderResourceView(data.MetalnessRoughness[i].GetResource(),
				&srvDesc,
				data.SRVDescTable[i].CPUHandle(GBufferRendererData::GBUFFER_METALNESS_ROUGHNESS));
		}
	}

	// motion-vector
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_MOTION_VECTOR];

		data.MotionVec = ZetaMove(gpuMem.GetTexture2D("GBuffer_MotionVec", width, height,
			GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_MOTION_VECTOR],
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
			1,
			&clearValue));

		// RTVs
		rtvDesc.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_MOTION_VECTOR];

		device->CreateRenderTargetView(data.MotionVec.GetResource(),
			&rtvDesc,
			data.RTVDescTable[0].CPUHandle(GBufferRendererData::GBUFFER_MOTION_VECTOR));

		device->CreateRenderTargetView(data.MotionVec.GetResource(),
			&rtvDesc,
			data.RTVDescTable[1].CPUHandle(GBufferRendererData::GBUFFER_MOTION_VECTOR));

		// SRVs
		srvDesc.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_MOTION_VECTOR];

		device->CreateShaderResourceView(data.MotionVec.GetResource(),
			&srvDesc,
			data.SRVDescTable[0].CPUHandle(GBufferRendererData::GBUFFER_MOTION_VECTOR));

		device->CreateShaderResourceView(data.MotionVec.GetResource(),
			&srvDesc,
			data.SRVDescTable[1].CPUHandle(GBufferRendererData::GBUFFER_MOTION_VECTOR));
	}

	// emissive-color	
	{
		D3D12_CLEAR_VALUE clearValue = {};
		memset(clearValue.Color, 0, sizeof(float) * 4);
		clearValue.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_BASE_COLOR];

		data.EmissiveColor = ZetaMove(gpuMem.GetTexture2D("GBuffer_EmissiveColor", width, height,
			GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_EMISSIVE_COLOR],
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_RENDER_TARGET,
			1,
			&clearValue));

		// RTVs
		rtvDesc.Format = GBufferRendererData::GBUFFER_FORMAT[::GBufferRendererData::GBUFFER_EMISSIVE_COLOR];

		device->CreateRenderTargetView(data.EmissiveColor.GetResource(),
			&rtvDesc,
			data.RTVDescTable[0].CPUHandle(GBufferRendererData::GBUFFER_EMISSIVE_COLOR));

		device->CreateRenderTargetView(data.EmissiveColor.GetResource(),
			&rtvDesc,
			data.RTVDescTable[1].CPUHandle(GBufferRendererData::GBUFFER_EMISSIVE_COLOR));

		// SRVs
		srvDesc.Format = GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER::GBUFFER_EMISSIVE_COLOR];

		device->CreateShaderResourceView(data.EmissiveColor.GetResource(),
			&srvDesc,
			data.SRVDescTable[0].CPUHandle(GBufferRendererData::GBUFFER_EMISSIVE_COLOR));

		device->CreateShaderResourceView(data.EmissiveColor.GetResource(),
			&srvDesc,
			data.SRVDescTable[1].CPUHandle(GBufferRendererData::GBUFFER_EMISSIVE_COLOR));
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
				GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_DEPTH],
				D3D12_RESOURCE_STATE_DEPTH_WRITE,
				TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL,
				1,
				&clearValueDepth));

			device->CreateDepthStencilView(data.DepthBuffer[i].GetResource(), &desc,
				data.DSVDescTable[i].CPUHandle(0));

			device->CreateShaderResourceView(data.DepthBuffer[i].GetResource(), &srvDesc,
				data.SRVDescTable[i].CPUHandle(GBufferRendererData::GBUFFER_DEPTH));
		}
	}
}

void GBufferRenderer::OnWindowSizeChanged(const RenderSettings& settings, GBufferRendererData& data) noexcept
{
	GBufferRenderer::CreateGBuffers(data);
}

void GBufferRenderer::Shutdown(GBufferRendererData& data) noexcept
{
	for (int i = 0; i < GBufferRendererData::MAX_NUM_RENDER_PASSES; i++)
	{
		data.RenderPasses[i].Reset();
	}

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
void GBufferRenderer::Update(GBufferRendererData& gbuffData, const LightManagerData& lightManagerData) noexcept
{
	const int outIdx = App::GetRenderer().CurrOutIdx();
	SceneCore& scene = App::GetScene();
	const Vector<uint64_t>& frameInstances = scene.GetFrameInstances();
	const int oldNumRenderPasses = gbuffData.NumRenderPasses;

	if (frameInstances.empty())
		gbuffData.NumRenderPasses = 0;
	else
	{
		// compute number of render passes to use
		// for now, assign every 64 meshes to one pass, up to MAX_NUM_RENDER_PASSES
		size_t passOffsets[GBufferRendererData::MAX_NUM_RENDER_PASSES];
		size_t passSizes[GBufferRendererData::MAX_NUM_RENDER_PASSES];

		//gbuffData.NumRenderPasses = (int)Math::SubdivideRangeWithMin(332, GBufferRendererData::MAX_NUM_RENDER_PASSES,
		//	passOffsets, passSizes, 64llu);

		gbuffData.NumRenderPasses = (int)Math::SubdivideRangeWithMin(frameInstances.size(), GBufferRendererData::MAX_NUM_RENDER_PASSES,
			passOffsets, passSizes, 64llu);

		size_t currOffset = 0;

		for (int i = 0; i < gbuffData.NumRenderPasses; i++)
		{
			Assert(passSizes[i] > 0, "Number of meshes per-pass must be greater than zero");

			// fill in the draw arguments
			SmallVector<GBufferPass::InstanceData> instances;
			instances.resize(passSizes[i]);

			for (size_t currInstance = passOffsets[i]; currInstance < passOffsets[i] + passSizes[i]; currInstance++)
			{
				const uint64_t instanceID = frameInstances[currInstance];
				const uint64_t meshID = scene.GetMeshIDForInstance(instanceID);
				const MeshData mesh = scene.GetMeshData(meshID);
				const Material mat = scene.GetMaterial(mesh.MatID);
				const size_t frameInstanceIdx = currInstance - currOffset;

				instances[frameInstanceIdx].VB = mesh.VB;
				instances[frameInstanceIdx].IB = mesh.IB;
				instances[frameInstanceIdx].VBSizeInBytes = mesh.NumVertices * sizeof(VertexPosNormalTexTangent);
				instances[frameInstanceIdx].IBSizeInBytes = mesh.NumIndices * sizeof(INDEX_TYPE);
				instances[frameInstanceIdx].IndexCount = mesh.NumIndices;
				instances[frameInstanceIdx].IdxInMatBuff = mat.GpuBufferIndex();
				instances[frameInstanceIdx].PrevToWorld = scene.GetPrevToWorld(instanceID);
				instances[frameInstanceIdx].CurrToWorld = scene.GetToWorld(instanceID);
				instances[frameInstanceIdx].InstanceID = instanceID;
			}

			// exclude the depth-buffer
			const int NUM_RTVs = GBufferRendererData::GBUFFER::COUNT - 1;

			DXGI_FORMAT rtvFormats[NUM_RTVs] = {
				GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_BASE_COLOR],
				GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_NORMAL_CURV],
				GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_METALNESS_ROUGHNESS],
				GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_MOTION_VECTOR],
				GBufferRendererData::GBUFFER_FORMAT[GBufferRendererData::GBUFFER_EMISSIVE_COLOR] };

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = GetPSODesc(&VertexPosNormalTexTangent::InputLayout,
				NUM_RTVs,
				rtvFormats,
				RendererConstants::DEPTH_BUFFER_FORMAT);

			if (RendererConstants::USE_REVERSE_Z)
				psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;

			if (!gbuffData.RenderPasses[i].IsInitialized())
			{
				gbuffData.RenderPasses[i].Init(ZetaMove(psoDesc));
			}

			// these change every frame
			gbuffData.RenderPasses[i].SetDescriptor(GBufferPass::SHADER_IN_DESC::RTV,
				gbuffData.RTVDescTable[outIdx].CPUHandle(0));
			gbuffData.RenderPasses[i].SetDescriptor(GBufferPass::SHADER_IN_DESC::DEPTH_BUFFER,
				gbuffData.DSVDescTable[outIdx].CPUHandle(0));

			gbuffData.RenderPasses[i].SetInstances(ZetaMove(instances));

			currOffset += passSizes[i];
		}
	}

	if (oldNumRenderPasses > gbuffData.NumRenderPasses)
	{
		for (int i = std::max(1, gbuffData.NumRenderPasses); i < oldNumRenderPasses; i++)
		{
			gbuffData.RenderPasses[i].Reset();
		}
	}

	// clear the gbuffers
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::BASE_COLOR,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferRendererData::GBUFFER_BASE_COLOR));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::NORMAL,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferRendererData::GBUFFER_NORMAL_CURV));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::METALNESS_ROUGHNESS,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferRendererData::GBUFFER_METALNESS_ROUGHNESS));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::MOTION_VECTOR,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferRendererData::GBUFFER_MOTION_VECTOR));
	gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::EMISSIVE_COLOR,
		gbuffData.RTVDescTable[outIdx].CPUHandle(GBufferRendererData::GBUFFER_EMISSIVE_COLOR));
	gbuffData.ClearPass.SetDescriptor(GBufferPass::SHADER_IN_DESC::DEPTH_BUFFER,
		gbuffData.DSVDescTable[outIdx].CPUHandle(0));

	// additionally clear the HDR light accumulation texture (if initialized)
	if (!lightManagerData.HdrLightAccumRTV.IsEmpty())
	{
		gbuffData.ClearPass.SetDescriptor(ClearPass::SHADER_IN_DESC::HDR_LIGHT_ACCUM,
			lightManagerData.HdrLightAccumRTV.CPUHandle(0));
	}
}

void GBufferRenderer::Register(GBufferRendererData& data, RenderGraph& renderGraph) noexcept
{
	// Clear
	fastdelegate::FastDelegate1<CommandList&> clearDlg = fastdelegate::MakeDelegate(&data.ClearPass,
		&ClearPass::Clear);

	data.ClearHandle = renderGraph.RegisterRenderPass("Clear", RENDER_NODE_TYPE::RENDER, clearDlg);

	// Draw
	for (int i = 0; i < data.NumRenderPasses; i++)
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.RenderPasses[i],
			&GBufferPass::Render);

		StackStr(name, n, "GBufferPass_%d", i);
		data.Handles[i] = renderGraph.RegisterRenderPass(name, RENDER_NODE_TYPE::RENDER, dlg);
	}

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

void GBufferRenderer::DeclareAdjacencies(GBufferRendererData& data, const LightManagerData& lightManagerData, RenderGraph& renderGraph) noexcept
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

	if (!lightManagerData.HdrLightAccumRTV.IsEmpty())
		renderGraph.AddOutput(data.ClearHandle, lightManagerData.HdrLightAccumTex.GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);

	for (int i = 0; i < data.NumRenderPasses; i++)
	{
		// make the GBufferPass dependant on Clear
		renderGraph.AddInput(data.Handles[i], RenderGraph::DUMMY_RES::RES_0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		renderGraph.AddOutput(data.Handles[i], data.BaseColor[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		renderGraph.AddOutput(data.Handles[i], data.Normal[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		renderGraph.AddOutput(data.Handles[i], data.MetalnessRoughness[outIdx].GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		renderGraph.AddOutput(data.Handles[i], data.MotionVec.GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		renderGraph.AddOutput(data.Handles[i], data.EmissiveColor.GetPathID(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		renderGraph.AddOutput(data.Handles[i], data.DepthBuffer[outIdx].GetPathID(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
}
