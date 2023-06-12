#include "SunShadow.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <RayTracing/Sampler.h>
#include <Support/Param.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::RT;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// SunShadow
//--------------------------------------------------------------------------------------

SunShadow::SunShadow() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// frame constants
	m_rootSig.InitAsCBV(0,												// root idx
		0,																// register
		0,																// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_ALL,									// visibility
		GlobalResource::FRAME_CONSTANTS_BUFFER_NAME);

	// root constants
	m_rootSig.InitAsConstants(1,		// root idx
		NUM_CONSTS,						// num DWORDs
		1,								// register
		0);								// register space

	// BVH
	m_rootSig.InitAsBufferSRV(2,						// root idx
		0,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		GlobalResource::RT_SCENE_BVH);

	// Owen-Scrambled Sobol Sequence
	m_rootSig.InitAsBufferSRV(3,						// root idx
		1,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SOBOL_SEQ);

	// scrambling tile
	m_rootSig.InitAsBufferSRV(4,						// root idx
		2,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE);

	// ranking tile
	m_rootSig.InitAsBufferSRV(5,						// root idx
		3,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE);

	m_oldNumSpatialPasses = m_numSpatialPasses;
}

SunShadow::~SunShadow() noexcept
{
	Reset();
}

void SunShadow::Init() noexcept
{
	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;;

	auto samplers = App::GetRenderer().GetStaticSamplers();
	s_rpObjs.Init("SunShadow", m_rootSig, samplers.size(), samplers.data(), flags);

	auto createPSO = [this](int i)
	{
		m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
			s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[i]);
	};

	createPSO((int)SHADERS::SHADOW_MASK);
	createPSO((int)SHADERS::DNSR_TEMPORAL_PASS);
	createPSO((int)SHADERS::DNSR_SPATIAL_FILTER);

	m_descTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((int)DESC_TABLE::COUNT);
	CreateResources();

	m_temporalCB.IsTemporalValid = false;
	m_spatialCB.EdgeStoppingShadowStdScale = DefaultParamVals::EdgeStoppingShadowStdScale;
	m_spatialCB.EdgeStoppingNormalExp = DefaultParamVals::EdgeStoppingNormalExp;
	m_spatialCB.MinFilterVar = 0.0f;

	ParamVariant softShadows;
	softShadows.InitBool("Renderer", "SunShadow", "SoftShadows",
		fastdelegate::MakeDelegate(this, &SunShadow::DoSoftShadowsCallback), m_doSoftShadows);
	App::AddParam(softShadows);

	ParamVariant numSpatialPasses;
	numSpatialPasses.InitInt("Renderer", "SunShadow", "#SpatialFilterPasses",
		fastdelegate::MakeDelegate(this, &SunShadow::NumSpatialFilterPassesCallback), m_numSpatialPasses, 0, 3, 1);
	App::AddParam(numSpatialPasses);

	ParamVariant minVar;
	minVar.InitFloat("Renderer", "SunShadow", "MinFilterVariance",
		fastdelegate::MakeDelegate(this, &SunShadow::MinFilterVarianceCallback),
		m_spatialCB.MinFilterVar,					// val	
		0.0f,										// min
		8.0f,										// max
		1e-2f);										// step
	App::AddParam(minVar);

	//App::AddShaderReloadHandler("SunShadow_DNSR_Temporal", fastdelegate::MakeDelegate(this, &SunShadow::ReloadDNSRTemporal));
	//App::AddShaderReloadHandler("SunShadow_DNSR_Spatial", fastdelegate::MakeDelegate(this, &SunShadow::ReloadDNSRSpatial));
	App::AddShaderReloadHandler("SunShadow_Trace", fastdelegate::MakeDelegate(this, &SunShadow::ReloadSunShadowTrace));
}

void SunShadow::Reset() noexcept
{
	if (IsInitialized())
	{
		s_rpObjs.Clear();

		m_shadowMask.Reset();
		m_descTable.Reset();
	}
}

void SunShadow::OnWindowResized() noexcept
{
	CreateResources();
}

