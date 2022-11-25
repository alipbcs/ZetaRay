#include "STAD.h"
#include "../../Core/Renderer.h"
#include "../../Core/CommandList.h"
#include "../../Scene/SceneRenderer/SceneRenderer.h"
#include "../../Win32/App.h"
#include "../../Support/Param.h"
#include "../../RayTracing/Sampler.h"

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DHelper;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;
using namespace ZetaRay::RT;

//--------------------------------------------------------------------------------------
// STAD
//--------------------------------------------------------------------------------------

STAD::STAD() noexcept
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
		SceneRenderer::FRAME_CONSTANTS_BUFFER_NAME);

	// Owen-Scrambled Sobol Sequence
	m_rootSig.InitAsBufferSRV(2,						// root idx
		0,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SOBOL_SEQ);

	// scrambling-tile
	m_rootSig.InitAsBufferSRV(3,						// root idx
		1,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE);

	// ranking-tile
	m_rootSig.InitAsBufferSRV(4,						// root idx
		2,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE);
}

STAD::~STAD() noexcept
{
	Reset();
}

void STAD::Init() noexcept
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

	auto* samplers = App::GetRenderer().GetStaticSamplers();
	s_rpObjs.Init("STAD", m_rootSig, RendererConstants::NUM_STATIC_SAMPLERS, samplers, flags);

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

	App::AddShaderReloadHandler("STAD_TemporalPass", fastdelegate::MakeDelegate(this, &STAD::ReloadTemporalPass));
	App::AddShaderReloadHandler("STAD_SpatialFilter", fastdelegate::MakeDelegate(this, &STAD::ReloadSpatialFilter));
}

void STAD::Reset() noexcept
{
	if (IsInitialized())
	{
		s_rpObjs.Clear();

		App::RemoveParam("Renderer", "STAD", "MaxTSPP");
		App::RemoveParam("Renderer", "STAD", "BilinearMaxPlaneDist");
		App::RemoveParam("Renderer", "STAD", "BilinearNormalScale");
		App::RemoveParam("Renderer", "STAD", "BilinearNormalExp");
		App::RemoveParam("Renderer", "STAD", "EdgeStoppingMaxPlaneDist");
		App::RemoveParam("Renderer", "STAD", "EdgeStoppingNormalExp");
		App::RemoveParam("Renderer", "STAD", "SpatialFilter");
		App::RemoveParam("Renderer", "STAD", "#SpatialFilterPasses");
		App::RemoveParam("Renderer", "STAD", "FilterRadiusBase");
		//App::RemoveParam("Renderer", "STAD", "FilterRadiusScale");

		App::RemoveShaderReloadHandler("STAD_TemporalPass");
		App::RemoveShaderReloadHandler("STAD_SpatialFilter");

#ifdef _DEBUG
		memset(m_inputGpuHeapIndices, 0, (int)SHADER_IN_RES::COUNT * sizeof(uint32_t));
#endif // _DEBUG

		memset(m_psos, 0, (int)SHADERS::COUNT * sizeof(ID3D12PipelineState*));

		m_descTable.Reset();
		m_temporalCache[0].Reset();
		m_temporalCache[1].Reset();

		m_descTable.Reset();

		m_isTemporalCacheValid = false;
	}
}

void STAD::OnWindowResized() noexcept
{
	CreateResources();
	m_cbTemporalFilter.IsTemporalCacheValid = false;
}

void STAD::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	const int w = renderer.GetRenderWidth();
	const int h = renderer.GetRenderHeight();
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

		computeCmdList.PIXBeginEvent("STAD_TemporalPass");
		computeCmdList.SetPipelineState(m_psos[(uint32_t)SHADERS::TEMPORAL_PASS]);

		m_cbTemporalFilter.InputReservoir_A_DescHeapIdx = m_inputGpuHeapIndices[(int)SHADER_IN_RES::RESTIR_GI_RESERVOIR_A];
		m_cbTemporalFilter.InputReservoir_B_DescHeapIdx = m_inputGpuHeapIndices[(int)SHADER_IN_RES::RESTIR_GI_RESERVOIR_B];
		m_cbTemporalFilter.PrevTemporalCacheDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheSRV);
		m_cbTemporalFilter.CurrTemporalCacheDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheUAV);
		m_cbTemporalFilter.IsTemporalCacheValid = m_isTemporalCacheValid;

		m_rootSig.SetRootConstants(0, sizeof(cbSTADTemporalFilter) / sizeof(DWORD), &m_cbTemporalFilter);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch((uint32_t)CeilUnsignedIntDiv(w, STAD_TEMPORAL_PASS_THREAD_GROUP_SIZE_X),
			(uint32_t)CeilUnsignedIntDiv(h, STAD_TEMPORAL_PASS_THREAD_GROUP_SIZE_Y),
			STAD_TEMPORAL_PASS_THREAD_GROUP_SIZE_Z);

		computeCmdList.PIXEndEvent();
	}

	if (m_doSpatialFilter)
	{
		computeCmdList.PIXBeginEvent("STAD_SpatialFilter");

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::SPATIAL_FILTER]);

		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, STAD_SPATIAL_FILTER_THREAD_GROUP_SIZE_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, STAD_SPATIAL_FILTER_THREAD_GROUP_SIZE_Y);

		m_cbSpatialFilter.DispatchDimX = dispatchDimX;
		m_cbSpatialFilter.DispatchDimY = dispatchDimY;
		m_cbSpatialFilter.NumGroupsInTile = STAD_SPATIAL_TILE_WIDTH * m_cbSpatialFilter.DispatchDimY;
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

			computeCmdList.TransitionResource(barriers, ZetaArrayLen(barriers));

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

		computeCmdList.TransitionResource(&barrier, 1);
	}

	// for next frame
	m_currTemporalCacheOutIdx = (m_currTemporalCacheOutIdx + 1) & 0x1;
	m_isTemporalCacheValid = true;
}

