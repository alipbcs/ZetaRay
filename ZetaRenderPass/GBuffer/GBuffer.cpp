#include "GBuffer.h"
#include <Core/RendererCore.h>
#include <Core/Vertex.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Scene/SceneCore.h>
#include <Math/MatrixFuncs.h>
#include <Model/Mesh.h>
#include <algorithm>

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Util;
using namespace ZetaRay::Model;

//--------------------------------------------------------------------------------------
// GBufferPass
//--------------------------------------------------------------------------------------

GBufferPass::GBufferPass() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// frame constants
	m_rootSig.InitAsCBV(0,							// root idx
		0,											// register num
		0,											// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::FRAME_CONSTANTS_BUFFER_NAME);

	// root constants
	m_rootSig.InitAsConstants(1,		// root idx
		NUM_CONSTS,						// num DWORDs
		1,								// register num
		0);								// register space

	// mesh buffer
	m_rootSig.InitAsBufferSRV(2,						// root idx
		0,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC);		// flags

	// scene VB
	m_rootSig.InitAsBufferSRV(3,						// root idx
		1,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		GlobalResource::SCENE_VERTEX_BUFFER);

	// scene IB
	m_rootSig.InitAsBufferSRV(4,						// root idx
		2,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		GlobalResource::SCENE_INDEX_BUFFER);

	// material buffer
	m_rootSig.InitAsBufferSRV(5,					// root idx
		3,											// register num
		0,											// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::MATERIAL_BUFFER);

	// indirect args
	m_rootSig.InitAsBufferUAV(6,					// root idx
		0,											// register num
		0,											// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
		D3D12_SHADER_VISIBILITY_ALL,
		nullptr,
		true);
}

GBufferPass::~GBufferPass() noexcept
{
	Reset();
}

void GBufferPass::Init(Util::Span<DXGI_FORMAT> rtvs) noexcept
{
	auto& renderer = App::GetRenderer();

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	auto samplers = App::GetRenderer().GetStaticSamplers();
	s_rpObjs.Init("GBufferPass", m_rootSig, samplers.size(), samplers.data(), flags);
	CreatePSOs(rtvs);

	// command signature
	D3D12_INDIRECT_ARGUMENT_DESC indirectCallArgs[2];
	indirectCallArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
	indirectCallArgs[0].Constant.RootParameterIndex = 1;
	indirectCallArgs[0].Constant.Num32BitValuesToSet = sizeof(cbGBuffer) / sizeof(DWORD);
	indirectCallArgs[0].Constant.DestOffsetIn32BitValues = 0;

	indirectCallArgs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

	D3D12_COMMAND_SIGNATURE_DESC desc;
	desc.ByteStride = sizeof(CommandSig);
	desc.NumArgumentDescs = ZetaArrayLen(indirectCallArgs);
	desc.pArgumentDescs = indirectCallArgs;
	desc.NodeMask = 0;

	CheckHR(renderer.GetDevice()->CreateCommandSignature(&desc, s_rpObjs.m_rootSig.Get(), IID_PPV_ARGS(m_cmdSig.GetAddressOf())));

	// create a zero-initialized buffer for resetting the counter
	m_zeroBuffer = renderer.GetGpuMemory().GetDefaultHeapBuffer("Zero",
		sizeof(uint32_t),
		D3D12_RESOURCE_STATE_COMMON,
		false,
		true);
}

void GBufferPass::Reset() noexcept
{
	if (m_graphicsPso[0])
	{
		s_rpObjs.Clear();
		memset(m_graphicsPso, 0, sizeof(ID3D12PipelineState*) * ZetaArrayLen(m_graphicsPso));
	}

	m_meshInstances.Reset();
	m_indirectDrawArgs.Reset();
	m_zeroBuffer.Reset();
	m_maxNumDrawCallsSoFar = 0;

#ifdef _DEBUG
	memset(m_inputDescriptors, 0, sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) * SHADER_IN_DESC::COUNT);
#endif // _DEBUG
}

