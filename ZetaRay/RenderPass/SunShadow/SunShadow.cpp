#include "SunShadow.h"
#include "../../Core/Renderer.h"
#include "../../Core/CommandList.h"
#include "../../Scene/SceneRenderer/SceneRenderer.h"
#include "../../RayTracing/Sampler.h"

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::RT;

//--------------------------------------------------------------------------------------
// SunShadow
//--------------------------------------------------------------------------------------

SunShadow::SunShadow() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// frame constants
	m_rootSig.InitAsCBV(0,												// root idx
		0,																// register
		0,																// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_ALL,									// visibility
		SceneRenderer::FRAME_CONSTANTS_BUFFER_NAME);

	// root constants
	m_rootSig.InitAsConstants(1,		// root idx
		NUM_CONSTS,						// num DWORDs
		1,								// register
		0);								// register space

	// BVH
	m_rootSig.InitAsBufferSRV(2,						// root idx
		0,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		SceneRenderer::RT_SCENE_BVH);

	// Owen-Scrambled Sobol Sequence
	m_rootSig.InitAsBufferSRV(3,						// root idx
		1,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SOBOL_SEQ);

	// scrambling tile
	m_rootSig.InitAsBufferSRV(4,						// root idx
		2,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE);

	// ranking tile
	m_rootSig.InitAsBufferSRV(5,						// root idx
		3,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE);
}

SunShadow::~SunShadow() noexcept
{
	Reset();
}

void SunShadow::Init() noexcept
{
	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;;

	auto samplers = App::GetRenderer().GetStaticSamplers();
	s_rpObjs.Init("SunShadow", m_rootSig, samplers.size(), samplers.data(), flags);

	m_pso = s_rpObjs.m_psoLib.GetComputePSO(0, s_rpObjs.m_rootSig.Get(), COMPILED_CS[0]);

	m_descTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((int)DESC_TABLE::COUNT);
	CreateResources();

	App::AddShaderReloadHandler("SunShadow", fastdelegate::MakeDelegate(this, &SunShadow::ReloadShader));
}

void SunShadow::Reset() noexcept
{
	if (IsInitialized())
	{
		App::RemoveShaderReloadHandler("SunShadow");
		s_rpObjs.Clear();

		m_shadowMask.Reset();
		m_descTable.Reset();
	}
}

void SunShadow::OnWindowResized() noexcept
{
	CreateResources();
}

void SunShadow::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	computeCmdList.PIXBeginEvent("SunShadow");

	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	computeCmdList.SetPipelineState(m_pso);

	cbSunShadow localCB;
	localCB.OutShadowMaskDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::SHADOW_MASK_UAV);
	m_rootSig.SetRootConstants(0, NUM_CONSTS, &localCB);

	m_rootSig.End(computeCmdList);

	const int w = App::GetRenderer().GetRenderWidth();
	const int h = App::GetRenderer().GetRenderHeight();

	const int numGroupsX = (uint32_t)CeilUnsignedIntDiv(w, SUN_SHADOW_THREAD_GROUP_SIZE_X);
	const int numGroupsY = (uint32_t)CeilUnsignedIntDiv(h, SUN_SHADOW_THREAD_GROUP_SIZE_Y);

	computeCmdList.Dispatch(numGroupsX, numGroupsY, 1);

	computeCmdList.PIXEndEvent();
}

void SunShadow::CreateResources() noexcept
{
	auto& renderer = App::GetRenderer();

	const int w = App::GetRenderer().GetRenderWidth();
	const int h = App::GetRenderer().GetRenderHeight();

	const int texWidth = (uint32_t)CeilUnsignedIntDiv(w, SUN_SHADOW_THREAD_GROUP_SIZE_X);
	const int texHeight = (uint32_t)CeilUnsignedIntDiv(h, SUN_SHADOW_THREAD_GROUP_SIZE_Y);

	m_shadowMask = renderer.GetGpuMemory().GetTexture2D("SunShadowMask",
		texWidth, texHeight,
		DXGI_FORMAT_R32_UINT,
		D3D12_RESOURCE_STATE_COMMON,
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	Direct3DHelper::CreateTexture2DUAV(m_shadowMask, m_descTable.CPUHandle((uint32_t)DESC_TABLE::SHADOW_MASK_UAV));
}

void SunShadow::ReloadShader() noexcept
{
	s_rpObjs.m_psoLib.Reload(0, "SunShadow\\SunShadow.hlsl", true);
	m_pso = s_rpObjs.m_psoLib.GetComputePSO(0, s_rpObjs.m_rootSig.Get(), COMPILED_CS[0]);
}

