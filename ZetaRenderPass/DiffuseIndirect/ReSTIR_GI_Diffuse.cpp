#include "ReSTIR_GI_Diffuse.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <RayTracing/Sampler.h>
#include <Support/Param.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::RT;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// ReSTIR_GI_Diffuse
//--------------------------------------------------------------------------------------

ReSTIR_GI_Diffuse::ReSTIR_GI_Diffuse()
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

	// material buffer
	m_rootSig.InitAsBufferSRV(3,						// root idx
		1,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		GlobalResource::MATERIAL_BUFFER);

	// Owen-Scrambled Sobol Sequence
	m_rootSig.InitAsBufferSRV(4,						// root idx
		3,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SOBOL_SEQ_32);

	// scrambling tile
	m_rootSig.InitAsBufferSRV(5,						// root idx
		4,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE_32);

	// ranking tile
	m_rootSig.InitAsBufferSRV(6,						// root idx
		5,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE_32);

	// mesh buffer
	m_rootSig.InitAsBufferSRV(7,						// root idx
		6,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		GlobalResource::RT_FRAME_MESH_INSTANCES);
	
	// scene VB
	m_rootSig.InitAsBufferSRV(8,						// root idx
		7,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		GlobalResource::SCENE_VERTEX_BUFFER);

	// scene IB
	m_rootSig.InitAsBufferSRV(9,						// root idx
		8,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		GlobalResource::SCENE_INDEX_BUFFER);
}

ReSTIR_GI_Diffuse::~ReSTIR_GI_Diffuse()
{
	Reset();
}

