#include "IndirectDiffuse.h"
#include "../../Core/Renderer.h"
#include "../../Core/CommandList.h"
#include "../../Scene/SceneRenderer/SceneRenderer.h"
#include "../../RayTracing/Sampler.h"
#include "../../Win32/App.h"

using namespace ZetaRay;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;

//--------------------------------------------------------------------------------------
// IndirectDiffuse
//--------------------------------------------------------------------------------------

IndirectDiffuse::IndirectDiffuse() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// root constants
	m_rootSig.InitAsConstants(0,						// root idx
		sizeof(cbIndirectDiffuse) / sizeof(DWORD),		// num-DWORDs
		1,												// register
		0);												// register-space

	// frame constants
	m_rootSig.InitAsCBV(1,												// root idx
		0,																// register
		0,																// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_ALL,									// visibility
		SceneRenderer::FRAME_CONSTANTS_BUFFER_NAME);

	// BVH
	m_rootSig.InitAsBufferSRV(2,						// root idx
		0,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		SceneRenderer::RT_SCENE_BVH);

	// material buffer
	m_rootSig.InitAsBufferSRV(3,						// root idx
		1,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		SceneRenderer::MATERIAL_BUFFER);

	// Owen-Scrambled Sobol Sequence
	m_rootSig.InitAsBufferSRV(4,						// root idx
		3,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SOBOL_SEQ);

	// scrambling-tile
	m_rootSig.InitAsBufferSRV(5,						// root idx
		4,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE);

	// ranking-tile
	m_rootSig.InitAsBufferSRV(6,						// root idx
		5,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE);

	// mesh-instance
	m_rootSig.InitAsBufferSRV(7,						// root idx
		6,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		SceneRenderer::FRAME_MESH_INSTANCE_DATA);
}

IndirectDiffuse::~IndirectDiffuse() noexcept
{
	Reset();
}

void IndirectDiffuse::Init() noexcept
{
	auto* samplers = App::GetRenderer().GetStaticSamplers();

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	s_rpObjs.Init("IndirectDiffuse", m_rootSig, RendererConstants::NUM_STATIC_SAMPLERS, samplers, flags);

	// use an arbitrary number as "nameID" since there's only one shader
	m_pso = s_rpObjs.m_psoLib.GetComputePSO(0,
		s_rpObjs.m_rootSig.Get(),
		COMPILED_CS[0]);

	m_outUAV = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((int)DESC_TABLE::COUNT);
	CreateOutput();

	App::AddShaderReloadHandler("IndirectDiffuse", fastdelegate::MakeDelegate(this, &IndirectDiffuse::ReloadShaders));
}

void IndirectDiffuse::Reset() noexcept
{
	if (IsInitialized())
	{
		App::RemoveShaderReloadHandler("IndirectDiffuse");
		s_rpObjs.Clear();
	}

	m_outLi.Reset();
	m_outWo.Reset();
	m_outUAV.Reset();
}

void IndirectDiffuse::OnWindowResized() noexcept
{
	CreateOutput();
}

void IndirectDiffuse::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	computeCmdList.PIXBeginEvent("IndirectDiffuse");

	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	computeCmdList.SetPipelineState(m_pso);

	const int w = App::GetRenderer().GetRenderWidth();
	const int h = App::GetRenderer().GetRenderHeight();

	const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, RT_IND_DIFF_THREAD_GROUP_SIZE_X);
	const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, RT_IND_DIFF_THREAD_GROUP_SIZE_Y);

	cbIndirectDiffuse cb;
	cb.InputWidth = w;
	cb.InputHeight = h;
	cb.OutputLoDescHeapIdx = m_outUAV.GPUDesciptorHeapIndex((int)DESC_TABLE::INDIRECT_LI_UAV);
	cb.DispatchDimX = dispatchDimX;
	cb.DispatchDimY = dispatchDimY;
	cb.TileWidth = 8;
	cb.Log2TileWidth = 3;
	cb.NumGroupsInTile = cb.TileWidth * cb.DispatchDimY;

	Assert((1 << cb.Log2TileWidth) == cb.TileWidth, "these must be equal");

	m_rootSig.SetRootConstants(0, sizeof(cbIndirectDiffuse) / sizeof(DWORD), &cb);
	m_rootSig.End(computeCmdList);

	computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

	cmdList.PIXEndEvent();
}

void IndirectDiffuse::CreateOutput() noexcept
{
	auto& renderer = App::GetRenderer();

	m_outLi = renderer.GetGpuMemory().GetTexture2D("IndirectDiffuse_Li",
		renderer.GetRenderWidth(), renderer.GetRenderHeight(),
		INDIRECT_LI_TEX_FORMAT,
		D3D12_RESOURCE_STATE_COMMON,
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Format = INDIRECT_LI_TEX_FORMAT;
	uavDesc.Texture2D.MipSlice = 0;
	uavDesc.Texture2D.PlaneSlice = 0;

	renderer.GetDevice()->CreateUnorderedAccessView(m_outLi.GetResource(), nullptr, &uavDesc, m_outUAV.CPUHandle((int)DESC_TABLE::INDIRECT_LI_UAV));
}

void IndirectDiffuse::ReloadShaders() noexcept
{
	s_rpObjs.m_psoLib.Reload(0, "IndirectDiffuse\\IndirectDiffuse.hlsl", true);
	m_pso = s_rpObjs.m_psoLib.GetComputePSO(0, s_rpObjs.m_rootSig.Get(), COMPILED_CS[0]);
}

