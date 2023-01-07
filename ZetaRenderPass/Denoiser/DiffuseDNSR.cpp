#include "DiffuseDNSR.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>
#include <RayTracing/Sampler.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DHelper;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;
using namespace ZetaRay::RT;

//--------------------------------------------------------------------------------------
// DiffuseDNSR
//--------------------------------------------------------------------------------------

DiffuseDNSR::DiffuseDNSR() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// local CB	
	m_rootSig.InitAsConstants(0,					// root idx
		NUM_CONSTS,									// num DWORDs
		0,											// register num
		0);											// register space

	// frame constants
	m_rootSig.InitAsCBV(1,							// root idx
		1,											// register num
		0,											// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::FRAME_CONSTANTS_BUFFER_NAME);

	// Owen-Scrambled Sobol Sequence
	m_rootSig.InitAsBufferSRV(2,						// root idx
		0,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SOBOL_SEQ);

	// scrambling-tile
	m_rootSig.InitAsBufferSRV(3,						// root idx
		1,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE);

	// ranking-tile
	m_rootSig.InitAsBufferSRV(4,						// root idx
		2,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE);
}

DiffuseDNSR::~DiffuseDNSR() noexcept
{
	Reset();
}

void DiffuseDNSR::Init() noexcept
{
	const D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	auto samplers = App::GetRenderer().GetStaticSamplers();
	s_rpObjs.Init("DiffuseDNSR", m_rootSig, samplers.size(), samplers.data(), flags);

	m_psos[(int)SHADERS::TEMPORAL_PASS] = s_rpObjs.m_psoLib.GetComputePSO((int)SHADERS::TEMPORAL_PASS,
		s_rpObjs.m_rootSig.Get(), COMPILED_CS[(int)SHADERS::TEMPORAL_PASS]);
	m_psos[(int)SHADERS::SPATIAL_FILTER] = s_rpObjs.m_psoLib.GetComputePSO((int)SHADERS::SPATIAL_FILTER,
		s_rpObjs.m_rootSig.Get(), COMPILED_CS[(int)SHADERS::SPATIAL_FILTER]);

	m_descTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((int)DESC_TABLE::COUNT);
	CreateResources();

	InitParams();

	m_cbTemporalFilter.IsTemporalCacheValid = false;
	m_cbTemporalFilter.MaxTspp = DefaultParamVals::MaxTSPP;
	m_cbTemporalFilter.MaxPlaneDist = DefaultParamVals::BilinearMaxPlaneDist;
	m_cbTemporalFilter.BilinearNormalScale = DefaultParamVals::BilinearNormalScale;
	m_cbTemporalFilter.BilinearNormalExp = DefaultParamVals::BilinearNormalExp;

	m_cbSpatialFilter.MaxTspp = DefaultParamVals::MaxTSPP;
	m_cbSpatialFilter.MaxPlaneDist = DefaultParamVals::EdgeStoppingMaxPlaneDist;
	m_cbSpatialFilter.NormalExp = DefaultParamVals::EdgeStoppingNormalExp;
	m_cbSpatialFilter.FilterRadiusBase = DefaultParamVals::FilterRadiusBase;

	App::AddShaderReloadHandler("DiffuseDNSR_TemporalPass", fastdelegate::MakeDelegate(this, &DiffuseDNSR::ReloadTemporalPass));
	App::AddShaderReloadHandler("DiffuseDNSR_SpatialFilter", fastdelegate::MakeDelegate(this, &DiffuseDNSR::ReloadSpatialFilter));
}

