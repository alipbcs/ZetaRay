#include "GaussianFilter.h"
#include "../../Core/Renderer.h"
#include "../../Core/CommandList.h"
#include "../../Scene/SceneRenderer/SceneRenderer.h"
#include "../../Win32/App.h"
#include "../../Win32/Timer.h"

using namespace ZetaRay;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;

//--------------------------------------------------------------------------------------
// GaussianFilter
//--------------------------------------------------------------------------------------

GaussianFilter::GaussianFilter() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
}

GaussianFilter::~GaussianFilter() noexcept
{
	if (m_pso)
		s_rpObjs.Clear();
}

void GaussianFilter::Init(const char* ownerPath, int inputWidth, int inputHeight, DXGI_FORMAT f) noexcept
{
	// root-constants
	m_rootSig.InitAsConstants(0,					// root idx
		sizeof(cbGaussianFilter) / sizeof(DWORD),	// num DWORDs
		0,											// register
		0);											// register-space

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	auto* samplers = App::GetRenderer().GetStaticSamplers();
	s_rpObjs.Init("GaussianFilter", m_rootSig, RendererConstants::NUM_STATIC_SAMPLERS, samplers, flags);

	// use an arbitrary number as "nameID" since there's only one shader
	m_pso = s_rpObjs.m_psoLib.GetComputePSO(0, s_rpObjs.m_rootSig.Get(), COMPILED_CS[0]);

	m_descTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(1);
	CreateOutput(inputWidth, inputHeight, f);
}

void GaussianFilter::Reset() noexcept
{
	if (IsInitialized())
		s_rpObjs.Clear();

	m_filtered.Reset();
	m_descTable.Reset();
	memset(m_inputDesc, 0, (int)SHADER_IN_DESC::COUNT * sizeof(uint32_t));
	m_pso = nullptr;
}

void GaussianFilter::OnResize(int inputWidth, int inputHeight, DXGI_FORMAT f) noexcept
{
	CreateOutput(inputWidth, inputHeight, f);
}

void GaussianFilter::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	const int outIdx = App::GetTimer().GetTotalFrameCount() & 0x1;

	Assert(m_inputDesc[(int)SHADER_IN_DESC::SIGNAL] != 0, "Input descriptor hasn't been set.");
	const int w = (int)m_filtered.GetResource()->GetDesc().Width;
	const int h = (int)m_filtered.GetResource()->GetDesc().Height;

	cbGaussianFilter cb;
	cb.InputDescHeapIdx = m_inputDesc[(int)SHADER_IN_DESC::SIGNAL];
	cb.OutputDescHeapIdx = m_descTable.GPUDesciptorHeapIndex();
	cb.InputWidth = (float)w;
	cb.InputHeight = (float)h;

	m_rootSig.SetRootConstants(0, sizeof(cbGaussianFilter) / sizeof(DWORD), &cb);

	computeCmdList.PIXBeginEvent("GaussianFilter");

	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	computeCmdList.SetPipelineState(m_pso);

	m_rootSig.End(computeCmdList);

	computeCmdList.Dispatch((uint32_t)CeilUnsignedIntDiv(w, GAUSSAIN_FILT_THREAD_GROUP_SIZE_X), 
		(uint32_t)CeilUnsignedIntDiv(h, GAUSSAIN_FILT_THREAD_GROUP_SIZE_Y), 1);

	computeCmdList.PIXEndEvent();
}

void GaussianFilter::CreateOutput(int inputWidth, int inputHeight, DXGI_FORMAT f) noexcept
{
	auto& renderer = App::GetRenderer();

	m_filtered = renderer.GetGpuMemory().GetTexture2D("GaussianFilter_out",
		inputWidth, inputHeight,
		Direct3DHelper::NoSRGB(f),
		D3D12_RESOURCE_STATE_COMMON,
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	// create the descriptors
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Format = Direct3DHelper::NoSRGB(f);
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.PlaneSlice = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Format = Direct3DHelper::NoSRGB(f);
	uavDesc.Texture2D.MipSlice = 0;
	uavDesc.Texture2D.PlaneSlice = 0;

	renderer.GetDevice()->CreateUnorderedAccessView(m_filtered.GetResource(), nullptr, &uavDesc, m_descTable.CPUHandle(0));
}
