#include "LinearDepthGradient.h"
#include "../../Core/Renderer.h"
#include "../../Core/CommandList.h"
#include "../../Scene/SceneRenderer/SceneRenderer.h"
#include "../../Win32/App.h"

using namespace ZetaRay;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Core;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;

//--------------------------------------------------------------------------------------
// IndirectDiffuse
//--------------------------------------------------------------------------------------

LinearDepthGradient::LinearDepthGradient() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
}

LinearDepthGradient::~LinearDepthGradient() noexcept
{
	if (m_pso)
		s_rpObjs.Clear();
}

void LinearDepthGradient::Init() noexcept
{
	// frame constants
	m_rootSig.InitAsCBV(0,												// root idx
		0,																// register
		0,																// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_ALL,									// visibility
		SceneRenderer::FRAME_CONSTANTS_BUFFER_NAME);

	// root constants
	m_rootSig.InitAsConstants(1,						// root idx
		sizeof(cbLinearDepthGrad) / sizeof(DWORD),		// num-DWORDs
		1,												// register
		0);												// register-space

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	s_rpObjs.Init("LinearDepthGradient", m_rootSig, 0, nullptr, flags);

	// use an arbitrary number as "nameID" since there's only one shader
	m_pso = s_rpObjs.m_psoLib.GetComputePSO(0,
		s_rpObjs.m_rootSig.Get(),
		COMPILED_CS[0]);

	m_outUAV = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(1);
	CreateOutput();

	App::AddShaderReloadHandler("LinearDepthGrad", fastdelegate::MakeDelegate(this, &LinearDepthGradient::ReloadShader));
}

void LinearDepthGradient::Reset() noexcept
{
	if (IsInitialized())
	{
		App::RemoveShaderReloadHandler("LinearDepthGrad");
		s_rpObjs.Clear();
	}

	m_out.Reset();
	m_outUAV.Reset();
	m_pso = nullptr;
}

void LinearDepthGradient::OnWindowResized() noexcept
{
	CreateOutput();
}

void LinearDepthGradient::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	computeCmdList.PIXBeginEvent("LinearDepthGradient");

	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	computeCmdList.SetPipelineState(m_pso);

	const int w = App::GetRenderer().GetRenderWidth();
	const int h = App::GetRenderer().GetRenderHeight();

	const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, LINEAR_DEPTH_GRAD_THREAD_GROUP_SIZE_X);
	const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, LINEAR_DEPTH_GRAD_THREAD_GROUP_SIZE_Y);

	cbLinearDepthGrad cb;
	cb.OutputDescHeapIdx = m_outUAV.GPUDesciptorHeapIndex(0);
	m_rootSig.SetRootConstants(0, sizeof(cbLinearDepthGrad) / sizeof(DWORD), &cb);
	m_rootSig.End(computeCmdList);

	computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

	cmdList.PIXEndEvent();
}

void LinearDepthGradient::CreateOutput() noexcept
{
	auto& renderer = App::GetRenderer();

	m_out = renderer.GetGpuMemory().GetTexture2D("LinearDepthGradient_out",
		renderer.GetRenderWidth(), renderer.GetRenderHeight(),
		DXGI_FORMAT_R32G32_FLOAT,
		D3D12_RESOURCE_STATE_COMMON,
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
	uavDesc.Texture2D.MipSlice = 0;
	uavDesc.Texture2D.PlaneSlice = 0;

	renderer.GetDevice()->CreateUnorderedAccessView(m_out.GetResource(), nullptr, &uavDesc, m_outUAV.CPUHandle(0));
}

void LinearDepthGradient::ReloadShader() noexcept
{
	s_rpObjs.m_psoLib.Reload(0, "SVGF\\LinearDepthGradient.hlsl", true);
	m_pso = s_rpObjs.m_psoLib.GetComputePSO(0, s_rpObjs.m_rootSig.Get(), COMPILED_CS[0]);
}

