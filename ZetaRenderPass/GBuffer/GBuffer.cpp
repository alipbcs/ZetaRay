#include "GBuffer.h"
#include <Core/RendererCore.h>
#include <Core/Vertex.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Scene/SceneCore.h>
#include <Math/MatrixFuncs.h>
#include <Model/Mesh.h>

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

void GBufferPass::Init(D3D12_GRAPHICS_PIPELINE_STATE_DESC&& psoDesc) noexcept
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

	// PSOs
	for (int i = 0; i < (int)COMPUTE_SHADERS::COUNT; i++)
	{
		m_computePsos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
			s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[i]);
	}

	m_graphicsPso = s_rpObjs.m_psoLib.GetGraphicsPSO((int)COMPUTE_SHADERS::COUNT, psoDesc, s_rpObjs.m_rootSig.Get(), 
		COMPILED_VS[0], COMPILED_PS[0]);

	// create a zero-initialized buffer for resetting the counter
	m_zeroBuffer = renderer.GetGpuMemory().GetDefaultHeapBuffer("Zero", 
		sizeof(uint32_t), 
		D3D12_RESOURCE_STATE_COMMON, 
		false, 
		true);
}

void GBufferPass::Reset() noexcept
{
	if (m_graphicsPso)
	{
		s_rpObjs.Clear();
		m_graphicsPso = nullptr;
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

	auto& gpuMem = App::GetRenderer().GetGpuMemory();

	// TODO recreating every frame can be avoided if the existing one is large enough
	const size_t meshInsBuffsizeInBytes = sizeof(MeshInstance) * m_numMeshesThisFrame;
	m_meshInstances = gpuMem.GetDefaultHeapBufferAndInit("GBufferMeshInstances",
		meshInsBuffsizeInBytes,
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
		false,
		instances.data());

	// avoid recreating the indirect args buffer if the existing one is large enough
	if (m_maxNumDrawCallsSoFar < m_numMeshesThisFrame)
	{
		m_maxNumDrawCallsSoFar = m_numMeshesThisFrame;

		// extra 4 bytes for the counter
		m_counterBufferOffset = sizeof(CommandSig) * m_maxNumDrawCallsSoFar;
		const size_t indDrawArgsBuffsizeInBytes = m_counterBufferOffset + sizeof(uint32_t);

		m_indirectDrawArgs = gpuMem.GetDefaultHeapBuffer("IndirectDrawArgs",
			indDrawArgsBuffsizeInBytes,
			D3D12_RESOURCE_STATE_COMMON,
			true);
	}

	// descriptor table needs to be recreated every frame
	m_gpuDescTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((uint32_t)DESC_TABLE::COUNT);

	Direct3DHelper::CreateBufferSRV(m_meshInstances,
		m_gpuDescTable.CPUHandle((uint32_t)DESC_TABLE::MESH_INSTANCES_SRV),
		sizeof(MeshInstance),
		m_numMeshesThisFrame);

	Direct3DHelper::CreateRawBufferUAV(m_indirectDrawArgs,
		m_gpuDescTable.CPUHandle((uint32_t)DESC_TABLE::INDIRECT_ARGS_UAV),
		sizeof(CommandSig),
		m_numMeshesThisFrame);
}

void GBufferPass::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");

	if (m_numMeshesThisFrame == 0)
		return;

	// Occlusion culling
	{
		ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

		computeCmdList.PIXBeginEvent("OcclusionCulling");
		
		computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
		computeCmdList.SetPipelineState(m_computePsos[(int)COMPUTE_SHADERS::OCCLUSION_CULLING]);

		cbOcclussionCulling localCB;
		localCB.NumMeshes = m_numMeshesThisFrame;
		localCB.CounterBufferOffset = m_counterBufferOffset;

		m_rootSig.SetRootConstants(0, sizeof(localCB) / sizeof(DWORD), &localCB);
		m_rootSig.SetRootSRV(2, m_meshInstances.GetGpuVA());
		m_rootSig.SetRootUAV(6, m_indirectDrawArgs.GetGpuVA());
		m_rootSig.End(computeCmdList);

		computeCmdList.TransitionResource(m_indirectDrawArgs.GetResource(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_COPY_DEST);

		// clear the counter
		computeCmdList.CopyBufferRegion(m_indirectDrawArgs.GetResource(),
			m_counterBufferOffset, 
			m_zeroBuffer.GetResource(), 
			0, 
			sizeof(uint32_t));

		computeCmdList.TransitionResource(m_indirectDrawArgs.GetResource(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		computeCmdList.Dispatch((uint32_t)CeilUnsignedIntDiv(m_numMeshesThisFrame, OCCLUSION_CULL_THREAD_GROUP_SIZE_X),
			1, 1);

		computeCmdList.PIXEndEvent();
	}

	// GBuffer
	{
		GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

		directCmdList.PIXBeginEvent("GBufferPass");

		directCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
		directCmdList.SetPipelineState(m_graphicsPso);

		m_rootSig.SetRootSRV(2, m_meshInstances.GetGpuVA());
		m_rootSig.End(directCmdList);

		D3D12_VIEWPORT viewports[(int)SHADER_OUT::COUNT - 1] =
		{
			App::GetRenderer().GetRenderViewport(),
			App::GetRenderer().GetRenderViewport(),
			App::GetRenderer().GetRenderViewport(),
			App::GetRenderer().GetRenderViewport(),
			App::GetRenderer().GetRenderViewport()
		};

		D3D12_RECT scissors[(int)SHADER_OUT::COUNT - 1] =
		{
			App::GetRenderer().GetRenderScissor(),
			App::GetRenderer().GetRenderScissor(),
			App::GetRenderer().GetRenderScissor(),
			App::GetRenderer().GetRenderScissor(),
			App::GetRenderer().GetRenderScissor()
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

		directCmdList.TransitionResource(m_indirectDrawArgs.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

		directCmdList.ExecuteIndirect(m_cmdSig.Get(),
			m_numMeshesThisFrame,
			m_indirectDrawArgs.GetResource(),
			0,
			m_indirectDrawArgs.GetResource(),
			m_counterBufferOffset);

		directCmdList.PIXEndEvent();
	}
}

