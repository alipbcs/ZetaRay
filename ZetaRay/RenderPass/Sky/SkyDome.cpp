#include "SkyDome.h"
#include "../../Core/Renderer.h"
#include "../../Core/CommandList.h"
#include "../../Scene/SceneRenderer/SceneRenderer.h"
#include "../../Scene/SceneCore.h"
#include "../../Model/Mesh.h"
#include "../../Win32/App.h"

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Util;
using namespace ZetaRay::Model;

//--------------------------------------------------------------------------------------
// SkyDome
//--------------------------------------------------------------------------------------

SkyDome::SkyDome() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// frame constants
	m_rootSig.InitAsCBV(0,												// root idx
		0,																// register
		0,																// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_ALL,									// visibility
		SceneRenderer::FRAME_CONSTANTS_BUFFER_NAME);
}

SkyDome::~SkyDome() noexcept
{
	Reset();
}

void SkyDome::Init(D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc) noexcept
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

	auto* samplers = renderer.GetStaticSamplers();
	s_rpObjs.Init("SkyDome", m_rootSig, RendererConstants::NUM_STATIC_SAMPLERS, samplers, flags);

	// use an arbitrary number as "nameID" since there's only one shader
	m_pso = s_rpObjs.m_psoLib.GetGraphicsPSO(0, psoDesc, s_rpObjs.m_rootSig.Get(), COMPILED_VS[0], COMPILED_PS[0]);

	// create the sphere mesh
	SmallVector<VertexPosNormalTexTangent> vertices;
	SmallVector<INDEX_TYPE> indices;
//	float worldRadius = App::GetScene().GetWorldAABB().Extents.length();
	float worldRadius = 6360000.0f;

	PrimitiveMesh::ComputeSphere(vertices, indices, worldRadius * 2.0f, 8);

	size_t sizeInBytes = sizeof(VertexPosNormalTexTangent) * vertices.size();
	m_domeVertexBuffer = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit("DomeVertexBuffer", 
		sizeInBytes,
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, 
		false, 
		vertices.begin());

	sizeInBytes = sizeof(INDEX_TYPE) * indices.size();
	m_domeIndexBuffer = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit("DomeIndexBuffer", 
		sizeInBytes,
		D3D12_RESOURCE_STATE_INDEX_BUFFER, 
		false, 
		indices.begin());

	m_vbv.BufferLocation = m_domeVertexBuffer.GetGpuVA();
	m_vbv.SizeInBytes = (UINT)m_domeVertexBuffer.GetDesc().Width;
	m_vbv.StrideInBytes = sizeof(VertexPosNormalTexTangent);

	m_ibv.BufferLocation = m_domeIndexBuffer.GetGpuVA();
	m_ibv.Format = MESH_INDEX_FORMAT;
	m_ibv.SizeInBytes = (UINT)m_domeIndexBuffer.GetDesc().Width;

	m_cachedPsoDesc = psoDesc;
	App::AddShaderReloadHandler("SkyDome", fastdelegate::MakeDelegate(this, &SkyDome::ReloadShaders));
}

void SkyDome::Reset() noexcept
{
	if (IsInitialized())
	{
		App::RemoveShaderReloadHandler("SkyDome");
		s_rpObjs.Clear();
	}

	m_domeIndexBuffer.Reset();
	m_domeVertexBuffer.Reset();
	m_pso = nullptr;
}

void SkyDome::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
	GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

	directCmdList.PIXBeginEvent("SkyDome");

	Assert(m_descriptors[SHADER_IN_DESC::RTV].ptr > 0, "RTV hasn't been set.");
	Assert(m_descriptors[SHADER_IN_DESC::DEPTH_BUFFER].ptr > 0, "DSV hasn't been set.");

	directCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	directCmdList.SetPipelineState(m_pso);

	m_rootSig.End(directCmdList);

	D3D12_VIEWPORT viewports[1] = { App::GetRenderer().GetRenderViewport() };
	D3D12_RECT scissors[1] = { App::GetRenderer().GetRenderScissor() };

	directCmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	directCmdList.IASetVertexAndIndexBuffers(m_vbv, m_ibv);
	directCmdList.RSSetViewportsScissorsRects(1, viewports, scissors);
	directCmdList.OMSetRenderTargets(1, &m_descriptors[SHADER_IN_DESC::RTV], true, &m_descriptors[SHADER_IN_DESC::DEPTH_BUFFER]);

	directCmdList.DrawIndexedInstanced(m_ibv.SizeInBytes / sizeof(INDEX_TYPE), 1, 0, 0, 0);

	directCmdList.PIXEndEvent();
}

void SkyDome::ReloadShaders() noexcept
{
	s_rpObjs.m_psoLib.Reload(0, "Final\\FinalPass_ps.hlsl", false);
	m_pso = s_rpObjs.m_psoLib.GetGraphicsPSO(0,
		m_cachedPsoDesc,
		s_rpObjs.m_rootSig.Get(),
		COMPILED_VS[0],
		COMPILED_PS[0]);
}

