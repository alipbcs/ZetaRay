#include "SkyDI.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>
#include <RayTracing/Sampler.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;
using namespace ZetaRay::RT;

//--------------------------------------------------------------------------------------
// SkyDI
//--------------------------------------------------------------------------------------

SkyDI::SkyDI() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// root constants
	m_rootSig.InitAsConstants(0,		// root idx
		NUM_CONSTS,						// num DWORDs
		1,								// register
		0);								// register space

	// frame constants
	m_rootSig.InitAsCBV(1,												// root idx
		0,																// register
		0,																// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_ALL,									// visibility
		GlobalResource::FRAME_CONSTANTS_BUFFER);

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
		Sampler::SOBOL_SEQ_32);

	// scrambling tile
	m_rootSig.InitAsBufferSRV(4,						// root idx
		2,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE_32);

	// ranking tile
	m_rootSig.InitAsBufferSRV(5,						// root idx
		3,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE_32);
}

SkyDI::~SkyDI() noexcept
{
	Reset();
}

void SkyDI::Init() noexcept
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

	auto& renderer = App::GetRenderer();
	auto samplers = renderer.GetStaticSamplers();
	s_rpObjs.Init("SkyDI", m_rootSig, samplers.size(), samplers.data(), flags);

	for (int i = 0; i < (int)SHADERS::COUNT; i++)
	{
		m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
			s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[i]);
	}

	m_descTable = renderer.GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);
	CreateOutputs();

	memset(&m_cbTemporalResample, 0, sizeof(m_cbTemporalResample));
	memset(&m_cbSpatialResample, 0, sizeof(m_cbSpatialResample));
	memset(&m_cbDNSRTemporal, 0, sizeof(m_cbDNSRTemporal));
	memset(&m_cbDNSRSpatial, 0, sizeof(m_cbDNSRSpatial));
	m_cbTemporalResample.M_max = DefaultParamVals::TemporalM_max;
	m_cbTemporalResample.MinRoughnessResample = DefaultParamVals::MinRoughnessToResample;
	m_cbTemporalResample.PrefilterReservoirs = true;
	m_cbTemporalResample.CheckerboardTracing = true;
	m_cbSpatialResample.DoSpatialResampling = true;
	m_cbSpatialResample.MinRoughnessResample = DefaultParamVals::MinRoughnessToResample;
	m_cbDNSRTemporal.MaxTSPP_Diffuse = m_cbDNSRSpatial.MaxTSPP = DefaultParamVals::DNSRTspp_Diffuse;
	m_cbDNSRTemporal.MaxTSPP_Specular = m_cbDNSRSpatial.MaxTSPP = DefaultParamVals::DNSRTspp_Specular;
	m_cbDNSRTemporal.MinRoughnessResample = DefaultParamVals::MinRoughnessToResample;
	m_cbDNSRSpatial.MinRoughnessResample = DefaultParamVals::MinRoughnessToResample;
	m_cbDNSRTemporal.Denoise = m_cbDNSRSpatial.Denoise = true;
	m_cbDNSRSpatial.FilterDiffuse = true;
	m_cbDNSRSpatial.FilterSpecular = true;

	ParamVariant doTemporal;
	doTemporal.InitBool("Renderer", "Direct Lighting (Sky)", "TemporalResampling",
		fastdelegate::MakeDelegate(this, &SkyDI::DoTemporalResamplingCallback), m_doTemporalResampling);
	App::AddParam(doTemporal);

	ParamVariant doSpatial;
	doSpatial.InitBool("Renderer", "Direct Lighting (Sky)", "SpatialResampling",
		fastdelegate::MakeDelegate(this, &SkyDI::DoSpatialResamplingCallback), m_cbSpatialResample.DoSpatialResampling);
	App::AddParam(doSpatial);

	ParamVariant maxTemporalM;
	maxTemporalM.InitInt("Renderer", "Direct Lighting (Sky)", "MaxTemporalM",
		fastdelegate::MakeDelegate(this, &SkyDI::MaxTemporalMCallback),
		m_cbTemporalResample.M_max,		// val	
		1,								// min
		32,								// max
		1);								// step
	App::AddParam(maxTemporalM);

	ParamVariant checkerboarding;
	checkerboarding.InitBool("Renderer", "Direct Lighting (Sky)", "CheckerboardTrace",
		fastdelegate::MakeDelegate(this, &SkyDI::CheckerboardingCallback), m_cbTemporalResample.CheckerboardTracing);
	App::AddParam(checkerboarding);

	ParamVariant minRoughness;
	minRoughness.InitFloat("Renderer", "Direct Lighting (Sky)", "MinRoughnessToResample",
		fastdelegate::MakeDelegate(this, &SkyDI::MinRoughnessResampleCallback),
		m_cbTemporalResample.MinRoughnessResample,	// val	
		0,											// min
		1,											// max
		0.1f);										// step
	App::AddParam(minRoughness);

	ParamVariant prefilter;
	prefilter.InitBool("Renderer", "Direct Lighting (Sky)", "PrefilterReservoirs",
		fastdelegate::MakeDelegate(this, &SkyDI::SetReservoirPrefilteringEnablementCallback), m_cbTemporalResample.PrefilterReservoirs);
	App::AddParam(prefilter);

	ParamVariant denoise;
	denoise.InitBool("Renderer", "SkyDI Denoiser", "Enable",
		fastdelegate::MakeDelegate(this, &SkyDI::DoDenoisingCallback), m_cbDNSRTemporal.Denoise);
	App::AddParam(denoise);

	ParamVariant tsppDiffuse;
	tsppDiffuse.InitInt("Renderer", "SkyDI Denoiser", "TSPP_Diffuse",
		fastdelegate::MakeDelegate(this, &SkyDI::TsppDiffuseCallback),
		m_cbDNSRTemporal.MaxTSPP_Diffuse,			// val	
		1,											// min
		32,											// max
		1);											// step
	App::AddParam(tsppDiffuse);

	ParamVariant tsppSpecular;
	tsppSpecular.InitInt("Renderer", "SkyDI Denoiser", "TSPP_Specular",
		fastdelegate::MakeDelegate(this, &SkyDI::TsppSpecularCallback),
		m_cbDNSRTemporal.MaxTSPP_Specular,			// val	
		1,											// min
		32,											// max
		1);											// step
	App::AddParam(tsppSpecular);

	ParamVariant dnsrSpatialFilterDiffuse;
	dnsrSpatialFilterDiffuse.InitBool("Renderer", "SkyDI Denoiser", "SpatialFiltering (Diffuse)",
		fastdelegate::MakeDelegate(this, &SkyDI::DnsrSpatialFilterDiffuseCallback), m_cbDNSRSpatial.FilterDiffuse);
	App::AddParam(dnsrSpatialFilterDiffuse);

	ParamVariant dnsrSpatialFilterSpecular;
	dnsrSpatialFilterSpecular.InitBool("Renderer", "SkyDI Denoiser", "SpatialFiltering (Specular)",
		fastdelegate::MakeDelegate(this, &SkyDI::DnsrSpatialFilterSpecularCallback), m_cbDNSRSpatial.FilterSpecular);
	App::AddParam(dnsrSpatialFilterSpecular);

	App::AddShaderReloadHandler("SkyDI_Temporal", fastdelegate::MakeDelegate(this, &SkyDI::ReloadTemporalPass));
	App::AddShaderReloadHandler("SkyDI_Spatial", fastdelegate::MakeDelegate(this, &SkyDI::ReloadSpatialPass));
	App::AddShaderReloadHandler("SkyDI_DNSR_Temporal", fastdelegate::MakeDelegate(this, &SkyDI::ReloadDNSRTemporal));
	App::AddShaderReloadHandler("SkyDI_DNSR_Spatial", fastdelegate::MakeDelegate(this, &SkyDI::ReloadDNSRSpatial));

	m_isTemporalReservoirValid = false;
}

