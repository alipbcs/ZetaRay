#include "FinalPass.h"
#include "../../Core/Renderer.h"
#include "../../Core/CommandList.h"
#include "../../Scene/SceneRenderer/SceneRenderer.h"
#include "../../Model/Mesh.h"
#include "../../Win32/App.h"

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
		0,																// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_PIXEL,									// visibility
		SceneRenderer::FRAME_CONSTANTS_BUFFER_NAME);

	// root-constants
	m_rootSig.InitAsConstants(1,				// root idx
		sizeof(cbFinalPass) / sizeof(DWORD),	// num DWORDs
		1,										// register
		0,										// register-space
		D3D12_SHADER_VISIBILITY_PIXEL);

	// avg-lum
	m_rootSig.InitAsBufferSRV(2,				// root idx
		0,										// register
		0,										// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		D3D12_SHADER_VISIBILITY_PIXEL);
}

FinalPass::~FinalPass() noexcept
{
	if (m_pso)
		s_rpObjs.Clear();
}

void FinalPass::Init(D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc) noexcept
{
	auto& renderer = App::GetRenderer();

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	auto* samplers = App::GetRenderer().GetStaticSamplers();
	s_rpObjs.Init("Final", m_rootSig, RendererConstants::NUM_STATIC_SAMPLERS, samplers, flags);

	// use an arbitrary number as "nameID" since there's only one shader
	m_pso = s_rpObjs.m_psoLib.GetGraphicsPSO(0, psoDesc, s_rpObjs.m_rootSig.Get(), COMPILED_VS[0], COMPILED_PS[0]);

	//m_cbLocal.KeyValue = DefaultParamVals::KeyValue;
	m_cbLocal.DisplayBaseColor = false;
	m_cbLocal.DisplayDepth = false;
	m_cbLocal.DisplayMetalnessRoughness = false;
	m_cbLocal.DisplayNormals = false;
	m_cbLocal.DisplayMotionVec = false;
	m_cbLocal.DisplayIndirectDiffuse = false;
	m_cbLocal.DisplayStadTemporalCache = false;
	m_cbLocal.DoTonemapping = true;
	m_cbLocal.VisualizeOcclusion = false;

	ParamVariant p1;
	p1.InitEnum("Renderer", "Final", "Display", fastdelegate::MakeDelegate(this, &FinalPass::ChangeRenderOptionCallback),
		DefaultParamVals::RenderOptions, sizeof(DefaultParamVals::RenderOptions) / sizeof(const char*), 0);
	App::AddParam(p1);

	/*
	ParamVariant p0;
	p0.InitFloat("Renderer", "Final", "KeyValue", fastdelegate::MakeDelegate(this, &FinalPass::KeyValueCallback),
		DefaultParamVals::KeyValue,		// val	
		0.0f,							// min
		0.5f,							// max
		0.01f);							// step
	App::AddParam(p0);
	*/

	ParamVariant p5;
	p5.InitBool("Renderer", "Settings", "Tonemapping", fastdelegate::MakeDelegate(this, &FinalPass::DoTonemappingCallback),
		m_cbLocal.DoTonemapping);
	App::AddParam(p5);

	ParamVariant p6;
	p6.InitBool("Renderer", "Settings", "VisualizeOcclusion", fastdelegate::MakeDelegate(this, &FinalPass::VisualizeOcclusionCallback),
		false);
	App::AddParam(p6);

	m_cachedPsoDesc = psoDesc;
	App::AddShaderReloadHandler("Final", fastdelegate::MakeDelegate(this, &FinalPass::ReloadShaders));
}

void FinalPass::Reset() noexcept
{
	if (IsInitialized())
		s_rpObjs.Clear();

	App::RemoveParam("Renderer", "Final", "Display");
	//App::RemoveParam("Renderer", "Final", "KeyValue");
	App::RemoveParam("Renderer", "Settings", "Tonemapping");

	App::RemoveShaderReloadHandler("Final");
}

