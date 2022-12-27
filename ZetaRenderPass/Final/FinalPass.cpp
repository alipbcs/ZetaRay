#include "FinalPass.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>

using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Core;
using namespace ZetaRay::Support;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Math;

//--------------------------------------------------------------------------------------
// FinalPass
//--------------------------------------------------------------------------------------

FinalPass::FinalPass() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// frame constants
	m_rootSig.InitAsCBV(0,												// root idx
		0,																// register
		0,																// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_PIXEL,									// visibility
		GlobalResource::FRAME_CONSTANTS_BUFFER_NAME);

	// root constants
	m_rootSig.InitAsConstants(1,				// root idx
		sizeof(cbFinalPass) / sizeof(DWORD),	// num DWORDs
		1,										// register
		0,										// register space
		D3D12_SHADER_VISIBILITY_PIXEL);
}

FinalPass::~FinalPass() noexcept
{
	if (m_pso)
		s_rpObjs.Clear();
}

void FinalPass::Init() noexcept
{
	auto& renderer = App::GetRenderer();

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	auto samplers = App::GetRenderer().GetStaticSamplers();
	s_rpObjs.Init("Final", m_rootSig, samplers.size(), samplers.data(), flags);
	CreatePSO();

	m_cbLocal.DisplayOption = (int)DisplayOption::DEFAULT;
	m_cbLocal.Tonemapper = (int)Tonemapper::UE4_FILMIC;
	m_cbLocal.VisualizeOcclusion = false;

	ParamVariant p1;
	p1.InitEnum("Renderer", "Display", "FinalRender", fastdelegate::MakeDelegate(this, &FinalPass::ChangeDisplayOptionCallback),
		Params::DisplayOptions, ZetaArrayLen(Params::DisplayOptions), m_cbLocal.DisplayOption);
	App::AddParam(p1);

	ParamVariant p2;
	p2.InitEnum("Renderer", "Display", "Tonemapper", fastdelegate::MakeDelegate(this, &FinalPass::ChangeTonemapperCallback),
		Params::Tonemappers, ZetaArrayLen(Params::Tonemappers), m_cbLocal.Tonemapper);
	App::AddParam(p2);

	ParamVariant p6;
	p6.InitBool("Renderer", "Settings", "VisualizeOcclusion", fastdelegate::MakeDelegate(this, &FinalPass::VisualizeOcclusionCallback),
		false);
	App::AddParam(p6);

	App::AddShaderReloadHandler("Final", fastdelegate::MakeDelegate(this, &FinalPass::ReloadShaders));
}

void FinalPass::Reset() noexcept
{
	if (IsInitialized())
		s_rpObjs.Clear();

	//App::RemoveParam("Renderer", "Final", "Display");
	//App::RemoveParam("Renderer", "Final", "KeyValue");
	//App::RemoveParam("Renderer", "Settings", "Tonemapping");

	App::RemoveShaderReloadHandler("Final");
}