void ReSTIR_GI_Diffuse::Init()
{
	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	auto samplers = App::GetRenderer().GetStaticSamplers();
	s_rpObjs.Init("ReSTIR_GI_Diffuse", m_rootSig, samplers.size(), samplers.data(), flags);

	for (int i = 0; i < (int)SHADERS::COUNT; i++)
	{
		m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
			s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[i]);
	}

	m_descTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);
	CreateOutputs();

	memset(&m_cbRGITemporal, 0, sizeof(m_cbRGITemporal));
	memset(&m_cbRGISpatial, 0, sizeof(m_cbRGISpatial));
	memset(&m_cbDNSRTemporal, 0, sizeof(m_cbDNSRTemporal));
	memset(&m_cbDNSRSpatial, 0, sizeof(m_cbDNSRSpatial));

	m_cbRGITemporal.DoTemporalResampling = true;
	m_cbRGITemporal.PdfCorrection = m_cbRGISpatial.PdfCorrection = true;
	m_cbRGITemporal.FrameCounter = 0;
	m_cbRGITemporal.CheckerboardTracing = true;
	m_cbRGISpatial.NormalExp = DefaultParamVals::RGINormalExp;
	m_cbRGISpatial.DoSpatialResampling = true;
	m_cbDNSRTemporal.IsTemporalCacheValid = false;
	m_cbDNSRTemporal.MaxTspp = m_cbDNSRSpatial.MaxTspp = DefaultParamVals::DNSRMaxTSPP;
	m_cbDNSRSpatial.NormalExp = DefaultParamVals::EdgeStoppingNormalExp;
	m_cbDNSRSpatial.MinFilterRadius = DefaultParamVals::DNSRMinFilterRadius;
	m_cbDNSRSpatial.MaxFilterRadius = DefaultParamVals::DNSRMaxFilterRadius;

	ParamVariant validationT;
	validationT.InitInt("Renderer", "ReSTIR GI (Diffuse)", "Validation Period",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Diffuse::ValidationPeriodCallback),
		DefaultParamVals::ValidationPeriod,		// val	
		0,										// min
		10,										// max
		1);										// step
	App::AddParam(validationT);

	ParamVariant doTemporal;
	doTemporal.InitBool("Renderer", "ReSTIR GI (Diffuse)", "Temporal Resampling",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Diffuse::DoTemporalResamplingCallback), m_cbRGITemporal.DoTemporalResampling);
	App::AddParam(doTemporal);

	ParamVariant doSpatial;
	doSpatial.InitBool("Renderer", "ReSTIR GI (Diffuse)", "Spatial Resampling",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Diffuse::DoSpatialResamplingCallback), m_cbRGISpatial.DoSpatialResampling);
	App::AddParam(doSpatial);

	ParamVariant checkerboard;
	checkerboard.InitBool("Renderer", "ReSTIR GI (Diffuse)", "Checkerboard Tracing",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Diffuse::CheckerboardTracingCallback), m_cbRGITemporal.CheckerboardTracing);
	App::AddParam(checkerboard);

	//ParamVariant maxTSPP;
	//maxTSPP.InitInt("Renderer", "Diffuse Denoiser", "TSPP_max", fastdelegate::MakeDelegate(this, &ReSTIR_GI_Diffuse::DNSRMaxTSPPCallback),
	//	DefaultParamVals::DNSRMaxTSPP,	// val
	//	1,								// min
	//	32,								// max
	//	1);								// step
	//App::AddParam(maxTSPP);

	//ParamVariant minDNSRRadius;
	//minDNSRRadius.InitInt("Renderer", "Diffuse Denoiser", "Min Radius", fastdelegate::MakeDelegate(this, &ReSTIR_GI_Diffuse::DNSRMinFilterRadiusCallback),
	//	m_cbDNSRSpatial.MinFilterRadius,	// val
	//	1,									// min
	//	32,									// max
	//	1);									// step
	//App::AddParam(minDNSRRadius);

	//ParamVariant maxDNSRRadius;
	//maxDNSRRadius.InitInt("Renderer", "Diffuse Denoiser", "Max Radius", fastdelegate::MakeDelegate(this, &ReSTIR_GI_Diffuse::DNSRMaxFilterRadiusCallback),
	//	m_cbDNSRSpatial.MaxFilterRadius,	// val
	//	1,									// min
	//	64,									// max
	//	1);									// step
	//App::AddParam(maxDNSRRadius);

	//App::AddShaderReloadHandler("ReSTIR_GI_Diffuse_Temporal", fastdelegate::MakeDelegate(this, &ReSTIR_GI_Diffuse::ReloadRGITemporalPass));
	//App::AddShaderReloadHandler("ReSTIR_GI_Diffuse_Spatial", fastdelegate::MakeDelegate(this, &ReSTIR_GI_Diffuse::ReloadRGISpatialPass));
	//App::AddShaderReloadHandler("ReSTIR_GI_Diffuse_Validation", fastdelegate::MakeDelegate(this, &ReSTIR_GI_Diffuse::ReloadValidationPass));
	//App::AddShaderReloadHandler("DiffuseDNSR_Temporal", fastdelegate::MakeDelegate(this, &ReSTIR_GI_Diffuse::ReloadDNSRTemporalPass));
	//App::AddShaderReloadHandler("DiffuseDNSR_SpatialFilter", fastdelegate::MakeDelegate(this, &ReSTIR_GI_Diffuse::ReloadDNSRSpatialPass));

	m_validationPeriod = DefaultParamVals::ValidationPeriod;
	m_isTemporalReservoirValid = false;
}

void ReSTIR_GI_Diffuse::Reset()
{
	if (IsInitialized())
	{
		s_rpObjs.Clear();
		
		for (int i = 0; i < 2; i++)
		{
			m_temporalReservoirs[i].ReservoirA.Reset();
			m_temporalReservoirs[i].ReservoirB.Reset();
			m_temporalReservoirs[i].ReservoirC.Reset();
			m_spatialReservoirs[i].ReservoirA.Reset();
			m_spatialReservoirs[i].ReservoirB.Reset();
			m_spatialReservoirs[i].ReservoirC.Reset();
			m_temporalCache[i].Reset();
		}

		//for (int i = 0; i < ZetaArrayLen(m_psos); i++)
		//	m_psos[i] = nullptr;

		m_descTable.Reset();
	}
}

void ReSTIR_GI_Diffuse::OnWindowResized()
{
	CreateOutputs();
	m_cbDNSRTemporal.IsTemporalCacheValid = false;
	m_cbRGITemporal.IsTemporalReservoirValid = false;
}