void STAD::CreateResources() noexcept
{
	auto& renderer = App::GetRenderer();
	auto* device = App::GetRenderer().GetDevice();

	// SRV
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.PlaneSlice = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	// UAV
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Texture2D.MipSlice = 0;
	uavDesc.Texture2D.PlaneSlice = 0;

	// temporal cache (ping-pong between frames)
	{
		m_temporalCache[0] = renderer.GetGpuMemory().GetTexture2D("STAD_TEMPORAL_CACHE_A",
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			ResourceFormats::TEMPORAL_CACHE,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		m_temporalCache[1] = renderer.GetGpuMemory().GetTexture2D("STAD_TEMPORAL_CACHE_B",
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			ResourceFormats::TEMPORAL_CACHE,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		srvDesc.Format = ResourceFormats::TEMPORAL_CACHE;
		uavDesc.Format = ResourceFormats::TEMPORAL_CACHE;

		device->CreateShaderResourceView(m_temporalCache[0].GetResource(),
			&srvDesc,
			m_descTable.CPUHandle((int)DESC_TABLE::TEMPORAL_CACHE_A_SRV));
		device->CreateUnorderedAccessView(m_temporalCache[0].GetResource(),
			nullptr,
			&uavDesc,
			m_descTable.CPUHandle((int)DESC_TABLE::TEMPORAL_CACHE_A_UAV));

		device->CreateShaderResourceView(m_temporalCache[1].GetResource(),
			&srvDesc,
			m_descTable.CPUHandle((int)DESC_TABLE::TEMPORAL_CACHE_B_SRV));
		device->CreateUnorderedAccessView(m_temporalCache[1].GetResource(),
			nullptr,
			&uavDesc,
			m_descTable.CPUHandle((int)DESC_TABLE::TEMPORAL_CACHE_B_UAV));
	}
}

void STAD::InitParams() noexcept
{
	ParamVariant enableSpatialFilter;
	enableSpatialFilter.InitBool("Renderer", "STAD", "SpatialFilter",
		fastdelegate::MakeDelegate(this, &STAD::SpatialFilterCallback),
		m_doSpatialFilter);
	App::AddParam(enableSpatialFilter);

	ParamVariant maxTSPP;
	maxTSPP.InitInt("Renderer", "STAD", "MaxTSPP", fastdelegate::MakeDelegate(this, &STAD::MaxTSPPCallback),
		DefaultParamVals::MaxTSPP,		// val
		1,								// min
		32,								// max
		1);								// step
	App::AddParam(maxTSPP);

	ParamVariant bilinearMaxPlaneDist;
	bilinearMaxPlaneDist.InitFloat("Renderer", "STAD", "BilinearMaxPlaneDist",
		fastdelegate::MakeDelegate(this, &STAD::BilinearMaxPlaneDistCallback),
		DefaultParamVals::BilinearMaxPlaneDist,		// val	
		1e-2f,										// min
		10.0f,										// max
		1e-2f);										// step
	App::AddParam(bilinearMaxPlaneDist);

	//ParamVariant bilinearNormalScale;
	//bilinearNormalScale.InitFloat("Renderer", "STAD", "BilinearNormalScale",
	//	fastdelegate::MakeDelegate(this, &STAD::BilinearNormalScaleCallback),
	//	DefaultParamVals::BilinearNormalScale,		// val	
	//	1.0f,										// min
	//	5.0f,										// max
	//	0.1f);										// step
	//App::AddParam(bilinearNormalScale);

	//ParamVariant bilinearNormalExp;
	//bilinearNormalExp.InitFloat("Renderer", "STAD", "BilinearNormalExp",
	//	fastdelegate::MakeDelegate(this, &STAD::BilinearNormalExpCallback),
	//	DefaultParamVals::BilinearNormalExp,		// val	
	//	16.0f,										// min
	//	128.0f,										// max
	//	1.0f);										// step	
	//App::AddParam(bilinearNormalExp);

	ParamVariant edgeStoppingNormalExp;
	edgeStoppingNormalExp.InitFloat("Renderer", "STAD", "EdgeStoppingNormalExp",
		fastdelegate::MakeDelegate(this, &STAD::EdgeStoppingNormalExpCallback),
		DefaultParamVals::EdgeStoppingNormalExp,		// val	
		1.0f,											// min
		8.0f,											// max
		1.0f);											// step
	App::AddParam(edgeStoppingNormalExp);

	ParamVariant edgeStoppingPlaneDist;
	edgeStoppingPlaneDist.InitFloat("Renderer", "STAD", "EdgeStoppingMaxPlaneDist",
		fastdelegate::MakeDelegate(this, &STAD::EdgeStoppingMaxPlaneDistCallback),
		DefaultParamVals::EdgeStoppingMaxPlaneDist,	// val	
		1e-2f,										// min
		1.0f,										// max
		1e-1f);										// step
	App::AddParam(edgeStoppingPlaneDist);

	ParamVariant numSpatialFilterPasses;
	numSpatialFilterPasses.InitInt("Renderer", "STAD", "#SpatialFilterPasses",
		fastdelegate::MakeDelegate(this, &STAD::NumSpatialFilterPassesCallback),
		DefaultParamVals::NumSpatialPasses,			// val	
		1,											// min
		3,											// max
		1);											// step
	App::AddParam(numSpatialFilterPasses);

	ParamVariant baseRadius;
	baseRadius.InitFloat("Renderer", "STAD", "FilterRadiusBase",
		fastdelegate::MakeDelegate(this, &STAD::FilterRadiusBaseCallback),
		DefaultParamVals::FilterRadiusBase,			// val	
		1e-3f,										// min
		1.0f,										// max
		1e-3f);										// step
	App::AddParam(baseRadius);

	//ParamVariant radiusScale;
	//radiusScale.InitFloat("Renderer", "STAD", "FilterRadiusScale",
	//	fastdelegate::MakeDelegate(this, &STAD::FilterRadiusScaleCallback),
	//	DefaultParamVals::FilterRadiusScale,		// val	
	//	1.0f,										// min
	//	10.0f,										// max
	//	0.5f);										// step
	//App::AddParam(radiusScale);
}

void STAD::MaxTSPPCallback(const ParamVariant& p) noexcept
{
	m_cbTemporalFilter.MaxTspp = p.GetInt().m_val;
}

void STAD::BilinearMaxPlaneDistCallback(const ParamVariant& p) noexcept
{
	m_cbTemporalFilter.MaxPlaneDist = p.GetFloat().m_val;
}

//void STAD::BilinearNormalScaleCallback(const ParamVariant& p) noexcept
//{
//	m_cbTemporalFilter.BilinearNormalScale = p.GetFloat().m_val;
//}
//
//void STAD::BilinearNormalExpCallback(const ParamVariant& p) noexcept
//{
//	m_cbTemporalFilter.BilinearNormalExp = p.GetFloat().m_val;
//}

void STAD::EdgeStoppingMaxPlaneDistCallback(const Support::ParamVariant& p) noexcept
{
	m_cbSpatialFilter.MaxPlaneDist = p.GetFloat().m_val;
}

void STAD::EdgeStoppingNormalExpCallback(const ParamVariant& p) noexcept
{
	m_cbSpatialFilter.NormalExp = p.GetFloat().m_val;
}

void STAD::NumSpatialFilterPassesCallback(const ParamVariant& p) noexcept
{
	m_numSpatialFilterPasses = p.GetInt().m_val;
}

void STAD::SpatialFilterCallback(const ParamVariant& p) noexcept
{
	m_doSpatialFilter = p.GetBool();
}

void STAD::FilterRadiusBaseCallback(const ParamVariant& p) noexcept
{
	m_cbSpatialFilter.FilterRadiusBase = p.GetFloat().m_val;
}

void STAD::FilterRadiusScaleCallback(const ParamVariant& p) noexcept
{
	m_cbSpatialFilter.FilterRadiusScale = p.GetFloat().m_val;
}

void STAD::ReloadTemporalPass() noexcept
{
	const int i = (int)SHADERS::TEMPORAL_PASS;

	s_rpObjs.m_psoLib.Reload(i, "Denoiser\\STAD_TemporalFilter.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void STAD::ReloadSpatialFilter() noexcept
{
	const int i = (int)SHADERS::SPATIAL_FILTER;

	s_rpObjs.m_psoLib.Reload(i, "Denoiser\\STAD_SpatialFilter.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}