void DiffuseDNSR::Reset() noexcept
{
	if (IsInitialized())
	{
		s_rpObjs.Clear();

		App::RemoveParam("Renderer", "DiffuseDNSR", "MaxTSPP");
		App::RemoveParam("Renderer", "DiffuseDNSR", "BilinearMaxPlaneDist");
		App::RemoveParam("Renderer", "DiffuseDNSR", "BilinearNormalScale");
		App::RemoveParam("Renderer", "DiffuseDNSR", "BilinearNormalExp");
		App::RemoveParam("Renderer", "DiffuseDNSR", "EdgeStoppingMaxPlaneDist");
		App::RemoveParam("Renderer", "DiffuseDNSR", "EdgeStoppingNormalExp");
		App::RemoveParam("Renderer", "DiffuseDNSR", "SpatialFilter");
		App::RemoveParam("Renderer", "DiffuseDNSR", "#SpatialFilterPasses");
		App::RemoveParam("Renderer", "DiffuseDNSR", "FilterRadiusBase");
		//App::RemoveParam("Renderer", "DiffuseDNSR", "FilterRadiusScale");

		App::RemoveShaderReloadHandler("DiffuseDNSR_TemporalPass");
		App::RemoveShaderReloadHandler("DiffuseDNSR_SpatialFilter");

#ifdef _DEBUG
		memset(m_inputGpuHeapIndices, 0, (int)SHADER_IN_RES::COUNT * sizeof(uint32_t));
#endif // _DEBUG

		memset(m_psos, 0, (int)SHADERS::COUNT * sizeof(ID3D12PipelineState*));

		m_descTable.Reset();
		m_temporalCache[0].Reset();
		m_temporalCache[1].Reset();

		m_isTemporalCacheValid = false;
	}
}

void DiffuseDNSR::OnWindowResized() noexcept
{
	CreateResources();
	m_cbTemporalFilter.IsTemporalCacheValid = false;
}