void SunShadow::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();
	const int w = renderer.GetRenderWidth();
	const int h = renderer.GetRenderHeight();

	const int originalTemporalCacheIdx = m_currTemporalCacheOutIdx;

	int temporalCacheSRV = m_currTemporalCacheOutIdx == 1 ? (int)DESC_TABLE::TEMPORAL_CACHE_A_SRV :
		(int)DESC_TABLE::TEMPORAL_CACHE_B_SRV;
	int temporalCacheUAV = m_currTemporalCacheOutIdx == 1 ? (int)DESC_TABLE::TEMPORAL_CACHE_B_UAV :
		(int)DESC_TABLE::TEMPORAL_CACHE_A_UAV;

	auto swapTemporalCaches = [this, &temporalCacheSRV, &temporalCacheUAV]()
	{
		m_currTemporalCacheOutIdx = (m_currTemporalCacheOutIdx + 1) & 0x1;

		temporalCacheSRV = m_currTemporalCacheOutIdx == 1 ? (int)DESC_TABLE::TEMPORAL_CACHE_A_SRV :
			(int)DESC_TABLE::TEMPORAL_CACHE_B_SRV;
		temporalCacheUAV = m_currTemporalCacheOutIdx == 1 ? (int)DESC_TABLE::TEMPORAL_CACHE_B_UAV :
			(int)DESC_TABLE::TEMPORAL_CACHE_A_UAV;
	};

	auto swapAndTransitionTemporalCaches = [this, &computeCmdList, &temporalCacheSRV, &temporalCacheUAV]()
	{
		m_currTemporalCacheOutIdx = (m_currTemporalCacheOutIdx + 1) & 0x1;

		temporalCacheSRV = m_currTemporalCacheOutIdx == 1 ? (int)DESC_TABLE::TEMPORAL_CACHE_A_SRV :
			(int)DESC_TABLE::TEMPORAL_CACHE_B_SRV;
		temporalCacheUAV = m_currTemporalCacheOutIdx == 1 ? (int)DESC_TABLE::TEMPORAL_CACHE_B_UAV :
			(int)DESC_TABLE::TEMPORAL_CACHE_A_UAV;

		D3D12_RESOURCE_BARRIER postBarriers[] =
		{
			Direct3DHelper::TransitionBarrier(m_temporalCache[m_currTemporalCacheOutIdx].GetResource(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			Direct3DHelper::TransitionBarrier(m_temporalCache[1 - m_currTemporalCacheOutIdx].GetResource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
		};

		computeCmdList.ResourceBarrier(postBarriers, ZetaArrayLen(postBarriers));
	};

	// shadow mask
	{
		computeCmdList.PIXBeginEvent("SunShadowTrace");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SunShadowTrace");

		computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::SHADOW_MASK]);

		cbSunShadow localCB;
		localCB.OutShadowMaskDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::SHADOW_MASK_UAV);
		localCB.SoftShadows = m_doSoftShadows;
		m_rootSig.SetRootConstants(0, sizeof(cbSunShadow) / sizeof(DWORD), &localCB);

		m_rootSig.End(computeCmdList);

		const int numGroupsX = (uint32_t)CeilUnsignedIntDiv(w, SUN_SHADOW_THREAD_GROUP_SIZE_X);
		const int numGroupsY = (uint32_t)CeilUnsignedIntDiv(h, SUN_SHADOW_THREAD_GROUP_SIZE_Y);

		computeCmdList.Dispatch(numGroupsX, numGroupsY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	// temporal pass
	{
		computeCmdList.PIXBeginEvent("SunShadowDNSR_Temporal");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SunShadowDNSR_Temporal");

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_TEMPORAL_PASS]);

		D3D12_RESOURCE_BARRIER barriers[] =
		{
			Direct3DHelper::TransitionBarrier(m_shadowMask.GetResource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			Direct3DHelper::TransitionBarrier(m_metadata.GetResource(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		};

		computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

		m_temporalCB.ShadowMaskSRVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::SHADOW_MASK_SRV);
		m_temporalCB.MomentsUAVHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::MOMENTS_UAV);
		m_temporalCB.MetadataUAVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::METADATA_UAV);
		m_temporalCB.PrevTemporalCacheHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheSRV);
		m_temporalCB.CurrTemporalCacheHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheUAV);
		m_temporalCB.NumShadowMaskThreadGroupsX = (uint16_t)CeilUnsignedIntDiv(w, SUN_SHADOW_THREAD_GROUP_SIZE_X);
		m_temporalCB.NumShadowMaskThreadGroupsY = (uint16_t)CeilUnsignedIntDiv(h, SUN_SHADOW_THREAD_GROUP_SIZE_Y);

		m_rootSig.SetRootConstants(0, sizeof(cbFFX_DNSR_Temporal) / sizeof(DWORD), &m_temporalCB);
		m_rootSig.End(computeCmdList);

		const int numGroupsX = (uint32_t)CeilUnsignedIntDiv(w, DNSR_TEMPORAL_THREAD_GROUP_SIZE_X);
		const int numGroupsY = (uint32_t)CeilUnsignedIntDiv(h, DNSR_TEMPORAL_THREAD_GROUP_SIZE_Y);

		computeCmdList.Dispatch(numGroupsX, numGroupsY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	// spatial filter
	{
		computeCmdList.PIXBeginEvent("SunShadowDNSR_Spatial");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SunShadowDNSR_Spatial");

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_SPATIAL_FILTER]);

		swapTemporalCaches();

		D3D12_RESOURCE_BARRIER preBarriers[] =
		{
			Direct3DHelper::TransitionBarrier(m_metadata.GetResource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			Direct3DHelper::TransitionBarrier(m_temporalCache[m_currTemporalCacheOutIdx].GetResource(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			Direct3DHelper::TransitionBarrier(m_temporalCache[1 - m_currTemporalCacheOutIdx].GetResource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
			Direct3DHelper::TransitionBarrier(m_shadowMask.GetResource(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};

		computeCmdList.ResourceBarrier(preBarriers, ZetaArrayLen(preBarriers));

		const int numGroupsX = (uint32_t)CeilUnsignedIntDiv(w, DNSR_SPATIAL_FILTER_THREAD_GROUP_SIZE_X);
		const int numGroupsY = (uint32_t)CeilUnsignedIntDiv(h, DNSR_SPATIAL_FILTER_THREAD_GROUP_SIZE_Y);

		for (int i = 0; i < m_numSpatialPasses; i++)
		{
			m_spatialCB.PassNum = (uint16_t)i;
			m_spatialCB.StepSize = 1 << i;

			m_spatialCB.MetadataSRVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::METADATA_SRV);
			m_spatialCB.InTemporalCacheHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheSRV);
			m_spatialCB.OutTemporalCacheHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheUAV);

			m_rootSig.SetRootConstants(0, sizeof(cbFFX_DNSR_Spatial) / sizeof(DWORD), &m_spatialCB);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch(numGroupsX, numGroupsY, 1);
			
			// swap temporal caches
			if(i != m_numSpatialPasses - 1)
				swapAndTransitionTemporalCaches();
		}

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		// [hack] render graph is unaware of renderpass-internal transitions. Restore the initial state to avoid
		// render graph and actual state getting out of sync
		if(m_currTemporalCacheOutIdx != originalTemporalCacheIdx)
			swapAndTransitionTemporalCaches();

		computeCmdList.PIXEndEvent();
	}

	m_currTemporalCacheOutIdx = (m_currTemporalCacheOutIdx + 1) & 0x1;
	m_temporalCB.IsTemporalValid = true;
}

void SunShadow::CreateResources() noexcept
{
	auto& gpuMem = App::GetRenderer().GetGpuMemory();

	const int w = App::GetRenderer().GetRenderWidth();
	const int h = App::GetRenderer().GetRenderHeight();

	// shadow mask
	{
		const int texWidth = (uint32_t)CeilUnsignedIntDiv(w, SUN_SHADOW_THREAD_GROUP_SIZE_X);
		const int texHeight = (uint32_t)CeilUnsignedIntDiv(h, SUN_SHADOW_THREAD_GROUP_SIZE_Y);

		m_shadowMask = gpuMem.GetTexture2D("SunShadowMask",
			texWidth, texHeight,
			ResourceFormats::SHADOW_MASK,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DHelper::CreateTexture2DSRV(m_shadowMask, m_descTable.CPUHandle((uint32_t)DESC_TABLE::SHADOW_MASK_SRV));
		Direct3DHelper::CreateTexture2DUAV(m_shadowMask, m_descTable.CPUHandle((uint32_t)DESC_TABLE::SHADOW_MASK_UAV));
	}

	// metadata
	{
		const int texWidth = (uint32_t)CeilUnsignedIntDiv(w, DNSR_TEMPORAL_THREAD_GROUP_SIZE_X);
		const int texHeight = (uint32_t)CeilUnsignedIntDiv(h, DNSR_TEMPORAL_THREAD_GROUP_SIZE_Y);

		m_metadata = gpuMem.GetTexture2D("SunShadowMetadata",
			texWidth, texHeight,
			ResourceFormats::THREAD_GROUP_METADATA,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DHelper::CreateTexture2DSRV(m_metadata, m_descTable.CPUHandle((uint32_t)DESC_TABLE::METADATA_SRV));
		Direct3DHelper::CreateTexture2DUAV(m_metadata, m_descTable.CPUHandle((uint32_t)DESC_TABLE::METADATA_UAV));
	}

	// moments
	{
		m_moments = gpuMem.GetTexture2D("SunShadowMoments",
			w, h,
			ResourceFormats::MOMENTS,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DHelper::CreateTexture2DUAV(m_moments, m_descTable.CPUHandle((uint32_t)DESC_TABLE::MOMENTS_UAV));
	}

	// temporal cache
	{
		m_temporalCache[0] = gpuMem.GetTexture2D("SunShadowTemporalCache_A",
			w, h,
			ResourceFormats::TEMPORAL_CACHE,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		m_temporalCache[1] = gpuMem.GetTexture2D("ShadowTemporalCache_B",
			w, h,
			ResourceFormats::TEMPORAL_CACHE,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DHelper::CreateTexture2DSRV(m_temporalCache[0], m_descTable.CPUHandle((uint32_t)DESC_TABLE::TEMPORAL_CACHE_A_SRV));
		Direct3DHelper::CreateTexture2DUAV(m_temporalCache[0], m_descTable.CPUHandle((uint32_t)DESC_TABLE::TEMPORAL_CACHE_A_UAV));

		Direct3DHelper::CreateTexture2DSRV(m_temporalCache[1], m_descTable.CPUHandle((uint32_t)DESC_TABLE::TEMPORAL_CACHE_B_SRV));
		Direct3DHelper::CreateTexture2DUAV(m_temporalCache[1], m_descTable.CPUHandle((uint32_t)DESC_TABLE::TEMPORAL_CACHE_B_UAV));
	}
}

void SunShadow::DoSoftShadowsCallback(const Support::ParamVariant& p) noexcept
{
	m_doSoftShadows = p.GetBool();

	if (!m_doSoftShadows)
	{
		m_oldNumSpatialPasses = m_numSpatialPasses;
		m_numSpatialPasses = 0;
	}
	else
		m_numSpatialPasses = m_oldNumSpatialPasses;
}

void SunShadow::NumSpatialFilterPassesCallback(const Support::ParamVariant& p) noexcept
{
	m_numSpatialPasses = p.GetInt().m_val;
}

void SunShadow::MinFilterVarianceCallback(const Support::ParamVariant& p) noexcept
{
	m_spatialCB.MinFilterVar = p.GetFloat().m_val;
}

void SunShadow::EdgeStoppingShadowStdScaleCallback(const Support::ParamVariant& p) noexcept
{
	m_spatialCB.EdgeStoppingShadowStdScale = p.GetFloat().m_val;
}

void SunShadow::ReloadDNSRTemporal() noexcept
{
	const int i = (int)SHADERS::DNSR_TEMPORAL_PASS;

	s_rpObjs.m_psoLib.Reload(i, "SunShadow\\ffx_denoiser_temporal.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void SunShadow::ReloadDNSRSpatial() noexcept
{
	const int i = (int)SHADERS::DNSR_SPATIAL_FILTER;

	s_rpObjs.m_psoLib.Reload(i, "SunShadow\\ffx_denoiser_spatial_filter.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void SunShadow::ReloadSunShadowTrace() noexcept
{
	const int i = (int)SHADERS::SHADOW_MASK;

	s_rpObjs.m_psoLib.Reload(i, "SunShadow\\SunShadow.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}