void GBufferPass::SetInstances(Span<MeshInstance> instances) noexcept
{
	m_numMeshesThisFrame = (uint32_t)instances.size();

	if (m_numMeshesThisFrame == 0)
		return;

	auto split = std::partition(instances.begin(), instances.end(),
		[](const MeshInstance& mesh)
		{
			return !mesh.IsDoubleSided;
		});

	m_numSingleSidedMeshes = (uint32_t)(split - instances.begin());

	auto& gpuMem = App::GetRenderer().GetGpuMemory();

	// TODO recreating every frame can be avoided if the existing one is large enough
	const size_t meshInsBuffSizeInBytes = sizeof(MeshInstance) * m_numMeshesThisFrame;
	m_meshInstances = gpuMem.GetDefaultHeapBufferAndInit("GBufferMeshInstances",
		meshInsBuffSizeInBytes,
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
		false,
		instances.data());

	// avoid recreating the indirect args buffer if the existing one is large enough
	if (m_maxNumDrawCallsSoFar < m_numMeshesThisFrame)
	{
		m_maxNumDrawCallsSoFar = m_numMeshesThisFrame;

		// extra 8 bytes for the counters
		m_counterSingleSidedBufferOffset = sizeof(CommandSig) * m_maxNumDrawCallsSoFar;
		m_counterDoubleSidedBufferOffset = m_counterSingleSidedBufferOffset + sizeof(uint32_t);

		const size_t indDrawArgsBuffSizeInBytes = m_counterDoubleSidedBufferOffset + sizeof(uint32_t);

		m_indirectDrawArgs = gpuMem.GetDefaultHeapBuffer("IndirectDrawArgs",
			indDrawArgsBuffSizeInBytes,
			D3D12_RESOURCE_STATE_COMMON,
			true);
	}
}