void SkyDI::Reset() noexcept
{
	if (IsInitialized())
	{
		s_rpObjs.Clear();

		App::RemoveShaderReloadHandler("SkyDI_Temporal");
		App::RemoveShaderReloadHandler("SkyDI_Spatial");
		App::RemoveShaderReloadHandler("SkyDI_DNSR_Temporal");
		App::RemoveShaderReloadHandler("SkyDI_DNSR_Spatial");
		App::RemoveParam("Renderer", "Direct Lighting (Sky)", "TemporalResampling");
		App::RemoveParam("Renderer", "Direct Lighting (Sky)", "SpatialResampling");
		App::RemoveParam("Renderer", "Direct Lighting (Sky)", "MaxTemporalM");
		App::RemoveParam("Renderer", "Direct Lighting (Sky)", "CheckerboardTrace");
		App::RemoveParam("Renderer", "Direct Lighting (Sky)", "MinRoughnessToResample");
		App::RemoveParam("Renderer", "Direct Lighting (Sky)", "PrefilterReservoirs");
		App::RemoveParam("Renderer", "SkyDI Denoiser", "Enable");
		App::RemoveParam("Renderer", "SkyDI Denoiser", "TSPP_Diffuse");
		App::RemoveParam("Renderer", "SkyDI Denoiser", "TSPP_Specular");
		App::RemoveParam("Renderer", "SkyDI Denoiser", "SpatialFiltering (Diffuse)");
		App::RemoveParam("Renderer", "SkyDI Denoiser", "SpatialFiltering (Specular)");
		
		for (int i = 0; i < 2; i++)
		{
			m_temporalReservoirs[i].ReservoirA.Reset();
			m_temporalReservoirs[i].ReservoirB.Reset();
			m_dnsrCache[i].Specular.Reset();
			m_dnsrCache[i].Diffuse.Reset();
		}

		m_spatialReservoir.ReservoirA.Reset();
		m_dnsrFinal.Reset();

		for (int i = 0; i < ZetaArrayLen(m_psos); i++)
			m_psos[i] = nullptr;

		m_descTable.Reset();
	}
}

