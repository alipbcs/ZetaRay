#include "SunLight.h"
#include "../../Core/Renderer.h"
#include "../../Core/CommandList.h"
#include "../../Scene/SceneRenderer/SceneRenderer.h"
#include "../../Win32/App.h"
#include "../../RayTracing/Sampler.h"

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::RT;

//--------------------------------------------------------------------------------------
// SunLight
//--------------------------------------------------------------------------------------

SunLight::SunLight() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// frame constants
	m_rootSig.InitAsCBV(0,												// root idx
		0,																// register
		0,																// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_PIXEL,									// visibility
		SceneRenderer::FRAME_CONSTANTS_BUFFER_NAME);

	// BVH
	m_rootSig.InitAsBufferSRV(1,						// root idx
		0,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_PIXEL,					// visibility
		SceneRenderer::RT_SCENE_BVH);

	// Owen-Scrambled Sobol Sequence
	m_rootSig.InitAsBufferSRV(2,						// root idx
		1,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_PIXEL,					// visibility
		Sampler::SOBOL_SEQ);

	// scrambling-tile
	m_rootSig.InitAsBufferSRV(3,						// root idx
		2,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_PIXEL,					// visibility
		Sampler::SCRAMBLING_TILE);

	// ranking-tile
	m_rootSig.InitAsBufferSRV(4,						// root idx
		3,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_PIXEL,					// visibility
		Sampler::SCRAMBLING_TILE);
}

SunLight::~SunLight() noexcept
{
	Reset();
}

void SunLight::Init(D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc) noexcept
{
	auto* samplers = App::GetRenderer().GetStaticSamplers();

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;;

	s_rpObjs.Init("SunLight", m_rootSig, RendererConstants::NUM_STATIC_SAMPLERS, samplers, flags);

	// use an arbitrary number as "nameID" since there's only one shader
	m_pso = s_rpObjs.m_psoLib.GetGraphicsPSO(0, 
		psoDesc, 
		s_rpObjs.m_rootSig.Get(), 
		COMPILED_VS[0],
		COMPILED_PS[0]);

	m_cachedPsoDesc = psoDesc;
	App::AddShaderReloadHandler("SunLight", fastdelegate::MakeDelegate(this, &SunLight::ReloadShaders));
}

void SunLight::Reset() noexcept
{
	if (IsInitialized())
	{
		App::RemoveShaderReloadHandler("SunLight");
		s_rpObjs.Clear();
	}
}

void SunLight::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
	GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

	directCmdList.PIXBeginEvent("SunLight");

	D3D12_VIEWPORT viewports[1] = { App::GetRenderer().GetRenderViewport() };
	D3D12_RECT scissors[1] = { App::GetRenderer().GetRenderScissor() };

	directCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	directCmdList.SetPipelineState(m_pso);

	m_rootSig.End(directCmdList);

	directCmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	directCmdList.RSSetViewportsScissorsRects(1, viewports, scissors);
	Assert(m_cpuDescriptors[SHADER_IN_CPU_DESC::RTV].ptr > 0, "RTV hasn't been set.");
	Assert(m_cpuDescriptors[SHADER_IN_CPU_DESC::DEPTH_BUFFER].ptr > 0, "DSV hasn't been set.");
	directCmdList.OMSetRenderTargets(1, &m_cpuDescriptors[SHADER_IN_CPU_DESC::RTV], true, 
		&m_cpuDescriptors[SHADER_IN_CPU_DESC::DEPTH_BUFFER]);
	directCmdList.DrawInstanced(3, 1, 0, 0);

	directCmdList.PIXEndEvent();
}

void SunLight::ReloadShaders() noexcept
{
	s_rpObjs.m_psoLib.Reload(0, "Sun\\SunLight.hlsl", false);
	m_pso = s_rpObjs.m_psoLib.GetGraphicsPSO(0,
		m_cachedPsoDesc,
		s_rpObjs.m_rootSig.Get(),
		COMPILED_VS[0],
		COMPILED_PS[0]);
}

