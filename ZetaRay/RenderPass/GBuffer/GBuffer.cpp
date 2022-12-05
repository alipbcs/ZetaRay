#include "GBuffer.h"
#include "../../Core/Renderer.h"
#include "../../Core/Vertex.h"
#include "../../Core/CommandList.h"
#include "../../Scene/SceneRenderer/SceneRenderer.h"
#include "../../Math/MatrixFuncs.h"
#include "../../Scene/SceneCore.h"

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Util;

//--------------------------------------------------------------------------------------
// GBufferPass
//--------------------------------------------------------------------------------------

GBufferPass::GBufferPass() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// instance buffer
	m_rootSig.InitAsCBV(0,							// root idx
		0,											// register num
		0,											// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		D3D12_SHADER_VISIBILITY_VERTEX);

	// frame constants
	m_rootSig.InitAsCBV(1,							// root idx
		1,											// register num
		0,											// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		D3D12_SHADER_VISIBILITY_ALL,
		SceneRenderer::FRAME_CONSTANTS_BUFFER_NAME);

	// material buffer
	m_rootSig.InitAsBufferSRV(2,					// root idx
		0,											// register num
		0,											// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		D3D12_SHADER_VISIBILITY_PIXEL,
		SceneRenderer::MATERIAL_BUFFER);
}

GBufferPass::~GBufferPass() noexcept
{
	Reset();
}

void GBufferPass::Init(D3D12_GRAPHICS_PIPELINE_STATE_DESC&& psoDesc) noexcept
{
	auto& renderer = App::GetRenderer();
	auto& scene = App::GetScene();
	
	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	auto* samplers = App::GetRenderer().GetStaticSamplers();
	s_rpObjs.Init("GBufferPass", m_rootSig, RendererConstants::NUM_STATIC_SAMPLERS, samplers, flags);

	m_pso = s_rpObjs.m_psoLib.GetGraphicsPSO(0, psoDesc, s_rpObjs.m_rootSig.Get(), COMPILED_VS[0], COMPILED_PS[0]);
}

void GBufferPass::Reset() noexcept
{
	if (m_pso)
		s_rpObjs.Clear();

	m_perDrawCallArgs.free_memory();
	m_perDrawCB.Reset();
	m_pso = nullptr;
	//m_rootSig.Reset();

#ifdef _DEBUG
	memset(m_descriptors, 0, sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) * SHADER_IN_DESC::COUNT);
#endif // _DEBUG
}

void GBufferPass::SetInstances(Span<InstanceData> instances) noexcept
{
	auto& gpuMem = App::GetRenderer().GetGpuMemory();
	constexpr size_t instanceSize = Math::AlignUp(sizeof(DrawCB), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
	m_perDrawCB = gpuMem.GetUploadHeapBuffer(instanceSize * instances.size(), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

	{
		DrawCB cb;

		for (int i = 0; i < instances.size(); i++)
		{
			cb.CurrWorld = float3x4(instances[i].CurrToWorld);
			cb.PrevWorld = float3x4(instances[i].PrevToWorld);
			cb.MatID = instances[i].IdxInMatBuff;

			m_perDrawCB.Copy(i * instanceSize, sizeof(DrawCB), &cb);
		}
	}

	{
		// Warning: m_perDrawCallArgs is a class member and appears to persist between frames,
		// yet since it's using FrameAllocator for SmallVector allocations, its capacity must be
		// set to zero before usage in each frame, otherwise it might attempt to reuse previous
		// frame's temp memory
		m_perDrawCallArgs.free_memory();
		m_perDrawCallArgs.resize(instances.size());

		for (int i = 0; i < instances.size(); i++)
		{
			m_perDrawCallArgs[i].VBStartOffsetInBytes = instances[i].VBStartOffsetInBytes;
			m_perDrawCallArgs[i].VertexCount = instances[i].VertexCount;
			m_perDrawCallArgs[i].IBStartOffsetInBytes = instances[i].IBStartOffsetInBytes;
			m_perDrawCallArgs[i].IndexCount = instances[i].IndexCount;
			//m_perDrawCallArgs[i].MeshID = instances[i].MeshID;
			m_perDrawCallArgs[i].InstanceID = instances[i].InstanceID;
		}
	}
}

void GBufferPass::Render(CommandList& cmdList) noexcept
{
	//Assert(m_rtvTable->GetNumDescriptors() == SHADER_OUT::COUNT - 1, "Mismatch between #RTVs and #GBuffers");
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
	GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);
	constexpr size_t instanceSize = Math::AlignUp(sizeof(DrawCB), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

	directCmdList.PIXBeginEvent("GBufferPass");

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

	Assert(sizeof(viewports) / sizeof(D3D12_VIEWPORT) == (int)SHADER_OUT::COUNT - 1, "bug");
	Assert(sizeof(scissors) / sizeof(D3D12_RECT) == (int)SHADER_OUT::COUNT - 1, "bug");

	directCmdList.RSSetViewportsScissorsRects((int)SHADER_OUT::COUNT - 1, viewports, scissors);

	Assert(m_descriptors[(int)SHADER_IN_DESC::RTV].ptr > 0, "RTV hasn't been set.");
	Assert(m_descriptors[(int)SHADER_IN_DESC::DEPTH_BUFFER].ptr > 0, "DSV hasn't been set.");

	directCmdList.OMSetRenderTargets((int)SHADER_OUT::COUNT - 1, &m_descriptors[SHADER_IN_DESC::RTV],
		true, &m_descriptors[(int)SHADER_IN_DESC::DEPTH_BUFFER]);
	directCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	directCmdList.SetPipelineState(m_pso);
	directCmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	D3D12_VERTEX_BUFFER_VIEW vbv;
	vbv.StrideInBytes = sizeof(Vertex);
	
	D3D12_INDEX_BUFFER_VIEW ibv;
	ibv.Format = DXGI_FORMAT_R32_UINT;
	int i = 0;

	const Core::DefaultHeapBuffer& sceneVB = App::GetScene().GetMeshVB();
	Assert(sceneVB.IsInitialized(), "VB hasn't been built yet.");
	const auto vbGpuVa = sceneVB.GetGpuVA();

	const Core::DefaultHeapBuffer& sceneIB = App::GetScene().GetMeshIB();
	Assert(sceneIB.IsInitialized(), "IB hasn't been built yet.");
	const auto ibGpuVa = sceneIB.GetGpuVA();

	for (auto& instance : m_perDrawCallArgs)
	{
		char buff[32];
		stbsp_snprintf(buff, sizeof(buff), "Mesh_%llu", instance.InstanceID);
		directCmdList.PIXBeginEvent(buff);

		vbv.BufferLocation = vbGpuVa + instance.VBStartOffsetInBytes;
		vbv.SizeInBytes = instance.VertexCount * sizeof(Vertex);

		ibv.BufferLocation = ibGpuVa + instance.IBStartOffsetInBytes;
		ibv.SizeInBytes = instance.IndexCount * sizeof(uint32_t);

		directCmdList.IASetVertexAndIndexBuffers(vbv, ibv);

		m_rootSig.SetRootCBV(0, m_perDrawCB.GetGpuVA() + i++ * instanceSize);
		m_rootSig.End(directCmdList);

		directCmdList.DrawIndexedInstanced(
			instance.IndexCount,
			1,
			0,
			0,
			0);

		directCmdList.PIXEndEvent();
	}

	directCmdList.PIXEndEvent();
}