void SkyDI::OnWindowResized() noexcept
{
	CreateOutputs();
	m_isTemporalReservoirValid = false;
}

void SkyDI::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();
	const int w = renderer.GetRenderWidth();
	const int h = renderer.GetRenderHeight();

	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

	// temporal resampling
	{
		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, SKY_DI_TEMPORAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, SKY_DI_TEMPORAL_GROUP_DIM_Y);

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SkyDI_Temporal");

		computeCmdList.PIXBeginEvent("SkyDI_Temporal");
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::TEMPORAL_RESAMPLE]);

		D3D12_RESOURCE_BARRIER barriers[2];

		// transition current temporal reservoir into write state
		barriers[0] = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalIdx].ReservoirA.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		barriers[1] = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalIdx].ReservoirB.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

		m_cbTemporalResample.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbTemporalResample.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbTemporalResample.NumGroupsInTile = SKY_DI_TEMPORAL_TILE_WIDTH * m_cbTemporalResample.DispatchDimY;
		m_cbTemporalResample.DoTemporalResampling = m_doTemporalResampling && m_isTemporalReservoirValid;
		m_cbTemporalResample.SampleIndex = (uint16_t)m_sampleIdx;

		auto srvAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV;
		auto srvBIdx = m_currTemporalIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV;
		auto uavAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_A_UAV : DESC_TABLE::TEMPORAL_RESERVOIR_0_A_UAV;
		auto uavBIdx = m_currTemporalIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_B_UAV : DESC_TABLE::TEMPORAL_RESERVOIR_0_B_UAV;

		m_cbTemporalResample.PrevTemporalReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
		m_cbTemporalResample.CurrTemporalReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
		m_cbTemporalResample.CurrTemporalReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);

		m_rootSig.SetRootConstants(0, sizeof(m_cbTemporalResample) / sizeof(DWORD), &m_cbTemporalResample);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();
	}

	// spatial resampling
	{
		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, SKY_DI_SPATIAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, SKY_DI_SPATIAL_GROUP_DIM_Y);

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::SPATIAL_RESAMPLE]);

		m_cbSpatialResample.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbSpatialResample.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbSpatialResample.NumGroupsInTile = SKY_DI_SPATIAL_TILE_WIDTH * m_cbSpatialResample.DispatchDimY;
		
		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SkyDI_Spatial");

		computeCmdList.PIXBeginEvent("SkyDI_Spatial");

		D3D12_RESOURCE_BARRIER barriers[3];

		// transition temporal reservoir into read state
		barriers[0] = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalIdx].ReservoirA.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barriers[1] = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalIdx].ReservoirB.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		// transition spatial reservoir into write state
		barriers[2] = Direct3DHelper::TransitionBarrier(m_spatialReservoir.ReservoirA.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

		auto srvAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV;
		auto srvBIdx = m_currTemporalIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV;
		auto uavAIdx = DESC_TABLE::SPATIAL_RESERVOIR_A_UAV;

		m_cbSpatialResample.InputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
		m_cbSpatialResample.InputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
		m_cbSpatialResample.OutputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);

		m_rootSig.SetRootConstants(0, sizeof(m_cbSpatialResample) / sizeof(DWORD), &m_cbSpatialResample);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();		
	}

	// denoiser - temporal
	{
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_TEMPORAL]);

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SkyDI_DNSR_Temporal");

		computeCmdList.PIXBeginEvent("SkyDI_DNSR_Temporal");

		D3D12_RESOURCE_BARRIER barriers[3];

		// transition spatial reservoir into read state
		barriers[0] = Direct3DHelper::TransitionBarrier(m_spatialReservoir.ReservoirA.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		// transition current denoiser caches into write
		barriers[1] = Direct3DHelper::TransitionBarrier(m_dnsrCache[m_currTemporalIdx].Diffuse.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		barriers[2] = Direct3DHelper::TransitionBarrier(m_dnsrCache[m_currTemporalIdx].Specular.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

		auto srvDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV : DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV;
		auto srvSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV : DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV;
		auto uavDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV : DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV;
		auto uavSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV : DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV;

		m_cbDNSRTemporal.InputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::SPATIAL_RESERVOIR_A_SRV);
		m_cbDNSRTemporal.PrevTemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvDiffuseIdx);
		m_cbDNSRTemporal.PrevTemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvSpecularIdx);
		m_cbDNSRTemporal.CurrTemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavDiffuseIdx);
		m_cbDNSRTemporal.CurrTemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavSpecularIdx);
		m_cbDNSRTemporal.FinalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::DNSR_FINAL_UAV);
		m_cbDNSRTemporal.IsTemporalCacheValid = m_isTemporalReservoirValid;

		m_rootSig.SetRootConstants(0, sizeof(m_cbDNSRTemporal) / sizeof(DWORD), &m_cbDNSRTemporal);
		m_rootSig.End(computeCmdList);

		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, SKY_DI_DNSR_TEMPORAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, SKY_DI_DNSR_TEMPORAL_GROUP_DIM_Y);
		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();
	}

	// denoiser - spatial
	{
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_SPATIAL]);

		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, SKY_DI_DNSR_SPATIAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, SKY_DI_DNSR_SPATIAL_GROUP_DIM_Y);

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SkyDI_DNSR_Spatial");

		computeCmdList.PIXBeginEvent("SkyDI_DNSR_Spatial");

		D3D12_RESOURCE_BARRIER barriers[2];

		// transition denoiser caches into read state
		barriers[0] = Direct3DHelper::TransitionBarrier(m_dnsrCache[m_currTemporalIdx].Diffuse.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barriers[1] = Direct3DHelper::TransitionBarrier(m_dnsrCache[m_currTemporalIdx].Specular.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

		auto srvDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV : DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV;
		auto srvSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV : DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV;

		m_cbDNSRSpatial.CurrTemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvDiffuseIdx);
		m_cbDNSRSpatial.CurrTemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvSpecularIdx);
		m_cbDNSRSpatial.FinalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::DNSR_FINAL_UAV);
		m_cbDNSRSpatial.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbDNSRSpatial.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbDNSRSpatial.NumGroupsInTile = SKY_DI_DNSR_SPATIAL_TILE_WIDTH * m_cbDNSRSpatial.DispatchDimY;

		m_rootSig.SetRootConstants(0, sizeof(m_cbDNSRSpatial) / sizeof(DWORD), &m_cbDNSRSpatial);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();
	}

	m_isTemporalReservoirValid = true;
	m_currTemporalIdx = 1 - m_currTemporalIdx;
	m_internalCounter++;

	// when checkerboarding, advance the sample index every other tracing frame
	if (!m_cbTemporalResample.CheckerboardTracing || (m_internalCounter & 0x1))
		m_sampleIdx = (m_sampleIdx + 1) & 31;
}