void DiffuseDNSR::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	const int w = renderer.GetRenderWidth();
	const int h = renderer.GetRenderHeight();
	auto& gpuTimer = renderer.GetGpuTimer();
	
	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

	int temporalCacheSRV = m_currTemporalCacheOutIdx == 1 ? (int)DESC_TABLE::TEMPORAL_CACHE_A_SRV :
		(int)DESC_TABLE::TEMPORAL_CACHE_B_SRV;
	int temporalCacheUAV = m_currTemporalCacheOutIdx == 1 ? (int)DESC_TABLE::TEMPORAL_CACHE_B_UAV :
		(int)DESC_TABLE::TEMPORAL_CACHE_A_UAV;

	const int temporalOut = m_currTemporalCacheOutIdx;
	const int temporalIn = (m_currTemporalCacheOutIdx + 1) & 0x1;

	// temporal pass
	{
		Assert(m_inputGpuHeapIndices[(int)SHADER_IN_RES::RESTIR_GI_RESERVOIR_A] != 0, "Input descriptor heap idx hasn't been set.");

		computeCmdList.PIXBeginEvent("DiffuseDNSR_TemporalPass");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "DiffuseDNSR_TemporalPass");

		computeCmdList.SetPipelineState(m_psos[(uint32_t)SHADERS::TEMPORAL_PASS]);

		m_cbTemporalFilter.InputReservoir_A_DescHeapIdx = m_inputGpuHeapIndices[(int)SHADER_IN_RES::RESTIR_GI_RESERVOIR_A];
		m_cbTemporalFilter.InputReservoir_B_DescHeapIdx = m_inputGpuHeapIndices[(int)SHADER_IN_RES::RESTIR_GI_RESERVOIR_B];
		m_cbTemporalFilter.PrevTemporalCacheDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheSRV);
		m_cbTemporalFilter.CurrTemporalCacheDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheUAV);
		m_cbTemporalFilter.IsTemporalCacheValid = m_isTemporalCacheValid;

		m_rootSig.SetRootConstants(0, sizeof(cbDiffuseDNSRTemporal) / sizeof(DWORD), &m_cbTemporalFilter);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch((uint32_t)CeilUnsignedIntDiv(w, DiffuseDNSR_TEMPORAL_THREAD_GROUP_SIZE_X),
			(uint32_t)CeilUnsignedIntDiv(h, DiffuseDNSR_TEMPORAL_THREAD_GROUP_SIZE_Y),
			1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	if (m_doSpatialFilter)
	{
		computeCmdList.PIXBeginEvent("DiffuseDNSR_SpatialFilter");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "DiffuseDNSR_SpatialFilter");

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::SPATIAL_FILTER]);

		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_Y);

		m_cbSpatialFilter.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbSpatialFilter.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbSpatialFilter.NumGroupsInTile = DiffuseDNSR_SPATIAL_TILE_WIDTH * m_cbSpatialFilter.DispatchDimY;
		m_cbSpatialFilter.NumPasses = m_numSpatialFilterPasses;

		for (int i = 0; i < m_numSpatialFilterPasses; i++)
		{
			m_currTemporalCacheOutIdx = (m_currTemporalCacheOutIdx + 1) & 0x1;

			D3D12_RESOURCE_BARRIER barriers[2];
			barriers[0] = Direct3DHelper::TransitionBarrier(m_temporalCache[m_currTemporalCacheOutIdx].GetResource(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			barriers[1] = Direct3DHelper::TransitionBarrier(m_temporalCache[(m_currTemporalCacheOutIdx + 1) & 0x1].GetResource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

			uint32_t prevTemporalCacheSRV = m_currTemporalCacheOutIdx == 1 ? (uint32_t)DESC_TABLE::TEMPORAL_CACHE_A_SRV :
				(uint32_t)DESC_TABLE::TEMPORAL_CACHE_B_SRV;
			uint32_t nextTemporalCacheUAV = m_currTemporalCacheOutIdx == 1 ? (uint32_t)DESC_TABLE::TEMPORAL_CACHE_B_UAV :
				(uint32_t)DESC_TABLE::TEMPORAL_CACHE_A_UAV;

			m_cbSpatialFilter.TemporalCacheInDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(prevTemporalCacheSRV);
			m_cbSpatialFilter.TemporalCacheOutDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(nextTemporalCacheUAV);
			//m_cbSpatialFilter.Step = 1 << (m_numSpatialFilterPasses - 1 - i);
			//m_cbSpatialFilter.FilterRadiusScale = (float)(1 << (m_numSpatialFilterPasses - 1 - i));
			m_cbSpatialFilter.FilterRadiusScale = (float)(1 << i);
			m_cbSpatialFilter.CurrPass = i;

			m_rootSig.SetRootConstants(0, sizeof(m_cbSpatialFilter) / sizeof(DWORD), &m_cbSpatialFilter);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
		}

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	// restore the initial state
	if (temporalOut != m_currTemporalCacheOutIdx)
	{
		// [hack] render graph is unaware of renderpass-internal transitions. Restore the initial state to avoid
		// render graph and actual state getting out of sync
		D3D12_RESOURCE_BARRIER barrier = Direct3DHelper::TransitionBarrier(m_temporalCache[(m_currTemporalCacheOutIdx + 1) & 0x1].GetResource(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		computeCmdList.ResourceBarrier(&barrier, 1);
	}

	// for next frame
	m_currTemporalCacheOutIdx = (m_currTemporalCacheOutIdx + 1) & 0x1;
	m_isTemporalCacheValid = true;
}

void DiffuseDNSR::CreateResources() noexcept
{
	auto& renderer = App::GetRenderer();

	// temporal cache (ping-pong between frames)
	{
		m_temporalCache[0] = renderer.GetGpuMemory().GetTexture2D("DiffuseDNSR_TEMPORAL_CACHE_A",
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			ResourceFormats::TEMPORAL_CACHE,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		m_temporalCache[1] = renderer.GetGpuMemory().GetTexture2D("DiffuseDNSR_TEMPORAL_CACHE_B",
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			ResourceFormats::TEMPORAL_CACHE,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DHelper::CreateTexture2DSRV(m_temporalCache[0], m_descTable.CPUHandle((int)DESC_TABLE::TEMPORAL_CACHE_A_SRV));
		Direct3DHelper::CreateTexture2DUAV(m_temporalCache[0], m_descTable.CPUHandle((int)DESC_TABLE::TEMPORAL_CACHE_A_UAV));

		Direct3DHelper::CreateTexture2DSRV(m_temporalCache[1], m_descTable.CPUHandle((int)DESC_TABLE::TEMPORAL_CACHE_B_SRV));
		Direct3DHelper::CreateTexture2DUAV(m_temporalCache[1], m_descTable.CPUHandle((int)DESC_TABLE::TEMPORAL_CACHE_B_UAV));
	}
}

void DiffuseDNSR::InitParams() noexcept
{
	ParamVariant enableSpatialFilter;
	enableSpatialFilter.InitBool("Renderer", "DiffuseDNSR", "SpatialFilter",
		fastdelegate::MakeDelegate(this, &DiffuseDNSR::SpatialFilterCallback),
		m_doSpatialFilter);
	App::AddParam(enableSpatialFilter);

	//ParamVariant maxTSPP;
	//maxTSPP.InitInt("Renderer", "DiffuseDNSR", "MaxTSPP", fastdelegate::MakeDelegate(this, &DiffuseDNSR::MaxTSPPCallback),
	//	DefaultParamVals::MaxTSPP,		// val
	//	1,								// min
	//	32,								// max
	//	1);								// step
	//App::AddParam(maxTSPP);

	ParamVariant bilinearMaxPlaneDist;
	bilinearMaxPlaneDist.InitFloat("Renderer", "DiffuseDNSR", "BilinearMaxPlaneDist",
		fastdelegate::MakeDelegate(this, &DiffuseDNSR::BilinearMaxPlaneDistCallback),
		DefaultParamVals::BilinearMaxPlaneDist,		// val	
		1e-2f,										// min
		10.0f,										// max
		1e-2f);										// step
	App::AddParam(bilinearMaxPlaneDist);

	//ParamVariant bilinearNormalScale;
	//bilinearNormalScale.InitFloat("Renderer", "DiffuseDNSR", "BilinearNormalScale",
	//	fastdelegate::MakeDelegate(this, &DiffuseDNSR::BilinearNormalScaleCallback),
	//	DefaultParamVals::BilinearNormalScale,		// val	
	//	1.0f,										// min
	//	5.0f,										// max
	//	0.1f);										// step
	//App::AddParam(bilinearNormalScale);

	//ParamVariant bilinearNormalExp;
	//bilinearNormalExp.InitFloat("Renderer", "DiffuseDNSR", "BilinearNormalExp",
	//	fastdelegate::MakeDelegate(this, &DiffuseDNSR::BilinearNormalExpCallback),
	//	DefaultParamVals::BilinearNormalExp,		// val	
	//	16.0f,										// min
	//	128.0f,										// max
	//	1.0f);										// step	
	//App::AddParam(bilinearNormalExp);

	ParamVariant edgeStoppingNormalExp;
	edgeStoppingNormalExp.InitFloat("Renderer", "DiffuseDNSR", "EdgeStoppingNormalExp",
		fastdelegate::MakeDelegate(this, &DiffuseDNSR::EdgeStoppingNormalExpCallback),
		DefaultParamVals::EdgeStoppingNormalExp,		// val	
		1.0f,											// min
		8.0f,											// max
		1.0f);											// step
	App::AddParam(edgeStoppingNormalExp);

	ParamVariant edgeStoppingPlaneDist;
	edgeStoppingPlaneDist.InitFloat("Renderer", "DiffuseDNSR", "EdgeStoppingMaxPlaneDist",
		fastdelegate::MakeDelegate(this, &DiffuseDNSR::EdgeStoppingMaxPlaneDistCallback),
		DefaultParamVals::EdgeStoppingMaxPlaneDist,	// val	
		1e-2f,										// min
		1.0f,										// max
		1e-1f);										// step
	App::AddParam(edgeStoppingPlaneDist);

	ParamVariant numSpatialFilterPasses;
	numSpatialFilterPasses.InitInt("Renderer", "DiffuseDNSR", "#SpatialFilterPasses",
		fastdelegate::MakeDelegate(this, &DiffuseDNSR::NumSpatialFilterPassesCallback),
		DefaultParamVals::NumSpatialPasses,			// val	
		1,											// min
		3,											// max
		1);											// step
	App::AddParam(numSpatialFilterPasses);

	//ParamVariant baseRadius;
	//baseRadius.InitFloat("Renderer", "DiffuseDNSR", "FilterRadiusBase",
	//	fastdelegate::MakeDelegate(this, &DiffuseDNSR::FilterRadiusBaseCallback),
	//	DefaultParamVals::FilterRadiusBase,			// val	
	//	1e-3f,										// min
	//	1.0f,										// max
	//	1e-3f);										// step
	//App::AddParam(baseRadius);

	//ParamVariant radiusScale;
	//radiusScale.InitFloat("Renderer", "DiffuseDNSR", "FilterRadiusScale",
	//	fastdelegate::MakeDelegate(this, &DiffuseDNSR::FilterRadiusScaleCallback),
	//	DefaultParamVals::FilterRadiusScale,		// val	
	//	1.0f,										// min
	//	10.0f,										// max
	//	0.5f);										// step
	//App::AddParam(radiusScale);
}

void DiffuseDNSR::MaxTSPPCallback(const ParamVariant& p) noexcept
{
	m_cbTemporalFilter.MaxTspp = p.GetInt().m_val;
}

void DiffuseDNSR::BilinearMaxPlaneDistCallback(const ParamVariant& p) noexcept
{
	m_cbTemporalFilter.MaxPlaneDist = p.GetFloat().m_val;
}

//void DiffuseDNSR::BilinearNormalScaleCallback(const ParamVariant& p) noexcept
//{
//	m_cbTemporalFilter.BilinearNormalScale = p.GetFloat().m_val;
//}
//
//void DiffuseDNSR::BilinearNormalExpCallback(const ParamVariant& p) noexcept
//{
//	m_cbTemporalFilter.BilinearNormalExp = p.GetFloat().m_val;
//}

void DiffuseDNSR::EdgeStoppingMaxPlaneDistCallback(const Support::ParamVariant& p) noexcept
{
	m_cbSpatialFilter.MaxPlaneDist = p.GetFloat().m_val;
}

void DiffuseDNSR::EdgeStoppingNormalExpCallback(const ParamVariant& p) noexcept
{
	m_cbSpatialFilter.NormalExp = p.GetFloat().m_val;
}

void DiffuseDNSR::NumSpatialFilterPassesCallback(const ParamVariant& p) noexcept
{
	m_numSpatialFilterPasses = p.GetInt().m_val;
}

void DiffuseDNSR::SpatialFilterCallback(const ParamVariant& p) noexcept
{
	m_doSpatialFilter = p.GetBool();
}

void DiffuseDNSR::FilterRadiusBaseCallback(const ParamVariant& p) noexcept
{
	m_cbSpatialFilter.FilterRadiusBase = p.GetFloat().m_val;
}

void DiffuseDNSR::FilterRadiusScaleCallback(const ParamVariant& p) noexcept
{
	m_cbSpatialFilter.FilterRadiusScale = p.GetFloat().m_val;
}

void DiffuseDNSR::ReloadTemporalPass() noexcept
{
	const int i = (int)SHADERS::TEMPORAL_PASS;

	s_rpObjs.m_psoLib.Reload(i, "Denoiser\\DiffuseDNSR_TemporalFilter.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void DiffuseDNSR::ReloadSpatialFilter() noexcept
{
	const int i = (int)SHADERS::SPATIAL_FILTER;

	s_rpObjs.m_psoLib.Reload(i, "Denoiser\\DiffuseDNSR_SpatialFilter.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}