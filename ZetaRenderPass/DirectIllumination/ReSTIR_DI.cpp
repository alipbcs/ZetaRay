#include "ReSTIR_DI.h"
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
// ReSTIR_DI
//--------------------------------------------------------------------------------------

ReSTIR_DI::ReSTIR_DI() noexcept
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
		GlobalResource::FRAME_CONSTANTS_BUFFER_NAME);

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
}

ReSTIR_DI::~ReSTIR_DI() noexcept
{
	Reset();
}

void ReSTIR_DI::Init() noexcept
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
	s_rpObjs.Init("ReSTIR_DI", m_rootSig, samplers.size(), samplers.data(), flags);

	for (int i = 0; i < (int)SHADERS::COUNT; i++)
	{
		m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
			s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[i]);
	}

	m_descTable = renderer.GetCbvSrvUavDescriptorHeapGpu().Allocate((int)DESC_TABLE::COUNT);
	CreateOutputs();

	memset(&m_cbTemporalResample, 0, sizeof(m_cbTemporalResample));
	memset(&m_cbSpatialResample, 0, sizeof(m_cbSpatialResample));
	memset(&m_cbDNSRTemporal, 0, sizeof(m_cbDNSRTemporal));
	memset(&m_cbDNSRSpatial, 0, sizeof(m_cbDNSRSpatial));
	m_cbTemporalResample.DoTemporalResampling = m_doTemporalResampling;
	m_cbTemporalResample.M_max = DefaultParamVals::TemporalM_max;
	m_cbTemporalResample.MinRoughnessResample = DefaultParamVals::MinRoughnessToResample;
	m_cbTemporalResample.PrefilterReservoirs = true;
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
	doTemporal.InitBool("Renderer", "ReSTIR DI", "TemporalResampling",
		fastdelegate::MakeDelegate(this, &ReSTIR_DI::DoTemporalResamplingCallback), m_cbTemporalResample.DoTemporalResampling);
	App::AddParam(doTemporal);

	ParamVariant doSpatial;
	doSpatial.InitBool("Renderer", "ReSTIR DI", "SpatialResampling",
		fastdelegate::MakeDelegate(this, &ReSTIR_DI::DoSpatialResamplingCallback), m_cbSpatialResample.DoSpatialResampling);
	App::AddParam(doSpatial);

	ParamVariant maxTemporalM;
	maxTemporalM.InitInt("Renderer", "ReSTIR DI", "MaxTemporalM",
		fastdelegate::MakeDelegate(this, &ReSTIR_DI::MaxTemporalMCallback),
		m_cbTemporalResample.M_max,		// val	
		1,								// min
		32,								// max
		1);								// step
	App::AddParam(maxTemporalM);

	ParamVariant checkerboarding;
	checkerboarding.InitBool("Renderer", "ReSTIR DI", "CheckerboardTrace",
		fastdelegate::MakeDelegate(this, &ReSTIR_DI::CheckerboardingCallback), m_cbTemporalResample.CheckerboardTracing);
	App::AddParam(checkerboarding);

	ParamVariant minRoughness;
	minRoughness.InitFloat("Renderer", "ReSTIR DI", "MinRoughnessToResample",
		fastdelegate::MakeDelegate(this, &ReSTIR_DI::MinRoughnessResampleCallback),
		m_cbTemporalResample.MinRoughnessResample,	// val	
		0,											// min
		1,											// max
		0.1);										// step
	App::AddParam(minRoughness);

	ParamVariant prefilter;
	prefilter.InitBool("Renderer", "ReSTIR DI", "PrefilterReservoirs",
		fastdelegate::MakeDelegate(this, &ReSTIR_DI::SetReservoirPrefilteringEnablementCallback), m_cbTemporalResample.PrefilterReservoirs);
	App::AddParam(prefilter);

	ParamVariant denoise;
	denoise.InitBool("Renderer", "DirectDenoiser", "Enable",
		fastdelegate::MakeDelegate(this, &ReSTIR_DI::DoDenoisingCallback), m_cbDNSRTemporal.Denoise);
	App::AddParam(denoise);

	ParamVariant tsppDiffuse;
	tsppDiffuse.InitInt("Renderer", "DirectDenoiser", "TSPP_Diffuse",
		fastdelegate::MakeDelegate(this, &ReSTIR_DI::TsppDiffuseCallback),
		m_cbDNSRTemporal.MaxTSPP_Diffuse,			// val	
		1,											// min
		32,											// max
		1);											// step
	App::AddParam(tsppDiffuse);

	ParamVariant tsppSpecular;
	tsppSpecular.InitInt("Renderer", "DirectDenoiser", "TSPP_Specular",
		fastdelegate::MakeDelegate(this, &ReSTIR_DI::TsppSpecularCallback),
		m_cbDNSRTemporal.MaxTSPP_Specular,			// val	
		1,											// min
		32,											// max
		1);											// step
	App::AddParam(tsppSpecular);

	ParamVariant dnsrSpatialFilterDiffuse;
	dnsrSpatialFilterDiffuse.InitBool("Renderer", "DirectDenoiser", "SpatialFiltering (Diffuse)",
		fastdelegate::MakeDelegate(this, &ReSTIR_DI::DnsrSpatialFilterDiffuseCallback), m_cbDNSRSpatial.FilterDiffuse);
	App::AddParam(dnsrSpatialFilterDiffuse);

	ParamVariant dnsrSpatialFilterSpecular;
	dnsrSpatialFilterSpecular.InitBool("Renderer", "DirectDenoiser", "SpatialFiltering (Specular)",
		fastdelegate::MakeDelegate(this, &ReSTIR_DI::DnsrSpatialFilterSpecularCallback), m_cbDNSRSpatial.FilterSpecular);
	App::AddParam(dnsrSpatialFilterSpecular);

	App::AddShaderReloadHandler("ReSTIR_DI_Temporal", fastdelegate::MakeDelegate(this, &ReSTIR_DI::ReloadTemporalPass));
	App::AddShaderReloadHandler("ReSTIR_DI_Spatial", fastdelegate::MakeDelegate(this, &ReSTIR_DI::ReloadSpatialPass));
	App::AddShaderReloadHandler("DirectDNSR_Temporal", fastdelegate::MakeDelegate(this, &ReSTIR_DI::ReloadDNSRTemporal));
	App::AddShaderReloadHandler("DirectDNSR_Spatial", fastdelegate::MakeDelegate(this, &ReSTIR_DI::ReloadDNSRSpatial));

	m_isTemporalReservoirValid = false;
}