void SkyDI::CreateOutputs() noexcept
{
	auto& renderer = App::GetRenderer();

	auto func = [&renderer, this](Core::Texture& tex, DXGI_FORMAT f, const char* name, 
		DESC_TABLE srv, DESC_TABLE uav, D3D12_RESOURCE_STATES s = D3D12_RESOURCE_STATE_COMMON)
	{
		tex = renderer.GetGpuMemory().GetTexture2D(name,
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			f,
			s,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DHelper::CreateTexture2DSRV(tex, m_descTable.CPUHandle((int)srv));
		Direct3DHelper::CreateTexture2DUAV(tex, m_descTable.CPUHandle((int)uav));
	};

	// reservoir
	{
		// temporal reservoirs
		func(m_temporalReservoirs[0].ReservoirA, ResourceFormats::RESERVOIR_A, "SkyDI_TemporalReservoir_0_A",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_A_UAV);
		func(m_temporalReservoirs[0].ReservoirB, ResourceFormats::RESERVOIR_B, "SkyDI_TemporalReservoir_0_B",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_B_UAV);
		func(m_temporalReservoirs[1].ReservoirA, ResourceFormats::RESERVOIR_A, "SkyDI_TemporalReservoir_1_A",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_A_UAV);
		func(m_temporalReservoirs[1].ReservoirB, ResourceFormats::RESERVOIR_B, "SkyDI_TemporalReservoir_1_B",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_B_UAV);

		// spatial reservoir
		func(m_spatialReservoir.ReservoirA, ResourceFormats::RESERVOIR_A, "DI_SpatialReservoir_A",
			DESC_TABLE::SPATIAL_RESERVOIR_A_SRV, DESC_TABLE::SPATIAL_RESERVOIR_A_UAV);
	}

	// denoiser cache
	{
		func(m_dnsrCache[0].Diffuse, ResourceFormats::DNSR_TEMPORAL_CACHE, "SkyDI_DNSR_Diffuse_0",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV);
		func(m_dnsrCache[1].Diffuse, ResourceFormats::DNSR_TEMPORAL_CACHE, "SkyDI_DNSR_Diffuse_1",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV);
		func(m_dnsrCache[0].Specular, ResourceFormats::DNSR_TEMPORAL_CACHE, "SkyDI_DNSR_Specular_0",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV);
		func(m_dnsrCache[1].Specular, ResourceFormats::DNSR_TEMPORAL_CACHE, "SkyDI_DNSR_Specular_1",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV);

		m_dnsrFinal = renderer.GetGpuMemory().GetTexture2D("SkyDI_DNSR_Final",
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			ResourceFormats::DNSR_TEMPORAL_CACHE,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DHelper::CreateTexture2DUAV(m_dnsrFinal, m_descTable.CPUHandle((int)DESC_TABLE::DNSR_FINAL_UAV));
	}
}

void SkyDI::DoTemporalResamplingCallback(const Support::ParamVariant& p) noexcept
{
	m_doTemporalResampling = p.GetBool();
}

void SkyDI::DoSpatialResamplingCallback(const Support::ParamVariant& p) noexcept
{
	m_cbSpatialResample.DoSpatialResampling = p.GetBool();
}

void SkyDI::MaxTemporalMCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporalResample.M_max = (uint16_t)p.GetInt().m_val;
}

void SkyDI::CheckerboardingCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporalResample.CheckerboardTracing = p.GetBool();
	m_cbSpatialResample.CheckerboardTracing = p.GetBool();
}

