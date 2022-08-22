#include "Compositing.h"
#include "../../Core/Renderer.h"
#include "../../Core/CommandList.h"
#include "../../Scene/SceneRenderer/SceneRenderer.h"
#include "../../Win32/App.h"
#include "../../Win32/Timer.h"

using namespace ZetaRay;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;

//--------------------------------------------------------------------------------------
// Compositing
//--------------------------------------------------------------------------------------

Compositing::Compositing() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// root-constants
	m_rootSig.InitAsConstants(0,				// root idx
		sizeof(cbCompositing) / sizeof(DWORD),	// num DWORDs
		0,										// register
		0);										// register-space

	// frame constants
	m_rootSig.InitAsCBV(1,												// root idx
		1,																// register
		0,																// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_ALL,									// visibility
		SceneRenderer::FRAME_CONSTANTS_BUFFER_NAME);
}

Compositing::~Compositing() noexcept
{
	Reset();
}

void Compositing::Init() noexcept
{
	auto& renderer = App::GetRenderer();
	auto* samplers = renderer.GetStaticSamplers();

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	s_rpObjs.Init("Compositing", m_rootSig, RendererConstants::NUM_STATIC_SAMPLERS, samplers, flags);

	// use an arbitrary number as "nameID" since there's only one shader
	m_pso = s_rpObjs.m_psoLib.GetComputePSO(0, s_rpObjs.m_rootSig.Get(), COMPILED_CS[0]);

	m_localCB.UseDenoised = false;
	m_localCB.AccumulateInscattering = false;

	App::AddShaderReloadHandler("Compositing", fastdelegate::MakeDelegate(this, &Compositing::ReloadShader));
}

void Compositing::Reset() noexcept
{
	if (IsInitialized())
	{
		App::RemoveShaderReloadHandler("Compositing");
		s_rpObjs.Clear();
	}

	m_pso = nullptr;

#ifdef _DEBUG
	memset(&m_localCB, 0, sizeof(m_localCB));
#endif // _DEBUG
}

void Compositing::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	computeCmdList.PIXBeginEvent("Compositing");

	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	computeCmdList.SetPipelineState(m_pso);

	Assert(m_localCB.HDRLightAccumDescHeapIdx > 0, "Gpu descriptor for HDR light accum texture hasn't been set");

	//m_localCB.AccumulateInscattering = App::GetTimer().GetTotalFrameCount() > 1;

	if (m_localCB.AccumulateInscattering)
	{
		Assert(m_localCB.InscatteringDescHeapIdx > 0, "Gpu descriptor for inscattering texture hasn't been set");
		Assert(m_localCB.VoxelGridNearZ >= 0.0f, "Invalid voxel grid depth");
		Assert(m_localCB.VoxelGridFarZ > m_localCB.VoxelGridNearZ, "Invalid voxel grid depth");
		Assert(m_localCB.DepthMappingExp > 0.0f, "Invalid voxel grid depth mapping exponent");
	}

	m_rootSig.SetRootConstants(0, sizeof(cbCompositing) / sizeof(DWORD), &m_localCB);
	m_rootSig.End(computeCmdList);

	const int w = App::GetRenderer().GetRenderWidth();
	const int h = App::GetRenderer().GetRenderHeight();

	const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, THREAD_GROUP_SIZE_X);
	const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, THREAD_GROUP_SIZE_Y);

	computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

	computeCmdList.PIXEndEvent();
}

void Compositing::ReloadShader() noexcept
{
	s_rpObjs.m_psoLib.Reload(0, "Compositing\\Compositing.hlsl", true);
	m_pso = s_rpObjs.m_psoLib.GetComputePSO(0, s_rpObjs.m_rootSig.Get(), COMPILED_CS[0]);
}

