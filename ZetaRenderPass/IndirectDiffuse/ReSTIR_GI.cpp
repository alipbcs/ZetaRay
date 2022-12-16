#include "ReSTIR_GI.h"
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
// ReSTIR_GI
//--------------------------------------------------------------------------------------

ReSTIR_GI::ReSTIR_GI() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// root constants
	m_rootSig.InitAsConstants(0,		// root idx
		NUM_CONSTS,						// num-DWORDs
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
		Sampler::SOBOL_SEQ);

	// scrambling tile
	m_rootSig.InitAsBufferSRV(5,						// root idx
		4,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE);

	// ranking tile
	m_rootSig.InitAsBufferSRV(6,						// root idx
		5,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		Sampler::SCRAMBLING_TILE);

	// mesh buffer
	m_rootSig.InitAsBufferSRV(7,						// root idx
		6,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
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

ReSTIR_GI::~ReSTIR_GI() noexcept
{
	Reset();
}

void ReSTIR_GI::Init() noexcept
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
	s_rpObjs.Init("ReSTIR_GI", m_rootSig, samplers.size(), samplers.data(), flags);

	for (int i = 0; i < (int)SHADERS::COUNT; i++)
	{
		m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
			s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[i]);
	}

	m_descTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((int)DESC_TABLE::COUNT);
	CreateOutputs();

	memset(&m_cbTemporal, 0, sizeof(m_cbTemporal));
	memset(&m_cbSpatial, 0, sizeof(m_cbSpatial));
	m_cbTemporal.MaxPlaneDist = m_cbSpatial.MaxPlaneDist = DefaultParamVals::MaxPlaneDist;
	m_cbTemporal.DoTemporalResampling = true;
	m_cbTemporal.PdfCorrection = m_cbSpatial.PdfCorrection = true;
	m_cbSpatial.NormalExp = DefaultParamVals::NormalExp;
	m_cbTemporal.FrameCounter = 0;
	m_cbTemporal.CheckerboardTracing = true;

	ParamVariant paramMaxPlaneDist;
	paramMaxPlaneDist.InitFloat("Renderer", "ReSTIR_GI", "MaxPlaneDist",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI::MaxPlaneDistCallback),
		DefaultParamVals::MaxPlaneDist,		// val	
		1e-2f,								// min
		1.0f,								// max
		1e-2f);								// step
	App::AddParam(paramMaxPlaneDist);

	ParamVariant normalExp;
	normalExp.InitFloat("Renderer", "ReSTIR_GI", "NormalExp",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI::NormalExpCallback),
		DefaultParamVals::NormalExp,		// val	
		1.0f,								// min
		8.0f,								// max
		1.0f);								// step
	App::AddParam(normalExp);

	ParamVariant validationT;
	validationT.InitInt("Renderer", "ReSTIR_GI", "ValidationPeriod",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI::ValidationPeriodCallback),
		DefaultParamVals::ValidationPeriod,		// val	
		0,										// min
		10,										// max
		1);										// step
	App::AddParam(validationT);

	ParamVariant doTemporal;
	doTemporal.InitBool("Renderer", "ReSTIR_GI", "TemporalResampling",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI::DoTemporalResamplingCallback), true);
	App::AddParam(doTemporal);

	ParamVariant doSpatial;
	doSpatial.InitBool("Renderer", "ReSTIR_GI", "SpatialResampling",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI::DoSpatialResamplingCallback), m_doSpatialResampling);
	App::AddParam(doSpatial);

	ParamVariant pdfCorrection;
	pdfCorrection.InitBool("Renderer", "ReSTIR_GI", "PdfCorrection",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI::PdfCorrectionCallback), m_cbTemporal.PdfCorrection);
	App::AddParam(pdfCorrection);

	ParamVariant checkerboard;
	checkerboard.InitBool("Renderer", "ReSTIR_GI", "CheckerboardTracing",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI::CheckerboardTracingCallback), m_cbTemporal.CheckerboardTracing);
	App::AddParam(checkerboard);

	App::AddShaderReloadHandler("ReSTIR_GI_Temporal", fastdelegate::MakeDelegate(this, &ReSTIR_GI::ReloadTemporalPass));
	App::AddShaderReloadHandler("ReSTIR_GI_Spatial", fastdelegate::MakeDelegate(this, &ReSTIR_GI::ReloadSpatialPass));
	App::AddShaderReloadHandler("ReSTIR_GI_Validation", fastdelegate::MakeDelegate(this, &ReSTIR_GI::ReloadValidationPass));

	m_isTemporalReservoirValid = false;
}