void SkyDI::MinRoughnessResampleCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporalResample.MinRoughnessResample = p.GetFloat().m_val;
	m_cbSpatialResample.MinRoughnessResample = p.GetFloat().m_val;
	m_cbDNSRTemporal.MinRoughnessResample = p.GetFloat().m_val;
	m_cbDNSRSpatial.MinRoughnessResample = p.GetFloat().m_val;
}

void SkyDI::SetReservoirPrefilteringEnablementCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporalResample.PrefilterReservoirs = p.GetBool();
}

void SkyDI::DoDenoisingCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSRTemporal.Denoise = p.GetBool();
	m_cbDNSRSpatial.Denoise = p.GetBool();
}

void SkyDI::TsppDiffuseCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSRTemporal.MaxTSPP_Diffuse = (uint16_t)p.GetInt().m_val;
}

void SkyDI::TsppSpecularCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSRTemporal.MaxTSPP_Specular = (uint16_t)p.GetInt().m_val;
}

void SkyDI::DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSRSpatial.FilterDiffuse = p.GetBool();
}

void SkyDI::DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSRSpatial.FilterSpecular = p.GetBool();
}

void SkyDI::ReloadTemporalPass() noexcept
{
	const int i = (int)SHADERS::TEMPORAL_RESAMPLE;

	s_rpObjs.m_psoLib.Reload(i, "Sky\\DirectLighting\\SkyDI_Temporal.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void SkyDI::ReloadSpatialPass() noexcept
{
	const int i = (int)SHADERS::SPATIAL_RESAMPLE;

	s_rpObjs.m_psoLib.Reload(i, "Sky\\DirectLighting\\SkyDI_Spatial.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void SkyDI::ReloadDNSRTemporal() noexcept
{
	const int i = (int)SHADERS::DNSR_TEMPORAL;

	s_rpObjs.m_psoLib.Reload(i, "Sky\\DirectLighting\\SkyDI_DNSR_Temporal.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void SkyDI::ReloadDNSRSpatial() noexcept
{
	const int i = (int)SHADERS::DNSR_SPATIAL;

	s_rpObjs.m_psoLib.Reload(i, "Sky\\DirectLighting\\SkyDI_DNSR_SpatialFilter.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}
