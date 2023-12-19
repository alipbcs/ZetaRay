#include "SkyDome.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneCore.h>
#include <Model/Mesh.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Util;
using namespace ZetaRay::Model;

//--------------------------------------------------------------------------------------
// SkyDome
//--------------------------------------------------------------------------------------

SkyDome::SkyDome()
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// frame constants
	m_rootSig.InitAsCBV(0,												// root idx
		0,																// register
		0,																// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_ALL,									// visibility
		GlobalResource::FRAME_CONSTANTS_BUFFER);
}

SkyDome::~SkyDome()
{
	Reset();
}

void SkyDome::Init(DXGI_FORMAT rtvFormat)
{
	auto& renderer = App::GetRenderer();
	m_cachedRtvFormat = rtvFormat;

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	auto samplers = renderer.GetStaticSamplers();
	s_rpObjs.Init("SkyDome", m_rootSig, samplers.size(), samplers.data(), flags);

	CreatePSO();

	// create the sphere mesh
	SmallVector<Vertex> vertices;
	SmallVector<uint32_t> indices;
//	float worldRadius = App::GetScene().GetWorldAABB().Extents.length();
	float worldRadius = 6360.0f;

	PrimitiveMesh::ComputeSphere(vertices, indices, worldRadius * 2.0f, 8);

	uint32_t sizeInBytes = sizeof(Vertex) * (uint32_t)vertices.size();
	m_domeVertexBuffer = GpuMemory::GetDefaultHeapBufferAndInit("DomeVertexBuffer",
		sizeInBytes,
		false, 
		vertices.begin());

	sizeInBytes = sizeof(uint32_t) * (uint32_t)indices.size();
	m_domeIndexBuffer = GpuMemory::GetDefaultHeapBufferAndInit("DomeIndexBuffer",
		sizeInBytes,
		false, 
		indices.begin());

	m_vbv.BufferLocation = m_domeVertexBuffer.GpuVA();
	m_vbv.SizeInBytes = (UINT)m_domeVertexBuffer.Desc().Width;
	m_vbv.StrideInBytes = sizeof(Vertex);

	m_ibv.BufferLocation = m_domeIndexBuffer.GpuVA();
	m_ibv.Format = DXGI_FORMAT_R32_UINT;
	m_ibv.SizeInBytes = (UINT)m_domeIndexBuffer.Desc().Width;

	App::AddShaderReloadHandler("SkyDome", fastdelegate::MakeDelegate(this, &SkyDome::ReloadShaders));
}

void SkyDome::Reset()
{
	if (IsInitialized())
	{
		//App::RemoveShaderReloadHandler("SkyDome");
		s_rpObjs.Clear();
	}

	m_domeIndexBuffer.Reset();
	m_domeVertexBuffer.Reset();
	//m_pso = nullptr;
}

void SkyDome::Render(CommandList& cmdList)
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
	GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();

	directCmdList.PIXBeginEvent("SkyDome");

	// record the timestamp prior to execution
	const uint32_t queryIdx = gpuTimer.BeginQuery(directCmdList, "SkyDome");

	Assert(m_descriptors[SHADER_IN_DESC::RTV].ptr > 0, "RTV hasn't been set.");
	Assert(m_descriptors[SHADER_IN_DESC::DEPTH_BUFFER].ptr > 0, "DSV hasn't been set.");

	directCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	directCmdList.SetPipelineState(m_pso);

	m_rootSig.End(directCmdList);

	D3D12_VIEWPORT viewports[1] = { renderer.GetRenderViewport() };
	D3D12_RECT scissors[1] = { renderer.GetRenderScissor() };

	directCmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	directCmdList.IASetVertexAndIndexBuffers(m_vbv, m_ibv);
	directCmdList.RSSetViewportsScissorsRects(1, viewports, scissors);
	directCmdList.OMSetRenderTargets(1, &m_descriptors[SHADER_IN_DESC::RTV], true, &m_descriptors[SHADER_IN_DESC::DEPTH_BUFFER]);

	directCmdList.DrawIndexedInstanced(m_ibv.SizeInBytes / sizeof(uint32_t), 1, 0, 0, 0);

	// record the timestamp after execution
	gpuTimer.EndQuery(directCmdList, queryIdx);

	directCmdList.PIXEndEvent();
}

void SkyDome::CreatePSO()
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

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DUtil::GetPSODesc(&inputLayout,
		1,
		&m_cachedRtvFormat,
		Constants::DEPTH_BUFFER_FORMAT);

	psoDesc.DepthStencilState.DepthEnable = true;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;		// we're inside the sphere
	//psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

	// use an arbitrary number as "nameID" since there's only one shader
	m_pso = s_rpObjs.m_psoLib.GetGraphicsPSO(0, psoDesc, s_rpObjs.m_rootSig.Get(), COMPILED_VS[0], COMPILED_PS[0]);
}

void SkyDome::ReloadShaders()
{
	s_rpObjs.m_psoLib.Reload(0, "Sky\\SkyDome.hlsl", false);
	CreatePSO();
}