void ReSTIR_DI::Reset() noexcept
{
	if (IsInitialized())
	{
		s_rpObjs.Clear();

		App::RemoveShaderReloadHandler("ReSTIR_DI_Temporal");
		App::RemoveShaderReloadHandler("ReSTIR_DI_Spatial");
		App::RemoveShaderReloadHandler("DirectDNSR_Temporal");
		App::RemoveShaderReloadHandler("DirectDNSR_Spatial");
		App::RemoveParam("Renderer", "ReSTIR DI", "TemporalResampling");
		App::RemoveParam("Renderer", "ReSTIR DI", "SpatialResampling");
		App::RemoveParam("Renderer", "ReSTIR DI", "MaxTemporalM");
		App::RemoveParam("Renderer", "ReSTIR DI", "CheckerboardTrace");
		App::RemoveParam("Renderer", "ReSTIR DI", "MinRoughnessToResample");
		App::RemoveParam("Renderer", "ReSTIR DI", "PrefilterReservoirs");
		App::RemoveParam("Renderer", "DirectDenoiser", "Enable");
		App::RemoveParam("Renderer", "DirectDenoiser", "TSPP_Diffuse");
		App::RemoveParam("Renderer", "DirectDenoiser", "TSPP_Specular");
		App::RemoveParam("Renderer", "DirectDenoiser", "SpatialFiltering (Diffuse)");
		App::RemoveParam("Renderer", "DirectDenoiser", "SpatialFiltering (Specular)");
		
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

void ReSTIR_DI::OnWindowResized() noexcept
{
	CreateOutputs();
	m_isTemporalReservoirValid = false;
}

void ReSTIR_DI::Render(CommandList& cmdList) noexcept
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
		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, RDI_TEMPORAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, RDI_TEMPORAL_GROUP_DIM_Y);

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_DI_Temporal");

		computeCmdList.PIXBeginEvent("ReSTIR_DI_Temporal");
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
		m_cbTemporalResample.NumGroupsInTile = RDI_TEMPORAL_TILE_WIDTH * m_cbTemporalResample.DispatchDimY;
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
		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, RDI_SPATIAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, RDI_SPATIAL_GROUP_DIM_Y);

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::SPATIAL_RESAMPLE]);

		m_cbSpatialResample.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbSpatialResample.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbSpatialResample.NumGroupsInTile = RDI_SPATIAL_TILE_WIDTH * m_cbSpatialResample.DispatchDimY;
		
		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_DI_Spatial");

		computeCmdList.PIXBeginEvent("ReSTIR_DI_Spatial");

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
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "DirectDNSR_Temporal");

		computeCmdList.PIXBeginEvent("DirectDNSR_Temporal");

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

		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, DIRECT_DNSR_TEMPORAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, DIRECT_DNSR_TEMPORAL_GROUP_DIM_Y);
		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();
	}

	// denoiser - spatial
	{
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_SPATIAL]);

		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, DIRECT_DNSR_SPATIAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, DIRECT_DNSR_SPATIAL_GROUP_DIM_Y);

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "DirectDNSR_Spatial");

		computeCmdList.PIXBeginEvent("DirectDNSR_Spatial");

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
		m_cbDNSRSpatial.NumGroupsInTile = DIRECT_DNSR_SPATIAL_TILE_WIDTH * m_cbDNSRSpatial.DispatchDimY;

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