void GBufferPass::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");

	if (m_numMeshesThisFrame == 0)
		return;

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();

	// Occlusion culling
	{
		ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

		computeCmdList.PIXBeginEvent("OcclusionCulling");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "OcclusionCulling");

		computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
		computeCmdList.SetPipelineState(m_computePsos[(int)COMPUTE_SHADERS::OCCLUSION_CULLING]);

		cbOcclussionCulling localCB;
		m_rootSig.SetRootSRV(2, m_meshInstances.GetGpuVA());
		m_rootSig.SetRootUAV(6, m_indirectDrawArgs.GetGpuVA());

		computeCmdList.ResourceBarrier(m_indirectDrawArgs.GetResource(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_COPY_DEST);

		// clear the counters
		computeCmdList.CopyBufferRegion(m_indirectDrawArgs.GetResource(),
			m_counterSingleSidedBufferOffset,
			m_zeroBuffer.GetResource(),
			0,
			sizeof(uint32_t) + sizeof(uint32_t));

		computeCmdList.ResourceBarrier(m_indirectDrawArgs.GetResource(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		if (m_numSingleSidedMeshes)
		{
			localCB.NumMeshes = m_numSingleSidedMeshes;
			localCB.CounterBufferOffset = m_counterSingleSidedBufferOffset;
			localCB.MeshBufferStartIndex = 0;
			localCB.ArgBufferStartOffsetInBytes = 0;

			m_rootSig.SetRootConstants(0, sizeof(localCB) / sizeof(DWORD), &localCB);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch((uint32_t)CeilUnsignedIntDiv(m_numSingleSidedMeshes, OCCLUSION_CULL_THREAD_GROUP_SIZE_X),
				1, 1);
		}

		// avoid an empty dispatch call
		if (m_numSingleSidedMeshes < m_numMeshesThisFrame)
		{
			const uint32_t numDoubleSided = m_numMeshesThisFrame - m_numSingleSidedMeshes;
			localCB.NumMeshes = numDoubleSided;
			localCB.CounterBufferOffset = m_counterDoubleSidedBufferOffset;
			localCB.ArgBufferStartOffsetInBytes = m_numSingleSidedMeshes * sizeof(CommandSig);
			localCB.MeshBufferStartIndex = m_numSingleSidedMeshes;

			m_rootSig.SetRootConstants(0, sizeof(localCB) / sizeof(DWORD), &localCB);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch((uint32_t)CeilUnsignedIntDiv(numDoubleSided, OCCLUSION_CULL_THREAD_GROUP_SIZE_X),
				1, 1);
		}

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	// GBuffer
	{
		GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

		directCmdList.PIXBeginEvent("GBufferPass");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(directCmdList, "GBufferPass");

		directCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

		m_rootSig.SetRootSRV(2, m_meshInstances.GetGpuVA());
		m_rootSig.End(directCmdList);

		D3D12_VIEWPORT viewports[(int)SHADER_OUT::COUNT - 1] =
		{
			renderer.GetRenderViewport(),
			renderer.GetRenderViewport(),
			renderer.GetRenderViewport(),
			renderer.GetRenderViewport(),
			renderer.GetRenderViewport()
		};

		D3D12_RECT scissors[(int)SHADER_OUT::COUNT - 1] =
		{
			renderer.GetRenderScissor(),
			renderer.GetRenderScissor(),
			renderer.GetRenderScissor(),
			renderer.GetRenderScissor(),
			renderer.GetRenderScissor()
		};

		static_assert(ZetaArrayLen(viewports) == (int)SHADER_OUT::COUNT - 1, "bug");
		static_assert(ZetaArrayLen(scissors) == (int)SHADER_OUT::COUNT - 1, "bug");

		const Core::DefaultHeapBuffer& sceneIB = App::GetScene().GetMeshIB();
		Assert(sceneIB.IsInitialized(), "IB hasn't been built yet.");
		const auto ibGpuVa = sceneIB.GetGpuVA();

		D3D12_INDEX_BUFFER_VIEW ibv;
		ibv.BufferLocation = ibGpuVa;
		ibv.SizeInBytes = (UINT)sceneIB.GetDesc().Width;
		ibv.Format = DXGI_FORMAT_R32_UINT;

		directCmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		directCmdList.IASetIndexBuffer(ibv);
		directCmdList.RSSetViewportsScissorsRects((int)SHADER_OUT::COUNT - 1, viewports, scissors);

		Assert(m_inputDescriptors[(int)SHADER_IN_DESC::GBUFFERS_RTV].ptr > 0, "GBuffers RTV hasn't been set.");
		Assert(m_inputDescriptors[(int)SHADER_IN_DESC::CURR_DEPTH_BUFFER_DSV].ptr > 0, "Depth buffer DSV hasn't been set.");

		directCmdList.OMSetRenderTargets((int)SHADER_OUT::COUNT - 1, &m_inputDescriptors[SHADER_IN_DESC::GBUFFERS_RTV],
			true, &m_inputDescriptors[(int)SHADER_IN_DESC::CURR_DEPTH_BUFFER_DSV]);

		directCmdList.ResourceBarrier(m_indirectDrawArgs.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

		// single-sided meshes
		if (m_numSingleSidedMeshes)
		{
			directCmdList.SetPipelineState(m_graphicsPso[(int)PSO::ONE_SIDED]);

			directCmdList.ExecuteIndirect(m_cmdSig.Get(),
				m_numSingleSidedMeshes,
				m_indirectDrawArgs.GetResource(),
				0,
				m_indirectDrawArgs.GetResource(),
				m_counterSingleSidedBufferOffset);
		}

		// double-sided meshes
		if (m_numSingleSidedMeshes < m_numMeshesThisFrame)
		{
			const uint32_t numDoubleSided = m_numMeshesThisFrame - m_numSingleSidedMeshes;
			const uint32_t argBuffStartOffset = m_numSingleSidedMeshes * sizeof(CommandSig);

			directCmdList.SetPipelineState(m_graphicsPso[(int)PSO::DOUBLE_SIDED]);

			directCmdList.ExecuteIndirect(m_cmdSig.Get(),
				numDoubleSided,
				m_indirectDrawArgs.GetResource(),
				argBuffStartOffset,
				m_indirectDrawArgs.GetResource(),
				m_counterDoubleSidedBufferOffset);
		}

		// record the timestamp after execution
		gpuTimer.EndQuery(directCmdList, queryIdx);

		directCmdList.PIXEndEvent();
	}
}

void GBufferPass::CreatePSOs(Util::Span<DXGI_FORMAT> rtvs) noexcept
{
	for (int i = 0; i < (int)COMPUTE_SHADERS::COUNT; i++)
	{
		m_computePsos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
			s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[i]);
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DHelper::GetPSODesc(nullptr,
		(int)rtvs.size(),
		rtvs.data(),
		Constants::DEPTH_BUFFER_FORMAT);

	//D3D12_INPUT_ELEMENT_DESC inputElements[] =
	//{
	//	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	//	{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	//	{ "TEXUV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	//	{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	//};

	//D3D12_INPUT_LAYOUT_DESC inputLayout = D3D12_INPUT_LAYOUT_DESC{ .pInputElementDescs = inputElements, .NumElements = ZetaArrayLen(inputElements) };

	// reverse z
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;

	m_graphicsPso[(int)PSO::ONE_SIDED] = s_rpObjs.m_psoLib.GetGraphicsPSO((int)COMPUTE_SHADERS::COUNT + (int)PSO::ONE_SIDED,
		psoDesc,
		s_rpObjs.m_rootSig.Get(),
		COMPILED_VS[0],
		COMPILED_PS[0]);

	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	m_graphicsPso[(int)PSO::DOUBLE_SIDED] = s_rpObjs.m_psoLib.GetGraphicsPSO((int)COMPUTE_SHADERS::COUNT + (int)PSO::DOUBLE_SIDED,
		psoDesc,
		s_rpObjs.m_rootSig.Get(),
		COMPILED_VS[0],
		COMPILED_PS[0]);
}