void FinalPass::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
	GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

	directCmdList.PIXBeginEvent("Final");

	directCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	directCmdList.SetPipelineState(m_pso);

	Assert(m_buffers[(int)SHADER_IN_BUFFER_DESC::AVG_LUM] != 0, "Buffer hasn't been set/");
	m_rootSig.SetRootSRV(2, m_buffers[(int)SHADER_IN_BUFFER_DESC::AVG_LUM]);

	Assert(m_gpuDescs[(int)SHADER_IN_GPU_DESC::FINAL_LIGHTING] > 0, "Gpu Desc Idx hasn't been set.");
	m_cbLocal.InputDescHeapIdx = m_gpuDescs[(int)SHADER_IN_GPU_DESC::FINAL_LIGHTING];
	m_cbLocal.IndirectDiffuseLiDescHeapIdx = m_gpuDescs[(int)SHADER_IN_GPU_DESC::INDIRECT_DIFFUSE_LI];
	m_cbLocal.DenoiserTemporalCacheDescHeapIdx = m_gpuDescs[(int)SHADER_IN_GPU_DESC::DENOISER_TEMPORAL_CACHE];
	m_rootSig.SetRootConstants(0, sizeof(cbFinalPass) / sizeof(DWORD), &m_cbLocal);
	m_rootSig.End(directCmdList);

	D3D12_VIEWPORT viewports[1] = { App::GetRenderer().GetDisplayViewport() };
	D3D12_RECT scissors[1] = { App::GetRenderer().GetDisplayScissor() };
	directCmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	directCmdList.RSSetViewportsScissorsRects(1, viewports, scissors);
	Assert(m_cpuDescs[(int)SHADER_IN_CPU_DESC::RTV].ptr > 0, "RTV hasn't been set.");
	directCmdList.OMSetRenderTargets(1, &m_cpuDescs[(int)SHADER_IN_CPU_DESC::RTV], true, nullptr);
	directCmdList.DrawInstanced(3, 1, 0, 0);

	directCmdList.PIXEndEvent();
}

void FinalPass::DoTonemappingCallback(const ParamVariant& p) noexcept
{
	m_cbLocal.DoTonemapping = p.GetBool();
}

void FinalPass::VisualizeOcclusionCallback(const ParamVariant& p) noexcept
{
	m_cbLocal.VisualizeOcclusion = p.GetBool();
}

void FinalPass::ChangeRenderOptionCallback(const ParamVariant& p) noexcept
{
	int curr = p.GetEnum().m_curr;

	m_cbLocal.DisplayBaseColor = false;
	m_cbLocal.DisplayDepth = false;
	m_cbLocal.DisplayMetalnessRoughness = false;
	m_cbLocal.DisplayNormals = false;
	m_cbLocal.DisplayMotionVec = false;
	m_cbLocal.DisplayIndirectDiffuse = false;
	m_cbLocal.DisplayStadTemporalCache = false;

	if (curr == DefaultParamVals::BASE_COLOR)
		m_cbLocal.DisplayBaseColor = true;
	else if (curr == DefaultParamVals::NORMALS)
		m_cbLocal.DisplayNormals = true;
	else if (curr == DefaultParamVals::METALNESS_ROUGHNESS)
		m_cbLocal.DisplayMetalnessRoughness = true;
	else if (curr == DefaultParamVals::DEPTH)
		m_cbLocal.DisplayDepth = true;
	else if (curr == DefaultParamVals::MOTION_VECTOR)
		m_cbLocal.DisplayMotionVec = true;
	else if (curr == DefaultParamVals::INDIRECT_DIFFUSE)
		m_cbLocal.DisplayIndirectDiffuse = true;
	else if (curr == DefaultParamVals::STAD_TEMPORAL_CACHE)
		m_cbLocal.DisplayStadTemporalCache = true;
}

//void FinalPass::KeyValueCallback(const ParamVariant& p) noexcept
//{
//	m_cbLocal.KeyValue = p.GetFloat().m_val;
//}

void FinalPass::ReloadShaders() noexcept
{
	s_rpObjs.m_psoLib.Reload(0, "Final\\FinalPass.hlsl", false);
	m_pso = s_rpObjs.m_psoLib.GetGraphicsPSO(0,
		m_cachedPsoDesc,
		s_rpObjs.m_rootSig.Get(),
		COMPILED_VS[0],
		COMPILED_PS[0]);
}