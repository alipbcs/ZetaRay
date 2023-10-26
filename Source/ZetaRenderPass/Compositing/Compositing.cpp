#include "Compositing.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// Compositing
//--------------------------------------------------------------------------------------

Compositing::Compositing()
	: RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// root constants
	m_rootSig.InitAsConstants(0,				// root idx
		sizeof(cbCompositing) / sizeof(DWORD),	// num DWORDs
		0,										// register
		0);										// register space

	// frame constants
	m_rootSig.InitAsCBV(1,												// root idx
		1,																// register
		0,																// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_ALL,									// visibility
		GlobalResource::FRAME_CONSTANTS_BUFFER);
}

Compositing::~Compositing()
{
	Reset();
}

void Compositing::Init(bool skyIllum)
{
	auto& renderer = App::GetRenderer();
	auto samplers = renderer.GetStaticSamplers();

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	RenderPassBase::InitRenderPass("Compositing", flags, samplers);

	for (int i = 0; i < (int)SHADERS::COUNT; i++)
	{
		m_psos[i] = m_psoLib.GetComputePSO(i,
			m_rootSigObj.Get(),
			COMPILED_CS[i]);
	}

	memset(&m_cbComposit, 0, sizeof(m_cbComposit));
	SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::SUN_DI, true);
	SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::SKY_DI, skyIllum);
	SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::EMISSIVE_DI, true);

	CreateLightAccumTexure();

	ParamVariant p0;
	p0.InitBool("Renderer", "Lighting", "Sun", fastdelegate::MakeDelegate(this, &Compositing::SetSunLightingEnablementCallback),
		IS_CB_FLAG_SET(m_cbComposit, CB_COMPOSIT_FLAGS::SUN_DI));
	App::AddParam(p0);

	//ParamVariant p6;
	//p6.InitBool("Renderer", "Lighting", "Specular Indirect", fastdelegate::MakeDelegate(this, &Compositing::SetSpecularIndirectEnablementCallback),
	//	IS_CB_FLAG_SET(m_cbComposit, CB_COMPOSIT_FLAGS::SPECULAR_INDIRECT));
	//App::AddParam(p6);

	ParamVariant p7;
	p7.InitBool("Renderer", "Lighting", "Emissives", fastdelegate::MakeDelegate(this, &Compositing::SetEmissiveEnablementCallback),
		IS_CB_FLAG_SET(m_cbComposit, CB_COMPOSIT_FLAGS::EMISSIVE_DI));
	App::AddParam(p7);

	ParamVariant p9;
	p9.InitBool("Renderer", "Lighting", "Firefly Suppression", fastdelegate::MakeDelegate(this, &Compositing::SetFireflyFilterEnablement),
		m_filterFirefly);
	App::AddParam(p9);

	App::AddShaderReloadHandler("Compositing", fastdelegate::MakeDelegate(this, &Compositing::ReloadCompsiting));
}

void Compositing::Reset()
{
	if (IsInitialized())
	{
		m_hdrLightAccum.Reset();
		RenderPassBase::ResetRenderPass();
	}
}

void Compositing::OnWindowResized()
{
	CreateLightAccumTexure();
}

void Compositing::Render(CommandList& cmdList)
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	const uint32_t w = App::GetRenderer().GetRenderWidth();
	const uint32_t h = App::GetRenderer().GetRenderHeight();
	auto& gpuTimer = App::GetRenderer().GetGpuTimer();

	computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

	// compositing
	{
		computeCmdList.PIXBeginEvent("Compositing");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "Compositing");

		const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, COMPOSITING_THREAD_GROUP_DIM_X);
		const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, COMPOSITING_THREAD_GROUP_DIM_Y);

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::COMPOSIT]);

		if (IS_CB_FLAG_SET(m_cbComposit, CB_COMPOSIT_FLAGS::INSCATTERING))
		{
			Assert(m_cbComposit.InscatteringDescHeapIdx > 0, "Gpu descriptor for inscattering texture hasn't been set");
			Assert(m_cbComposit.VoxelGridNearZ >= 0.0f, "Invalid voxel grid depth");
			Assert(m_cbComposit.VoxelGridFarZ > m_cbComposit.VoxelGridNearZ, "Invalid voxel grid depth");
			Assert(m_cbComposit.DepthMappingExp > 0.0f, "Invalid voxel grid depth mapping exponent");
		}

		m_cbComposit.CompositedUAVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::LIGHT_ACCUM_UAV);

		m_rootSig.SetRootConstants(0, sizeof(cbCompositing) / sizeof(DWORD), &m_cbComposit);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	if (m_filterFirefly)
	{
		computeCmdList.PIXBeginEvent("FireflyFilter");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "FireflyFilter");

		const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, FIREFLY_FILTER_THREAD_GROUP_DIM_X);
		const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, FIREFLY_FILTER_THREAD_GROUP_DIM_Y);

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::FIREFLY_FILTER]);

		computeCmdList.UAVBarrier(m_hdrLightAccum.Resource());

		cbFireflyFilter cb;
		cb.CompositedUAVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::LIGHT_ACCUM_UAV);

		m_rootSig.SetRootConstants(0, sizeof(cbFireflyFilter) / sizeof(DWORD), &cb);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}
}

void Compositing::CreateLightAccumTexure()
{
	auto& renderer = App::GetRenderer();
	m_descTable = renderer.GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);

	D3D12_CLEAR_VALUE clearValue = {};
	memset(clearValue.Color, 0, sizeof(float) * 4);
	clearValue.Format = ResourceFormats::LIGHT_ACCUM;

	m_hdrLightAccum = GpuMemory::GetTexture2D("HDRLightAccum",
		renderer.GetRenderWidth(), renderer.GetRenderHeight(),
		ResourceFormats::LIGHT_ACCUM,
		D3D12_RESOURCE_STATE_COMMON,
		CREATE_TEXTURE_FLAGS::ALLOW_RENDER_TARGET | CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS,
		1,
		&clearValue);

	Direct3DUtil::CreateTexture2DUAV(m_hdrLightAccum, m_descTable.CPUHandle((int)DESC_TABLE::LIGHT_ACCUM_UAV));
}

void Compositing::SetSkyIllumEnablement(bool b)
{
	SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::SKY_DI, b);
}

void Compositing::SetFireflyFilterEnablement(const Support::ParamVariant& p)
{
	m_filterFirefly = p.GetBool();
}

void Compositing::SetSunLightingEnablementCallback(const Support::ParamVariant& p)
{
	SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::SUN_DI, p.GetBool());
}

//void Compositing::SetSpecularIndirectEnablementCallback(const Support::ParamVariant& p)
//{
//	SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::SPECULAR_INDIRECT, p.GetBool());
//}

void Compositing::SetEmissiveEnablementCallback(const Support::ParamVariant& p)
{
	SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::EMISSIVE_DI, p.GetBool());
}

void Compositing::ReloadCompsiting()
{
	const int i = (int)SHADERS::COMPOSIT;

	m_psoLib.Reload(0, "Compositing\\Compositing.hlsl", true);
	m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}