void ReSTIR_DI::CreateOutputs() noexcept
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
		func(m_temporalReservoirs[0].ReservoirA, ResourceFormats::RESERVOIR_A, "DI_TemporalReservoir_0_A",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_A_UAV);
		func(m_temporalReservoirs[0].ReservoirB, ResourceFormats::RESERVOIR_B, "DI_TemporalReservoir_0_B",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_B_UAV);
		func(m_temporalReservoirs[1].ReservoirA, ResourceFormats::RESERVOIR_A, "DI_TemporalReservoir_1_A",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_A_UAV);
		func(m_temporalReservoirs[1].ReservoirB, ResourceFormats::RESERVOIR_B, "DI_TemporalReservoir_1_B",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_B_UAV);

		// spatial reservoir
		func(m_spatialReservoir.ReservoirA, ResourceFormats::RESERVOIR_A, "DI_SpatialReservoir_A",
			DESC_TABLE::SPATIAL_RESERVOIR_A_SRV, DESC_TABLE::SPATIAL_RESERVOIR_A_UAV);
	}

	// denoiser cache
	{
		func(m_dnsrCache[0].Diffuse, ResourceFormats::DNSR_TEMPORAL_CACHE, "DirectDNSR_Diffuse_0",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV);
		func(m_dnsrCache[1].Diffuse, ResourceFormats::DNSR_TEMPORAL_CACHE, "DirectDNSR_Diffuse_1",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV);
		func(m_dnsrCache[0].Specular, ResourceFormats::DNSR_TEMPORAL_CACHE, "DirectDNSR_Specular_0",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV);
		func(m_dnsrCache[1].Specular, ResourceFormats::DNSR_TEMPORAL_CACHE, "DirectDNSR_Specular_1",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV);

		m_dnsrFinal = renderer.GetGpuMemory().GetTexture2D("DirectDNSR_Final",
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			ResourceFormats::DNSR_TEMPORAL_CACHE,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DHelper::CreateTexture2DUAV(m_dnsrFinal, m_descTable.CPUHandle((int)DESC_TABLE::DNSR_FINAL_UAV));
	}
}

void ReSTIR_DI::DoTemporalResamplingCallback(const Support::ParamVariant& p) noexcept
{
	m_doTemporalResampling = p.GetBool();
}

void ReSTIR_DI::DoSpatialResamplingCallback(const Support::ParamVariant& p) noexcept
{
	m_cbSpatialResample.DoSpatialResampling = p.GetBool();
}

void ReSTIR_DI::MaxTemporalMCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporalResample.M_max = (uint16_t)p.GetInt().m_val;
}

void ReSTIR_DI::CheckerboardingCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporalResample.CheckerboardTracing = p.GetBool();
	m_cbSpatialResample.CheckerboardTracing = p.GetBool();
}

void ReSTIR_DI::MinRoughnessResampleCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporalResample.MinRoughnessResample = p.GetFloat().m_val;
	m_cbSpatialResample.MinRoughnessResample = p.GetFloat().m_val;
	m_cbDNSRTemporal.MinRoughnessResample = p.GetFloat().m_val;
	m_cbDNSRSpatial.MinRoughnessResample = p.GetFloat().m_val;
}

void ReSTIR_DI::SetReservoirPrefilteringEnablementCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporalResample.PrefilterReservoirs = p.GetBool();
}

void ReSTIR_DI::DoDenoisingCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSRTemporal.Denoise = p.GetBool();
	m_cbDNSRSpatial.Denoise = p.GetBool();
}

void ReSTIR_DI::TsppDiffuseCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSRTemporal.MaxTSPP_Diffuse = (uint16_t)p.GetInt().m_val;
}

void ReSTIR_DI::TsppSpecularCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSRTemporal.MaxTSPP_Specular = (uint16_t)p.GetInt().m_val;
}

void ReSTIR_DI::DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSRSpatial.FilterDiffuse = p.GetBool();
}

void ReSTIR_DI::DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSRSpatial.FilterSpecular = p.GetBool();
}

void ReSTIR_DI::ReloadTemporalPass() noexcept
{
	const int i = (int)SHADERS::TEMPORAL_RESAMPLE;

	s_rpObjs.m_psoLib.Reload(i, "DirectIllumination\\ReSTIR_DI_Temporal.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void ReSTIR_DI::ReloadSpatialPass() noexcept
{
	const int i = (int)SHADERS::SPATIAL_RESAMPLE;

	s_rpObjs.m_psoLib.Reload(i, "DirectIllumination\\ReSTIR_DI_Spatial.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void ReSTIR_DI::ReloadDNSRTemporal() noexcept
{
	const int i = (int)SHADERS::DNSR_TEMPORAL;

	s_rpObjs.m_psoLib.Reload(i, "DirectIllumination\\DirectDNSR_Temporal.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void ReSTIR_DI::ReloadDNSRSpatial() noexcept
{
	const int i = (int)SHADERS::DNSR_SPATIAL;

	s_rpObjs.m_psoLib.Reload(i, "DirectIllumination\\DirectDNSR_SpatialFilter.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}