void FinalPass::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
	GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();

	directCmdList.PIXBeginEvent("Final");

	// record the timestamp prior to execution
	const uint32_t queryIdx = gpuTimer.BeginQuery(directCmdList, "Final");

	directCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	directCmdList.SetPipelineState(m_pso);

	Assert(m_gpuDescs[(int)SHADER_IN_GPU_DESC::FINAL_LIGHTING] > 0, "Gpu Desc Idx hasn't been set.");
	Assert(m_gpuDescs[(int)SHADER_IN_GPU_DESC::EXPOSURE] > 0, "Gpu Desc Idx hasn't been set.");
	m_cbLocal.InputDescHeapIdx = m_gpuDescs[(int)SHADER_IN_GPU_DESC::FINAL_LIGHTING];
	m_cbLocal.ExposureDescHeapIdx = m_gpuDescs[(int)SHADER_IN_GPU_DESC::EXPOSURE];
	m_cbLocal.DenoiserTemporalCacheDescHeapIdx = m_gpuDescs[(int)SHADER_IN_GPU_DESC::DENOISER_TEMPORAL_CACHE];
	m_cbLocal.TemporalReservoir_A_DescHeapIdx = m_gpuDescs[(int)SHADER_IN_GPU_DESC::ReSTIR_GI_TEMPORAL_RESERVOIR_A];
	m_cbLocal.TemporalReservoir_B_DescHeapIdx = m_gpuDescs[(int)SHADER_IN_GPU_DESC::ReSTIR_GI_TEMPORAL_RESERVOIR_B];
	m_cbLocal.TemporalReservoir_C_DescHeapIdx = m_gpuDescs[(int)SHADER_IN_GPU_DESC::ReSTIR_GI_TEMPORAL_RESERVOIR_C];
	m_cbLocal.SpatialReservoir_A_DescHeapIdx = m_gpuDescs[(int)SHADER_IN_GPU_DESC::ReSTIR_GI_SPATIAL_RESERVOIR_A];
	m_cbLocal.SpatialReservoir_B_DescHeapIdx = m_gpuDescs[(int)SHADER_IN_GPU_DESC::ReSTIR_GI_SPATIAL_RESERVOIR_B];
	m_cbLocal.SpatialReservoir_C_DescHeapIdx = m_gpuDescs[(int)SHADER_IN_GPU_DESC::ReSTIR_GI_SPATIAL_RESERVOIR_C];
	m_rootSig.SetRootConstants(0, sizeof(cbFinalPass) / sizeof(DWORD), &m_cbLocal);
	m_rootSig.End(directCmdList);

	D3D12_VIEWPORT viewports[1] = { renderer.GetDisplayViewport() };
	D3D12_RECT scissors[1] = { renderer.GetDisplayScissor() };
	directCmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	directCmdList.RSSetViewportsScissorsRects(1, viewports, scissors);
	Assert(m_cpuDescs[(int)SHADER_IN_CPU_DESC::RTV].ptr > 0, "RTV hasn't been set.");
	directCmdList.OMSetRenderTargets(1, &m_cpuDescs[(int)SHADER_IN_CPU_DESC::RTV], true, nullptr);
	directCmdList.DrawInstanced(3, 1, 0, 0);

	// record the timestamp after execution
	gpuTimer.EndQuery(directCmdList, queryIdx);

	directCmdList.PIXEndEvent();
}

void FinalPass::CreatePSO() noexcept
{
	DXGI_FORMAT rtvFormats[1] = { Constants::BACK_BUFFER_FORMAT };
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DHelper::GetPSODesc(nullptr,
		1,
		rtvFormats,
		Constants::DEPTH_BUFFER_FORMAT);

	// no blending required

	// disable depth testing and writing
	psoDesc.DepthStencilState.DepthEnable = false;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

	// disable triangle culling
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	// use an arbitrary number as "nameID" since there's only one shader
	m_pso = s_rpObjs.m_psoLib.GetGraphicsPSO(0, psoDesc, s_rpObjs.m_rootSig.Get(), COMPILED_VS[0], COMPILED_PS[0]);
}

void FinalPass::VisualizeOcclusionCallback(const ParamVariant& p) noexcept
{
	m_cbLocal.VisualizeOcclusion = p.GetBool();
}

void FinalPass::ChangeDisplayOptionCallback(const ParamVariant& p) noexcept
{
	m_cbLocal.DisplayOption = (uint16_t)p.GetEnum().m_curr;
}

void FinalPass::ChangeTonemapperCallback(const Support::ParamVariant& p) noexcept
{
	m_cbLocal.Tonemapper = (uint16_t)p.GetEnum().m_curr;
}

void FinalPass::ReloadShaders() noexcept
{
	s_rpObjs.m_psoLib.Reload(0, "Final\\FinalPass.hlsl", false);
	CreatePSO();
}