void ReSTIR_GI_Diffuse::Render(CommandList& cmdList)
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();
	const uint32_t w = renderer.GetRenderWidth();
	const uint32_t h = renderer.GetRenderWidth();

	const bool isTraceFrame = m_validationPeriod == 0 || m_validationFrame != 0;

	// temporal resampling/validation
	{
		const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RGI_DIFF_TEMPORAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RGI_DIFF_TEMPORAL_GROUP_DIM_Y);

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_GI_Diffuse_Temporal");

		if (isTraceFrame)
		{
			computeCmdList.PIXBeginEvent("ReSTIR_GI_Diffuse_Temporal");
			computeCmdList.SetPipelineState(m_psos[(int)SHADERS::TEMPORAL_PASS]);
		}
		else
		{
			computeCmdList.PIXBeginEvent("ReSTIR_GI_Diffuse_Validation");
			computeCmdList.SetPipelineState(m_psos[(int)SHADERS::VALIDATION]);
		}

		computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

		computeCmdList.ResourceBarrier(m_tsppAdjustment.Resource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		m_cbRGITemporal.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbRGITemporal.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbRGITemporal.IsTemporalReservoirValid = m_isTemporalReservoirValid;
		m_cbRGITemporal.NumGroupsInTile = RGI_DIFF_TEMPORAL_TILE_WIDTH * m_cbRGITemporal.DispatchDimY;
		m_cbRGITemporal.SampleIndex = (uint16_t)m_sampleIdx;
		m_cbRGITemporal.FrameCounter = m_internalCounter;

		auto srvAIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV;
		auto srvBIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV;
		auto srvCIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_0_C_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_1_C_SRV;
		auto uavAIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_A_UAV : DESC_TABLE::TEMPORAL_RESERVOIR_0_A_UAV;
		auto uavBIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_B_UAV : DESC_TABLE::TEMPORAL_RESERVOIR_0_B_UAV;
		auto uavCIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_C_UAV : DESC_TABLE::TEMPORAL_RESERVOIR_0_C_UAV;

		m_cbRGITemporal.PrevTemporalReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
		m_cbRGITemporal.PrevTemporalReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
		m_cbRGITemporal.PrevTemporalReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvCIdx);
		m_cbRGITemporal.CurrTemporalReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
		m_cbRGITemporal.CurrTemporalReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);
		m_cbRGITemporal.CurrTemporalReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavCIdx);
		m_cbRGITemporal.TsppAdjustment_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::TSPP_ADJUSTMENT_UAV);

		m_rootSig.SetRootConstants(0, sizeof(cb_RGI_Diff_Temporal) / sizeof(DWORD), &m_cbRGITemporal);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();
	}

	// spatial resampling
	{
		const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RGI_DIFF_SPATIAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RGI_DIFF_SPATIAL_GROUP_DIM_Y);

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::SPATIAL_PASS]);

		m_cbRGISpatial.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbRGISpatial.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbRGISpatial.NumGroupsInTile = RGI_DIFF_SPATIAL_TILE_WIDTH * m_cbRGISpatial.DispatchDimY;

		{
			// record the timestamp prior to execution
			const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_GI_Diffuse_Spatial_1");

			computeCmdList.PIXBeginEvent("ReSTIR_GI_Diffuse_Spatial_1");

			// transition temporal reservoir into read state
			D3D12_RESOURCE_BARRIER barriers[6];
			int curr = 0;
			barriers[curr++] = Direct3DUtil::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirA.Resource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			barriers[curr++] = Direct3DUtil::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirB.Resource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			barriers[curr++] = Direct3DUtil::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirC.Resource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			// transition spatial reservoir 0 into write state
			barriers[curr++] = Direct3DUtil::TransitionBarrier(m_spatialReservoirs[0].ReservoirA.Resource(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			barriers[curr++] = Direct3DUtil::TransitionBarrier(m_spatialReservoirs[0].ReservoirB.Resource(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			barriers[curr++] = Direct3DUtil::TransitionBarrier(m_spatialReservoirs[0].ReservoirC.Resource(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			Assert(curr == ZetaArrayLen(barriers), "bug");
			computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

			auto srvAIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV;
			auto srvBIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV;
			auto srvCIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_C_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_0_C_SRV;
			auto uavAIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_A_UAV;
			auto uavBIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_B_UAV;
			auto uavCIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_C_UAV;

			m_cbRGISpatial.InputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
			m_cbRGISpatial.InputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
			m_cbRGISpatial.InputReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvCIdx);
			m_cbRGISpatial.OutputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
			m_cbRGISpatial.OutputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);
			m_cbRGISpatial.OutputReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavCIdx);
			m_cbRGISpatial.IsFirstPass = true;

			m_rootSig.SetRootConstants(0, sizeof(cb_RGI_Diff_Spatial) / sizeof(DWORD), &m_cbRGISpatial);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

			// record the timestamp after execution
			gpuTimer.EndQuery(computeCmdList, queryIdx);

			cmdList.PIXEndEvent();
		}

		{
			// record the timestamp prior to execution
			const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_GI_Diffuse_Spatial_2");

			computeCmdList.PIXBeginEvent("ReSTIR_GI_Diffuse_Spatial_2");

			// transition spatial reservoir into read state
			D3D12_RESOURCE_BARRIER barrier0 = Direct3DUtil::TransitionBarrier(m_spatialReservoirs[0].ReservoirA.Resource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			D3D12_RESOURCE_BARRIER barrier1 = Direct3DUtil::TransitionBarrier(m_spatialReservoirs[0].ReservoirB.Resource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			D3D12_RESOURCE_BARRIER barrier2 = Direct3DUtil::TransitionBarrier(m_spatialReservoirs[0].ReservoirC.Resource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			D3D12_RESOURCE_BARRIER inBarriers[] = { barrier0, barrier1, barrier2 };
			computeCmdList.ResourceBarrier(inBarriers, ZetaArrayLen(inBarriers));

			auto srvAIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_A_SRV;
			auto srvBIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_B_SRV;
			auto srvCIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_C_SRV;
			auto uavAIdx = DESC_TABLE::SPATIAL_RESERVOIR_1_A_UAV;
			auto uavBIdx = DESC_TABLE::SPATIAL_RESERVOIR_1_B_UAV;
			auto uavCIdx = DESC_TABLE::SPATIAL_RESERVOIR_1_C_UAV;

			m_cbRGISpatial.InputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
			m_cbRGISpatial.InputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
			m_cbRGISpatial.InputReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvCIdx);
			m_cbRGISpatial.OutputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
			m_cbRGISpatial.OutputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);
			m_cbRGISpatial.OutputReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavCIdx);
			m_cbRGISpatial.IsFirstPass = false;

			m_rootSig.SetRootConstants(0, sizeof(cb_RGI_Diff_Spatial) / sizeof(DWORD), &m_cbRGISpatial);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

			// record the timestamp after execution
			gpuTimer.EndQuery(computeCmdList, queryIdx);

			cmdList.PIXEndEvent();
		}
	}

	const int initialDNSRTemporalIdx = m_currDNSRTemporalIdx;

	// denoiser temporal pass
	{
		computeCmdList.PIXBeginEvent("DiffuseDNSR_Temporal");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "DiffuseDNSR_Temporal");

		computeCmdList.SetPipelineState(m_psos[(uint32_t)SHADERS::DIFFUSE_DNSR_TEMPORAL]);

		computeCmdList.ResourceBarrier(m_tsppAdjustment.Resource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		const int temporalCacheSRV = m_currDNSRTemporalIdx == 1 ? (int)DESC_TABLE::TEMPORAL_CACHE_A_SRV :
			(int)DESC_TABLE::TEMPORAL_CACHE_B_SRV;
		const int temporalCacheUAV = m_currDNSRTemporalIdx == 1 ? (int)DESC_TABLE::TEMPORAL_CACHE_B_UAV :
			(int)DESC_TABLE::TEMPORAL_CACHE_A_UAV;

		m_cbDNSRTemporal.InputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::SPATIAL_RESERVOIR_1_A_SRV);
		m_cbDNSRTemporal.InputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::SPATIAL_RESERVOIR_1_B_SRV);
		m_cbDNSRTemporal.PrevTemporalCacheDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheSRV);
		m_cbDNSRTemporal.CurrTemporalCacheDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheUAV);
		m_cbDNSRTemporal.TsppAdjustmentDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::TSPP_ADJUSTMENT_SRV);
		m_cbDNSRTemporal.IsTemporalCacheValid = m_isTemporalReservoirValid;

		m_rootSig.SetRootConstants(0, sizeof(cbDiffuseDNSRTemporal) / sizeof(DWORD), &m_cbDNSRTemporal);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(CeilUnsignedIntDiv(w, DiffuseDNSR_TEMPORAL_THREAD_GROUP_SIZE_X),
			CeilUnsignedIntDiv(h, DiffuseDNSR_TEMPORAL_THREAD_GROUP_SIZE_Y),
			1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	// denoiser spatial filter
	{
		computeCmdList.PIXBeginEvent("DiffuseDNSR_SpatialFilter");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "DiffuseDNSR_SpatialFilter");

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DIFFUSE_DNSR_SPATIAL]);

		const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_X);
		const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_Y);

		m_cbDNSRSpatial.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbDNSRSpatial.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbDNSRSpatial.NumGroupsInTile = DiffuseDNSR_SPATIAL_TILE_WIDTH * m_cbDNSRSpatial.DispatchDimY;
		m_cbDNSRSpatial.NumPasses = (uint16_t)m_numDNSRSpatialFilterPasses;

		for (int i = 0; i < m_numDNSRSpatialFilterPasses; i++)
		{
			D3D12_RESOURCE_BARRIER barriers[2];
			barriers[0] = Direct3DUtil::TransitionBarrier(m_temporalCache[1 - m_currDNSRTemporalIdx].Resource(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			barriers[1] = Direct3DUtil::TransitionBarrier(m_temporalCache[m_currDNSRTemporalIdx].Resource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));
			
			// swap temporal caches
			m_currDNSRTemporalIdx = 1 - m_currDNSRTemporalIdx;

			uint32_t prevTemporalCacheSRV = m_currDNSRTemporalIdx == 1 ? (uint32_t)DESC_TABLE::TEMPORAL_CACHE_A_SRV :
				(uint32_t)DESC_TABLE::TEMPORAL_CACHE_B_SRV;
			uint32_t nextTemporalCacheUAV = m_currDNSRTemporalIdx == 1 ? (uint32_t)DESC_TABLE::TEMPORAL_CACHE_B_UAV :
				(uint32_t)DESC_TABLE::TEMPORAL_CACHE_A_UAV;

			m_cbDNSRSpatial.TemporalCacheInDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(prevTemporalCacheSRV);
			m_cbDNSRSpatial.TemporalCacheOutDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(nextTemporalCacheUAV);
			//m_cbSpatialFilter.Step = 1 << (m_numSpatialFilterPasses - 1 - i);
			//m_cbSpatialFilter.FilterRadiusScale = (float)(1 << (m_numSpatialFilterPasses - 1 - i));
			m_cbDNSRSpatial.FilterRadiusScale = (float)(1 << i);
			m_cbDNSRSpatial.CurrPass = (uint16_t)i;

			m_rootSig.SetRootConstants(0, sizeof(m_cbDNSRSpatial) / sizeof(DWORD), &m_cbDNSRSpatial);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
		}

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	// restore the initial state

	// [hack] render graph is unaware of renderpass-internal transitions. Restore the initial state to avoid
	// render graph and actual state getting out of sync
	{
		D3D12_RESOURCE_BARRIER restoreBarriers[3 + 2];
		int curr = 0;

		// temporal reservoirs
		restoreBarriers[curr++] = Direct3DUtil::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirA.Resource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		restoreBarriers[curr++] = Direct3DUtil::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirB.Resource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		restoreBarriers[curr++] = Direct3DUtil::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirC.Resource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		// dnsr temporal cache
		if (initialDNSRTemporalIdx != m_currDNSRTemporalIdx)
		{
			restoreBarriers[curr++] = Direct3DUtil::TransitionBarrier(m_temporalCache[initialDNSRTemporalIdx].Resource(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			restoreBarriers[curr++] = Direct3DUtil::TransitionBarrier(m_temporalCache[1 - initialDNSRTemporalIdx].Resource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}

		computeCmdList.ResourceBarrier(restoreBarriers, curr);
	}

	// when there's no spatial filtering
	m_currDNSRTemporalIdx = m_numDNSRSpatialFilterPasses == 0 ? 1 - m_currDNSRTemporalIdx : m_currDNSRTemporalIdx;

	if (!m_isTemporalReservoirValid)
		m_isTemporalReservoirValid = !m_cbRGITemporal.CheckerboardTracing ? true : m_sampleIdx >= 2;

	m_currTemporalReservoirIdx = 1 - m_currTemporalReservoirIdx;
	m_validationFrame = m_validationFrame < m_validationPeriod ? m_validationFrame + 1 : 0;
	m_internalCounter = isTraceFrame ? m_internalCounter + 1 : m_internalCounter;

	// 1. don't advance the sample index if this frame was validation
	// 2. when checkerboarding, advance the sample index every other tracing frame
	if(isTraceFrame && (!m_cbRGITemporal.CheckerboardTracing || (m_internalCounter & 0x1)))
		m_sampleIdx = (m_sampleIdx + 1) & 31;
}

void ReSTIR_GI_Diffuse::CreateOutputs()
{
	auto& renderer = App::GetRenderer();

	auto func = [&renderer, this](Texture& tex, DXGI_FORMAT f, const char* name, 
		DESC_TABLE srv, DESC_TABLE uav, D3D12_RESOURCE_STATES s = D3D12_RESOURCE_STATE_COMMON)
	{
		tex = GpuMemory::GetTexture2D(name,
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			f,
			s,
			CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DUtil::CreateTexture2DSRV(tex, m_descTable.CPUHandle((int)srv));
		Direct3DUtil::CreateTexture2DUAV(tex, m_descTable.CPUHandle((int)uav));
	};

	{
		// temporal reservoirs
		func(m_temporalReservoirs[0].ReservoirA, ResourceFormats::RESERVOIR_A, "Diff_TemporalReservoir_0_A",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_A_UAV);
		func(m_temporalReservoirs[0].ReservoirB, ResourceFormats::RESERVOIR_B, "Diff_TemporalReservoir_0_B",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_B_UAV);
		func(m_temporalReservoirs[0].ReservoirC, ResourceFormats::RESERVOIR_C, "Diff_TemporalReservoir_0_C",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_C_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_C_UAV);
		func(m_temporalReservoirs[1].ReservoirA, ResourceFormats::RESERVOIR_A, "Diff_TemporalReservoir_1_A",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_A_UAV);
		func(m_temporalReservoirs[1].ReservoirB, ResourceFormats::RESERVOIR_B, "Diff_TemporalReservoir_1_B",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_B_UAV);
		func(m_temporalReservoirs[1].ReservoirC, ResourceFormats::RESERVOIR_C, "Diff_TemporalReservoir_1_C",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_C_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_C_UAV);

		// spatial reservoirs
		func(m_spatialReservoirs[0].ReservoirA, ResourceFormats::RESERVOIR_A, "Diff_SpatialReservoir_0_A",
			DESC_TABLE::SPATIAL_RESERVOIR_0_A_SRV, DESC_TABLE::SPATIAL_RESERVOIR_0_A_UAV, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		func(m_spatialReservoirs[0].ReservoirB, ResourceFormats::RESERVOIR_B, "Diff_SpatialReservoir_0_B",
			DESC_TABLE::SPATIAL_RESERVOIR_0_B_SRV, DESC_TABLE::SPATIAL_RESERVOIR_0_B_UAV, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		func(m_spatialReservoirs[0].ReservoirC, ResourceFormats::RESERVOIR_C, "Diff_SpatialReservoir_0_C",
			DESC_TABLE::SPATIAL_RESERVOIR_0_C_SRV, DESC_TABLE::SPATIAL_RESERVOIR_0_C_UAV, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		func(m_spatialReservoirs[1].ReservoirA, ResourceFormats::RESERVOIR_A, "Diff_SpatialReservoir_1_A",
			DESC_TABLE::SPATIAL_RESERVOIR_1_A_SRV, DESC_TABLE::SPATIAL_RESERVOIR_1_A_UAV);
		func(m_spatialReservoirs[1].ReservoirB, ResourceFormats::RESERVOIR_B, "Diff_SpatialReservoir_1_B",
			DESC_TABLE::SPATIAL_RESERVOIR_1_B_SRV, DESC_TABLE::SPATIAL_RESERVOIR_1_B_UAV);
		func(m_spatialReservoirs[1].ReservoirC, ResourceFormats::RESERVOIR_C, "Diff_SpatialReservoir_1_C",
			DESC_TABLE::SPATIAL_RESERVOIR_1_C_SRV, DESC_TABLE::SPATIAL_RESERVOIR_1_C_UAV);

		// denoiser temporal cache
		func(m_temporalCache[0], ResourceFormats::DNSR_TEMPORAL_CACHE, "DiffuseDNSR_TEMPORAL_CACHE_A",
			DESC_TABLE::TEMPORAL_CACHE_A_SRV, DESC_TABLE::TEMPORAL_CACHE_A_UAV);
		func(m_temporalCache[1], ResourceFormats::DNSR_TEMPORAL_CACHE, "DiffuseDNSR_TEMPORAL_CACHE_B",
			DESC_TABLE::TEMPORAL_CACHE_B_SRV, DESC_TABLE::TEMPORAL_CACHE_B_UAV);
		func(m_tsppAdjustment, ResourceFormats::DNSR_TSPP_ADJUSTMENT, "DiffuseDNSR_TSPP_Adjustment",
			DESC_TABLE::TSPP_ADJUSTMENT_SRV, DESC_TABLE::TSPP_ADJUSTMENT_UAV);
	}
}

void ReSTIR_GI_Diffuse::DoTemporalResamplingCallback(const Support::ParamVariant& p)
{
	m_cbRGITemporal.DoTemporalResampling = p.GetBool();
}

void ReSTIR_GI_Diffuse::DoSpatialResamplingCallback(const Support::ParamVariant& p)
{
	m_cbRGISpatial.DoSpatialResampling = p.GetBool();
}

void ReSTIR_GI_Diffuse::ValidationPeriodCallback(const Support::ParamVariant& p)
{
	m_validationPeriod = p.GetInt().m_val;
}

void ReSTIR_GI_Diffuse::RGINormalExpCallback(const Support::ParamVariant& p)
{
	m_cbRGISpatial.NormalExp = p.GetFloat().m_val;
}

void ReSTIR_GI_Diffuse::CheckerboardTracingCallback(const Support::ParamVariant& p)
{
	m_cbRGITemporal.CheckerboardTracing = p.GetBool();
}

void ReSTIR_GI_Diffuse::DNSRNumSpatialPassesCallback(const ParamVariant& p)
{
	m_numDNSRSpatialFilterPasses = p.GetInt().m_val;
}

void ReSTIR_GI_Diffuse::DNSRMaxTSPPCallback(const Support::ParamVariant& p)
{
	m_cbDNSRTemporal.MaxTspp = (uint16_t)p.GetInt().m_val;
	m_cbDNSRSpatial.MaxTspp = (uint16_t)p.GetInt().m_val;
}

void ReSTIR_GI_Diffuse::DNSRNormalExpCallback(const Support::ParamVariant& p)
{
	m_cbDNSRSpatial.NormalExp = p.GetFloat().m_val;
}

void ReSTIR_GI_Diffuse::DNSRMinFilterRadiusCallback(const Support::ParamVariant& p)
{
	m_cbDNSRSpatial.MinFilterRadius = Math::Min(m_cbDNSRSpatial.MaxFilterRadius, (uint16_t)p.GetInt().m_val);
}

void ReSTIR_GI_Diffuse::DNSRMaxFilterRadiusCallback(const Support::ParamVariant& p)
{
	m_cbDNSRSpatial.MaxFilterRadius = Math::Max(m_cbDNSRSpatial.MinFilterRadius, (uint16_t)p.GetInt().m_val);
}

void ReSTIR_GI_Diffuse::ReloadRGITemporalPass()
{
	const int i = (int)SHADERS::TEMPORAL_PASS;

	s_rpObjs.m_psoLib.Reload(i, "DiffuseIndirect\\ReSTIR_GI_Diffuse_Temporal.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void ReSTIR_GI_Diffuse::ReloadRGISpatialPass()
{
	const int i = (int)SHADERS::SPATIAL_PASS;

	s_rpObjs.m_psoLib.Reload(i, "DiffuseIndirect\\ReSTIR_GI_Diffuse_Spatial.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void ReSTIR_GI_Diffuse::ReloadValidationPass()
{
	const int i = (int)SHADERS::VALIDATION;

	s_rpObjs.m_psoLib.Reload(i, "DiffuseIndirect\\ReSTIR_GI_Diffuse_Validation.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void ReSTIR_GI_Diffuse::ReloadDNSRTemporalPass()
{
	const int i = (int)SHADERS::DIFFUSE_DNSR_TEMPORAL;

	s_rpObjs.m_psoLib.Reload(i, "DiffuseIndirect\\DiffuseDNSR_Temporal.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void ReSTIR_GI_Diffuse::ReloadDNSRSpatialPass()
{
	const int i = (int)SHADERS::DIFFUSE_DNSR_SPATIAL;

	s_rpObjs.m_psoLib.Reload(i, "DiffuseIndirect\\DiffuseDNSR_SpatialFilter.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}