void ReSTIR_GI::Reset() noexcept
{
	if (IsInitialized())
	{
		App::RemoveShaderReloadHandler("ReSTIR_GI_Temporal");
		App::RemoveShaderReloadHandler("ReSTIR_GI_Spatial");
		App::RemoveShaderReloadHandler("ReSTIR_GI_Validation");
		s_rpObjs.Clear();
		
		for (int i = 0; i < 2; i++)
		{
			m_temporalReservoirs[i].ReservoirA.Reset();
			m_temporalReservoirs[i].ReservoirB.Reset();
			m_temporalReservoirs[i].ReservoirC.Reset();
			m_spatialReservoirs[i].ReservoirA.Reset();
			m_spatialReservoirs[i].ReservoirB.Reset();
			m_spatialReservoirs[i].ReservoirC.Reset();
		}

		for (int i = 0; i < ZetaArrayLen(m_psos); i++)
			m_psos[i] = nullptr;

		m_descTable.Reset();
	}
}

void ReSTIR_GI::OnWindowResized() noexcept
{
	CreateOutputs();
}

void ReSTIR_GI::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	const int w = App::GetRenderer().GetRenderWidth();
	const int h = App::GetRenderer().GetRenderHeight();
	const bool isTraceFrame = m_validationPeriod == 0 || m_validationFrame != 0;

	// Temporal resampling/Validation
	{
		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, RGI_TEMPORAL_THREAD_GROUP_SIZE_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, RGI_TEMPORAL_THREAD_GROUP_SIZE_Y);

		if (isTraceFrame)
		{
			computeCmdList.PIXBeginEvent("ReSTIR_GI_Temporal");
			computeCmdList.SetPipelineState(m_psos[(int)SHADERS::TEMPORAL_PASS]);
		}
		else
		{
			computeCmdList.PIXBeginEvent("ReSTIR_GI_Validation");
			computeCmdList.SetPipelineState(m_psos[(int)SHADERS::VALIDATION]);
		}

		computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

		m_cbTemporal.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbTemporal.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbTemporal.IsTemporalReservoirValid = m_isTemporalReservoirValid;
		m_cbTemporal.NumGroupsInTile = RGI_TEMPORAL_TILE_WIDTH * m_cbTemporal.DispatchDimY;
		m_cbTemporal.SampleIndex = (uint16_t)m_sampleIdx;
		m_cbTemporal.FrameCounter = m_internalCounter;

		auto srvAIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV;
		auto srvBIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV;
		auto srvCIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_0_C_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_1_C_SRV;
		auto uavAIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_A_UAV : DESC_TABLE::TEMPORAL_RESERVOIR_0_A_UAV;
		auto uavBIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_B_UAV : DESC_TABLE::TEMPORAL_RESERVOIR_0_B_UAV;
		auto uavCIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_C_UAV : DESC_TABLE::TEMPORAL_RESERVOIR_0_C_UAV;

		m_cbTemporal.PrevTemporalReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
		m_cbTemporal.PrevTemporalReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
		m_cbTemporal.PrevTemporalReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvCIdx);
		m_cbTemporal.CurrTemporalReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
		m_cbTemporal.CurrTemporalReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);
		m_cbTemporal.CurrTemporalReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavCIdx);

		m_rootSig.SetRootConstants(0, sizeof(cbTemporalPass) / sizeof(DWORD), &m_cbTemporal);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		cmdList.PIXEndEvent();
	}

	// spatial resampling
	if (m_doSpatialResampling)
	{
		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, RGI_SPATIAL_THREAD_GROUP_SIZE_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, RGI_SPATIAL_THREAD_GROUP_SIZE_Y);

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::SPATIAL_PASS]);

		m_cbSpatial.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbSpatial.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbSpatial.NumGroupsInTile = RGI_SPATIAL_TILE_WIDTH * m_cbSpatial.DispatchDimY;

		{
			computeCmdList.PIXBeginEvent("ReSTIR_GI_Spatial_1");

			// transition temporal reservoir into read state
			D3D12_RESOURCE_BARRIER barrier0 = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirA.GetResource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			D3D12_RESOURCE_BARRIER barrier1 = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirB.GetResource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			D3D12_RESOURCE_BARRIER barrier2 = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirC.GetResource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			// transition spatial reservoir 0 into write state
			D3D12_RESOURCE_BARRIER barrier3 = Direct3DHelper::TransitionBarrier(m_spatialReservoirs[0].ReservoirA.GetResource(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			D3D12_RESOURCE_BARRIER barrier4 = Direct3DHelper::TransitionBarrier(m_spatialReservoirs[0].ReservoirB.GetResource(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			D3D12_RESOURCE_BARRIER barrier5 = Direct3DHelper::TransitionBarrier(m_spatialReservoirs[0].ReservoirC.GetResource(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			D3D12_RESOURCE_BARRIER inBarriers[] = { barrier0, barrier1, barrier2, barrier3, barrier4, barrier5 };
			computeCmdList.TransitionResource(inBarriers, ZetaArrayLen(inBarriers));

			m_cbSpatial.IsFirstPass = true;

			auto srvAIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV;
			auto srvBIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV;
			auto srvCIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_C_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_0_C_SRV;
			auto uavAIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_A_UAV;
			auto uavBIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_B_UAV;
			auto uavCIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_C_UAV;

			m_cbSpatial.InputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
			m_cbSpatial.InputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
			m_cbSpatial.InputReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvCIdx);
			m_cbSpatial.OutputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
			m_cbSpatial.OutputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);
			m_cbSpatial.OutputReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavCIdx);

			m_rootSig.SetRootConstants(0, sizeof(cbSpatialPass) / sizeof(DWORD), &m_cbSpatial);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

			cmdList.PIXEndEvent();
		}

		{
			computeCmdList.PIXBeginEvent("ReSTIR_GI_Spatial_2");

			// transition spatial reservoir into read state
			D3D12_RESOURCE_BARRIER barrier0 = Direct3DHelper::TransitionBarrier(m_spatialReservoirs[0].ReservoirA.GetResource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			D3D12_RESOURCE_BARRIER barrier1 = Direct3DHelper::TransitionBarrier(m_spatialReservoirs[0].ReservoirB.GetResource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			D3D12_RESOURCE_BARRIER barrier2 = Direct3DHelper::TransitionBarrier(m_spatialReservoirs[0].ReservoirC.GetResource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			D3D12_RESOURCE_BARRIER inBarriers[] = { barrier0, barrier1, barrier2 };
			computeCmdList.TransitionResource(inBarriers, ZetaArrayLen(inBarriers));

			m_cbSpatial.IsFirstPass = false;

			auto srvAIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_A_SRV;
			auto srvBIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_B_SRV;
			auto srvCIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_C_SRV;
			auto uavAIdx = DESC_TABLE::SPATIAL_RESERVOIR_1_A_UAV;
			auto uavBIdx = DESC_TABLE::SPATIAL_RESERVOIR_1_B_UAV;
			auto uavCIdx = DESC_TABLE::SPATIAL_RESERVOIR_1_C_UAV;

			m_cbSpatial.InputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
			m_cbSpatial.InputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
			m_cbSpatial.InputReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvCIdx);
			m_cbSpatial.OutputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
			m_cbSpatial.OutputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);
			m_cbSpatial.OutputReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavCIdx);

			m_rootSig.SetRootConstants(0, sizeof(cbSpatialPass) / sizeof(DWORD), &m_cbSpatial);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

			cmdList.PIXEndEvent();
		}
	}

	// [hack] render graph is unaware of renderpass-internal transitions. Restore the initial state to avoid
	// render graph and actual state getting out of sync
	D3D12_RESOURCE_BARRIER barrierA = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirA.GetResource(),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	D3D12_RESOURCE_BARRIER barrierB = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirB.GetResource(),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	D3D12_RESOURCE_BARRIER barrierC = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirC.GetResource(),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	D3D12_RESOURCE_BARRIER outBarriers[] = { barrierA, barrierB, barrierC };
	computeCmdList.TransitionResource(outBarriers, ZetaArrayLen(outBarriers));

	if (!m_isTemporalReservoirValid)
		m_isTemporalReservoirValid = !m_cbTemporal.CheckerboardTracing ? true : m_sampleIdx >= 2;

	m_currTemporalReservoirIdx = 1 - m_currTemporalReservoirIdx;
	m_validationFrame = m_validationFrame < m_validationPeriod ? m_validationFrame + 1 : 0;
	m_internalCounter = isTraceFrame ? m_internalCounter + 1 : m_internalCounter;

	// 1. don't advance the sample index if this frame was validation
	// 2. if checkerboarding, advance the sample index every other tracing frame
	if(isTraceFrame && (!m_cbTemporal.CheckerboardTracing || (m_internalCounter & 0x1)))
		m_sampleIdx = (m_sampleIdx + 1) & 31;
}

void ReSTIR_GI::CreateOutputs() noexcept
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

	{
		// temporal reservoirs
		func(m_temporalReservoirs[0].ReservoirA, ResourceFormats::RESERVOIR_A, "TemporalReservoir_0_A",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_A_UAV);
		func(m_temporalReservoirs[0].ReservoirB, ResourceFormats::RESERVOIR_B, "TemporalReservoir_0_B",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_B_UAV);
		func(m_temporalReservoirs[0].ReservoirC, ResourceFormats::RESERVOIR_C, "TemporalReservoir_0_C",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_C_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_C_UAV);
		func(m_temporalReservoirs[1].ReservoirA, ResourceFormats::RESERVOIR_A, "TemporalReservoir_1_A",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_A_UAV);
		func(m_temporalReservoirs[1].ReservoirB, ResourceFormats::RESERVOIR_B, "TemporalReservoir_1_B",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_B_UAV);
		func(m_temporalReservoirs[1].ReservoirC, ResourceFormats::RESERVOIR_C, "TemporalReservoir_1_C",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_C_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_C_UAV);

		// spatial reservoirs
		func(m_spatialReservoirs[0].ReservoirA, ResourceFormats::RESERVOIR_A, "SpatialReservoir_0_A",
			DESC_TABLE::SPATIAL_RESERVOIR_0_A_SRV, DESC_TABLE::SPATIAL_RESERVOIR_0_A_UAV, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		func(m_spatialReservoirs[0].ReservoirB, ResourceFormats::RESERVOIR_B, "SpatialReservoir_0_B",
			DESC_TABLE::SPATIAL_RESERVOIR_0_B_SRV, DESC_TABLE::SPATIAL_RESERVOIR_0_B_UAV, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		func(m_spatialReservoirs[0].ReservoirC, ResourceFormats::RESERVOIR_C, "SpatialReservoir_0_C",
			DESC_TABLE::SPATIAL_RESERVOIR_0_C_SRV, DESC_TABLE::SPATIAL_RESERVOIR_0_C_UAV, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		func(m_spatialReservoirs[1].ReservoirA, ResourceFormats::RESERVOIR_A, "SpatialReservoir_1_A",
			DESC_TABLE::SPATIAL_RESERVOIR_1_A_SRV, DESC_TABLE::SPATIAL_RESERVOIR_1_A_UAV);
		func(m_spatialReservoirs[1].ReservoirB, ResourceFormats::RESERVOIR_B, "SpatialReservoir_1_B",
			DESC_TABLE::SPATIAL_RESERVOIR_1_B_SRV, DESC_TABLE::SPATIAL_RESERVOIR_1_B_UAV);
		func(m_spatialReservoirs[1].ReservoirC, ResourceFormats::RESERVOIR_C, "SpatialReservoir_1_C",
			DESC_TABLE::SPATIAL_RESERVOIR_1_C_SRV, DESC_TABLE::SPATIAL_RESERVOIR_1_C_UAV);
	}
}

void ReSTIR_GI::DoTemporalResamplingCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporal.DoTemporalResampling = p.GetBool();
}

void ReSTIR_GI::DoSpatialResamplingCallback(const Support::ParamVariant& p) noexcept
{
	m_doSpatialResampling = p.GetBool();
}

void ReSTIR_GI::PdfCorrectionCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporal.PdfCorrection = p.GetBool();
	m_cbSpatial.PdfCorrection = p.GetBool();
}

void ReSTIR_GI::MaxPlaneDistCallback(const ParamVariant& p) noexcept
{
	m_cbTemporal.MaxPlaneDist = p.GetFloat().m_val;
	m_cbSpatial.MaxPlaneDist = p.GetFloat().m_val;
}

void ReSTIR_GI::ValidationPeriodCallback(const Support::ParamVariant& p) noexcept
{
	m_validationPeriod = p.GetInt().m_val;
}

void ReSTIR_GI::NormalExpCallback(const Support::ParamVariant& p) noexcept
{
	m_cbSpatial.NormalExp = p.GetFloat().m_val;
}

void ReSTIR_GI::CheckerboardTracingCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporal.CheckerboardTracing = p.GetBool();
}

void ReSTIR_GI::ReloadTemporalPass() noexcept
{
	const int i = (int)SHADERS::TEMPORAL_PASS;

	s_rpObjs.m_psoLib.Reload(i, "IndirectDiffuse\\ReSTIR_GI_TemporalPass.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void ReSTIR_GI::ReloadSpatialPass() noexcept
{
	const int i = (int)SHADERS::SPATIAL_PASS;

	s_rpObjs.m_psoLib.Reload(i, "IndirectDiffuse\\ReSTIR_GI_SpatialPass.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void ReSTIR_GI::ReloadValidationPass() noexcept
{
	const int i = (int)SHADERS::VALIDATION;

	s_rpObjs.m_psoLib.Reload(i, "IndirectDiffuse\\ReSTIR_GI_Validation.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

