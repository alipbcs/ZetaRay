#include "LuminanceReduction.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DHelper;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;

//--------------------------------------------------------------------------------------
// LuminanceReduction
//--------------------------------------------------------------------------------------

LuminanceReduction::LuminanceReduction() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
}

LuminanceReduction::~LuminanceReduction() noexcept
{
	if (m_psos[0])
		s_rpObjs.Clear();

	Reset();
}

void LuminanceReduction::Init() noexcept
{
	// frame constants
	m_rootSig.InitAsCBV(0,						// root idx
		0,										// register num
		0,										// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, 
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::FRAME_CONSTANTS_BUFFER_NAME);

	// root-constants
	m_rootSig.InitAsConstants(1,				// root idx
		sizeof(cbReduction) / sizeof(DWORD),	// num DWORDs
		1,										// register
		0);										// register-space

	m_rootSig.InitAsBufferSRV(2,				// root idx
		0,										// register num
		0,										// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		D3D12_SHADER_VISIBILITY_ALL, 
		nullptr,
		true);
		
	m_rootSig.InitAsBufferUAV(3,				// root idx
		0,										// register num
		0,										// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE);

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	s_rpObjs.Init("LuminanceReduction", m_rootSig, 0, nullptr, flags);

	m_psos[0] = s_rpObjs.m_psoLib.GetComputePSO(0, s_rpObjs.m_rootSig.Get(), COMPILED_CS[0]);
	m_psos[1] = s_rpObjs.m_psoLib.GetComputePSO(1, s_rpObjs.m_rootSig.Get(), COMPILED_CS[1]);

	// create all the buffers
	CreateResources();
}

void LuminanceReduction::Reset() noexcept
{
	if (m_psos[0])
	{
		s_rpObjs.Clear();

		m_reducedLumIntermediate.Reset();
		m_reducedLum.Reset();
	}
}

void LuminanceReduction::OnWindowResized() noexcept
{
	auto& renderer = App::GetRenderer();
	const uint32_t numThreadGroups = renderer.GetRenderWidth() * renderer.GetRenderHeight() /
		(THREAD_GROUP_SIZE_X_FIRST * THREAD_GROUP_SIZE_Y_FIRST);

	m_reducedLumIntermediate = renderer.GetGpuMemory().GetDefaultHeapBuffer("LumReductionIntermediate",
		numThreadGroups * sizeof(float), D3D12_RESOURCE_STATE_COMMON, true, false);
}

void LuminanceReduction::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	Assert(m_inputDesc[(int)SHADER_IN_DESC::COMPOSITED] != -1, "Input descriptor hasn't been set.");

	auto& renderer = App::GetRenderer();
	const int w = renderer.GetRenderWidth();
	const int h = renderer.GetRenderHeight();
	const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, THREAD_GROUP_SIZE_X_FIRST);
	const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, THREAD_GROUP_SIZE_Y_FIRST);

	//
	// First Pass
	//
	{
		computeCmdList.PIXBeginEvent("LuminanceReduction_First");

		computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
		computeCmdList.SetPipelineState(m_psos[0]);

		//m_rootSig.SetRootSRV(2, m_reducedLumIntermediate.GetGpuVA());
		m_rootSig.SetRootUAV(3, m_reducedLumIntermediate.GetGpuVA());

		cbReduction cb;
		cb.InputDescHeapIdx = m_inputDesc[(int)SHADER_IN_DESC::COMPOSITED];
		cb.DispatchDimXFirstPass = dispatchDimX;
		cb.NumGroupsInFirstPass = dispatchDimX * dispatchDimY;
		cb.NumToProcessPerThreadSecondPass = (uint32_t)CeilUnsignedIntDiv(cb.NumGroupsInFirstPass, THREAD_GROUP_SIZE_X_SECOND);

		m_rootSig.SetRootConstants(0, sizeof(cbReduction) / sizeof(DWORD), &cb);
		m_rootSig.End(computeCmdList);

		D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(m_reducedLumIntermediate.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		computeCmdList.TransitionResource(&barrier, 1);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		computeCmdList.PIXEndEvent();
	}

	//
	// Second Pass
	//
	{
		computeCmdList.PIXBeginEvent("LuminanceReduction_Second");

		computeCmdList.SetPipelineState(m_psos[1]);

		D3D12_RESOURCE_BARRIER barriers[2];

		barriers[0] = UAVBarrier(m_reducedLumIntermediate.GetResource());
		barriers[1] = TransitionBarrier(m_reducedLumIntermediate.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		computeCmdList.TransitionResource(barriers, sizeof(barriers) / sizeof(D3D12_RESOURCE_BARRIER));

		m_rootSig.SetRootSRV(2, m_reducedLumIntermediate.GetGpuVA());
		m_rootSig.SetRootUAV(3, m_reducedLum.GetGpuVA());
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(1, 1, 1);

		computeCmdList.PIXEndEvent();
	}
}

void LuminanceReduction::CreateResources() noexcept
{
	auto& renderer = App::GetRenderer();
	const uint32_t numThreadGroups = renderer.GetRenderWidth() * renderer.GetRenderHeight() / 
		(THREAD_GROUP_SIZE_X_FIRST * THREAD_GROUP_SIZE_Y_FIRST);

	// D3D warning: Buffers are effectively created in state D3D12_RESOURCE_STATE_COMMON
	m_reducedLumIntermediate = renderer.GetGpuMemory().GetDefaultHeapBuffer("LumReductionIntermediate",
		numThreadGroups * sizeof(float), D3D12_RESOURCE_STATE_COMMON, true, false);
	m_reducedLum = renderer.GetGpuMemory().GetDefaultHeapBuffer("ReducedLum",
		sizeof(float), D3D12_RESOURCE_STATE_COMMON, true, false);
}
