#include "ReSTIR_GI_Specular.h"
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
// ReSTIR_GI_Specular
//--------------------------------------------------------------------------------------

ReSTIR_GI_Specular::ReSTIR_GI_Specular() noexcept
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

ReSTIR_GI_Specular::~ReSTIR_GI_Specular() noexcept
{
	Reset();
}

void ReSTIR_GI_Specular::Init() noexcept
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
	s_rpObjs.Init("ReSTIR_GI_Spec", m_rootSig, samplers.size(), samplers.data(), flags);

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
	m_cbTemporal.DoTemporalResampling = true;
	m_cbTemporal.PdfCorrection = m_cbSpatial.PdfCorrection = true;
	m_cbTemporal.RoughnessCutoff = m_cbSpatial.RoughnessCutoff = DefaultParamVals::RoughnessCutoff;
	m_cbTemporal.M_max = DefaultParamVals::TemporalM_max;
	m_cbTemporal.MinRoughnessResample = m_cbSpatial.MinRoughnessResample = m_cbDNSR.MinRoughnessResample = DefaultParamVals::MinRoughnessResample;
	m_cbTemporal.HitDistSigmaScale = DefaultParamVals::TemporalHitDistSigmaScale;
	m_cbTemporal.CheckerboardTracing = false;
	m_cbSpatial.HitDistSigmaScale = DefaultParamVals::SpatialHitDistSigmaScale;
	m_cbSpatial.DoSpatialResampling = true;
	m_cbSpatial.Radius = DefaultParamVals::SpatialResampleRadius;
	m_cbSpatial.M_max = DefaultParamVals::SpatialM_max;
	m_cbSpatial.NumIterations = DefaultParamVals::SpatialResampleNumIter;
	m_cbDNSR.Denoise = true;
	m_cbDNSR.MaxTSPP = DefaultParamVals::DNSRTspp;
	m_cbDNSR.RoughnessCutoff = m_cbTemporal.RoughnessCutoff;
	m_cbDNSR.RoughnessExpScale = DefaultParamVals::DNSRRoughnessExpScale;
	m_cbDNSR.ViewAngleExp = DefaultParamVals::DNSRViewAngleExp;

	//ParamVariant normalExp;
	//normalExp.InitFloat("Renderer", "ReSTIR_GI_Specular", "NormalExp",
	//	fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::NormalExpCallback),
	//	DefaultParamVals::NormalExp,		// val	
	//	1.0f,								// min
	//	8.0f,								// max
	//	1.0f);								// step
	//App::AddParam(normalExp);

	ParamVariant roughnessCutoff;
	roughnessCutoff.InitFloat("Renderer", "ReSTIR_GI_Specular", "RoughnessCutoff",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::RoughnessCutoffCallback),
		DefaultParamVals::RoughnessCutoff,	// val	
		0.0f,								// min
		1.0f,								// max
		0.1f);								// step
	App::AddParam(roughnessCutoff);

	ParamVariant temporalM;
	temporalM.InitInt("Renderer", "ReSTIR_GI_Specular", "Temporal M_max",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::MaxTemporalMCallback),
		m_cbTemporal.M_max,				// val	
		1,								// min
		20,								// max
		1);								// step
	App::AddParam(temporalM);

	ParamVariant spatialM;
	spatialM.InitInt("Renderer", "ReSTIR_GI_Specular", "Spatial M_max",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::MaxSpatialMCallback),
		m_cbSpatial.M_max,				// val	
		1,								// min
		20,								// max
		1);								// step
	App::AddParam(spatialM);

	ParamVariant numiter;
	numiter.InitInt("Renderer", "ReSTIR_GI_Specular", "NumIterations",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::NumIterationsCallback),
		m_cbSpatial.NumIterations,		// val	
		1,								// min
		16,								// max
		1);								// step
	App::AddParam(numiter);

	ParamVariant minAlpha;
	minAlpha.InitFloat("Renderer", "ReSTIR_GI_Specular", "MinRoughnessResample",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::MinRoughnessResample),
		m_cbTemporal.MinRoughnessResample,	// val	
		0.0f,								// min
		1.0f,								// max
		1e-2f);								// step
	App::AddParam(minAlpha);

	ParamVariant sigmaScaleTemporal;
	sigmaScaleTemporal.InitFloat("Renderer", "ReSTIR_GI_Specular", "TemporalHitDistSigmaScale",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::TemporalHistDistSigmaScaleCallback),
		m_cbTemporal.HitDistSigmaScale,	// val	
		0.5f,							// min
		1.5f,							// max
		1e-2f);							// step
	App::AddParam(sigmaScaleTemporal);

	ParamVariant sigmaScaleSpatial;
	sigmaScaleSpatial.InitFloat("Renderer", "ReSTIR_GI_Specular", "SpatialHitDistSigmaScale",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::SpatialHistDistSigmaScaleCallback),
		m_cbSpatial.HitDistSigmaScale,	// val	
		0.75f,							// min
		5.0f,							// max
		1e-2f);							// step
	App::AddParam(sigmaScaleSpatial);

	ParamVariant radius;
	radius.InitInt("Renderer", "ReSTIR_GI_Specular", "SpatialRadius",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::SpatialRadiusCallback),
		m_cbSpatial.Radius,				// val	
		1,								// min
		32,								// max
		1);								// step
	App::AddParam(radius);

	ParamVariant doTemporal;
	doTemporal.InitBool("Renderer", "ReSTIR_GI_Specular", "TemporalResampling",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::DoTemporalResamplingCallback), m_cbTemporal.DoTemporalResampling);
	App::AddParam(doTemporal);

	ParamVariant doSpatial;
	doSpatial.InitBool("Renderer", "ReSTIR_GI_Specular", "SpatialResampling",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::DoSpatialResamplingCallback), m_cbSpatial.DoSpatialResampling);
	App::AddParam(doSpatial);

	ParamVariant pdfCorrection;
	pdfCorrection.InitBool("Renderer", "ReSTIR_GI_Specular", "PdfCorrection",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::PdfCorrectionCallback), m_cbTemporal.PdfCorrection);
	App::AddParam(pdfCorrection);

	ParamVariant denoise;
	denoise.InitBool("Renderer", "SpecularDNSR", "Enable",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::DoDenoisingCallback), m_cbDNSR.Denoise);
	App::AddParam(denoise);

	ParamVariant tspp;
	tspp.InitInt("Renderer", "SpecularDNSR", "MaxTSPP",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::TsppCallback),
		m_cbDNSR.MaxTSPP,				// val	
		1,								// min
		32,								// max
		1);								// step
	App::AddParam(tspp);

	ParamVariant viewAngleExp;
	viewAngleExp.InitFloat("Renderer", "SpecularDNSR", "ViewAngleExp",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::DNSRViewAngleExpCallback),
		m_cbDNSR.ViewAngleExp,			// val	
		0.1f,							// min
		1.0f,							// max
		1e-2f);							// step
	App::AddParam(viewAngleExp);

	ParamVariant roughnessExp;
	roughnessExp.InitFloat("Renderer", "SpecularDNSR", "RoughnessExpScale",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::DNSRRoughnessExpScaleCallback),
		m_cbDNSR.RoughnessExpScale,		// val	
		0.1f,							// min
		1.0f,							// max
		1e-2f);							// step
	App::AddParam(roughnessExp);

	ParamVariant checkerboarding;
	checkerboarding.InitBool("Renderer", "ReSTIR_GI_Specular", "CheckerboardTrace",
		fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::CheckerboardingCallback), m_cbTemporal.CheckerboardTracing);
	App::AddParam(checkerboarding);

	App::AddShaderReloadHandler("ReSTIR_GI_Specular_Temporal", fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::ReloadTemporalPass));
	App::AddShaderReloadHandler("ReSTIR_GI_Specular_Spatial", fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::ReloadSpatialPass));
	App::AddShaderReloadHandler("SpecularDNSR", fastdelegate::MakeDelegate(this, &ReSTIR_GI_Specular::ReloadDNSR));

	m_isTemporalReservoirValid = false;
}

void ReSTIR_GI_Specular::Reset() noexcept
{
	if (IsInitialized())
	{
		//App::RemoveShaderReloadHandler("ReSTIR_GI_Temporal");
		//App::RemoveShaderReloadHandler("ReSTIR_GI_Spatial");
		s_rpObjs.Clear();
		
		for (int i = 0; i < 2; i++)
		{
			m_temporalReservoirs[i].ReservoirA.Reset();
			m_temporalReservoirs[i].ReservoirB.Reset();
			m_temporalReservoirs[i].ReservoirC.Reset();
			m_temporalReservoirs[i].ReservoirD.Reset();
			//m_spatialReservoirs[i].ReservoirA.Reset();
			//m_spatialReservoirs[i].ReservoirB.Reset();
			//m_spatialReservoirs[i].ReservoirC.Reset();
		}

		for (int i = 0; i < ZetaArrayLen(m_psos); i++)
			m_psos[i] = nullptr;

		m_descTable.Reset();
	}
}

void ReSTIR_GI_Specular::OnWindowResized() noexcept
{
	CreateOutputs();
	m_isTemporalReservoirValid = false;
}

void ReSTIR_GI_Specular::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();
	const int w = renderer.GetRenderWidth();
	const int h = renderer.GetRenderHeight();

	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

	// estimate curvature
	{
		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, ESTIMATE_CURVATURE_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, ESTIMATE_CURVATURE_GROUP_DIM_Y);

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "EstimateCurvature");

		computeCmdList.PIXBeginEvent("EstimateCurvature");
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::ESTIMATE_CURVAURE]);

		computeCmdList.ResourceBarrier(m_curvature.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		cbCurvature cb;
		cb.OutputUAVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::CURVATURE_UAV);

		m_rootSig.SetRootConstants(0, sizeof(cbCurvature) / sizeof(DWORD), &cb);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();
	}

	// Temporal resampling
	{
		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, RGI_SPEC_TEMPORAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, RGI_SPEC_TEMPORAL_GROUP_DIM_Y);

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_GI_Spec_Temporal");

		computeCmdList.PIXBeginEvent("ReSTIR_GI_Specular_Temporal");
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::TEMPORAL_RESAMPLE]);

		computeCmdList.ResourceBarrier(m_curvature.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		m_cbTemporal.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbTemporal.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbTemporal.IsTemporalReservoirValid = m_isTemporalReservoirValid;
		m_cbTemporal.SampleIndex = (uint16_t)m_sampleIdx;
		m_cbTemporal.NumGroupsInTile = RGI_SPEC_TEMPORAL_TILE_WIDTH * m_cbTemporal.DispatchDimY;

		auto srvAIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV;
		auto srvBIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV;
		auto srvCIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_0_C_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_1_C_SRV;
		auto srvDIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_0_D_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_1_D_SRV;
		auto uavAIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_A_UAV : DESC_TABLE::TEMPORAL_RESERVOIR_0_A_UAV;
		auto uavBIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_B_UAV : DESC_TABLE::TEMPORAL_RESERVOIR_0_B_UAV;
		auto uavCIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_C_UAV : DESC_TABLE::TEMPORAL_RESERVOIR_0_C_UAV;
		auto uavDIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_D_UAV : DESC_TABLE::TEMPORAL_RESERVOIR_0_D_UAV;

		m_cbTemporal.PrevTemporalReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
		m_cbTemporal.PrevTemporalReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
		m_cbTemporal.PrevTemporalReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvCIdx);
		m_cbTemporal.PrevTemporalReservoir_D_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvDIdx);
		m_cbTemporal.CurrTemporalReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
		m_cbTemporal.CurrTemporalReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);
		m_cbTemporal.CurrTemporalReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavCIdx);
		m_cbTemporal.CurrTemporalReservoir_D_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavDIdx);
		m_cbTemporal.CurvatureSRVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::CURVATURE_SRV);

		m_rootSig.SetRootConstants(0, sizeof(cb_RGI_Spec_Temporal) / sizeof(DWORD), &m_cbTemporal);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();
	}

	// spatial resampling
	{
		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, RGI_SPEC_SPATIAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, RGI_SPEC_SPATIAL_GROUP_DIM_Y);

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::SPATIAL_RESAMPLE]);

		m_cbSpatial.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbSpatial.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbSpatial.NumGroupsInTile = RGI_SPEC_SPATIAL_TILE_WIDTH * m_cbSpatial.DispatchDimY;

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_GI_Spec_Spatial");

		computeCmdList.PIXBeginEvent("ReSTIR_GI_Spec_Spatial");

		D3D12_RESOURCE_BARRIER barriers[4];
		int i = 0;

		// transition temporal reservoir into read state
		barriers[i++] = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirA.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barriers[i++] = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirB.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barriers[i++] = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirC.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barriers[i++] = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirD.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		Assert(i == ZetaArrayLen(barriers), "out-of-bound write");
		computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

		auto srvAIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV;
		auto srvBIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV;
		auto srvCIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_C_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_0_C_SRV;
		auto srvDIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::TEMPORAL_RESERVOIR_1_D_SRV : DESC_TABLE::TEMPORAL_RESERVOIR_0_D_SRV;
		auto uavAIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_A_UAV;
		auto uavBIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_B_UAV;
		auto uavDIdx = DESC_TABLE::SPATIAL_RESERVOIR_0_D_UAV;

		m_cbSpatial.InputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
		m_cbSpatial.InputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
		m_cbSpatial.InputReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvCIdx);
		m_cbSpatial.InputReservoir_D_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvDIdx);
		m_cbSpatial.OutputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
		m_cbSpatial.OutputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);
		m_cbSpatial.OutputReservoir_D_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavDIdx);

		m_rootSig.SetRootConstants(0, sizeof(cb_RGI_Spec_Spatial) / sizeof(DWORD), &m_cbSpatial);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();
	}

	// denoiser
	{
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR]);

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SpecularDNSR");

		computeCmdList.PIXBeginEvent("SpecularDNSR");

		D3D12_RESOURCE_BARRIER barriers[3];
		int i = 0;

		// transition spatial reservoir into read state
		barriers[i++] = Direct3DHelper::TransitionBarrier(m_spatialReservoir.ReservoirA.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barriers[i++] = Direct3DHelper::TransitionBarrier(m_spatialReservoir.ReservoirB.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barriers[i++] = Direct3DHelper::TransitionBarrier(m_spatialReservoir.ReservoirD.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		Assert(i == ZetaArrayLen(barriers), "out-of-bound write");
		computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

		auto srvIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_0_SRV : DESC_TABLE::DNSR_TEMPORAL_CACHE_1_SRV;
		auto uavIdx = m_currTemporalReservoirIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_1_UAV : DESC_TABLE::DNSR_TEMPORAL_CACHE_0_UAV;

		m_cbDNSR.InputReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::SPATIAL_RESERVOIR_0_A_SRV);
		m_cbDNSR.InputReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::SPATIAL_RESERVOIR_0_B_SRV);
		m_cbDNSR.InputReservoir_D_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::SPATIAL_RESERVOIR_0_D_SRV);
		m_cbDNSR.PrevTemporalCacheDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvIdx);
		m_cbDNSR.CurrTemporalCacheDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavIdx);
		m_cbDNSR.IsTemporalCacheValid = m_isTemporalReservoirValid;
		m_cbDNSR.CurvatureSRVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::CURVATURE_SRV);

		m_rootSig.SetRootConstants(0, sizeof(cbDNSR) / sizeof(DWORD), &m_cbDNSR);
		m_rootSig.End(computeCmdList);

		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, SPECULAR_DNSR_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, SPECULAR_DNSR_GROUP_DIM_Y);
		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();
	}

	// [hack] render graph is unaware of renderpass-internal transitions. Restore the initial state to avoid
	// render graph and actual state getting out of sync
	{
		D3D12_RESOURCE_BARRIER outBarriers[4 + 3];
		int curr = 0;

		outBarriers[curr++] = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirA.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		outBarriers[curr++] = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirB.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		outBarriers[curr++] = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirC.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		outBarriers[curr++] = Direct3DHelper::TransitionBarrier(m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirD.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		// transition spatial reservoir into uav state
		outBarriers[curr++] = Direct3DHelper::TransitionBarrier(m_spatialReservoir.ReservoirA.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		outBarriers[curr++] = Direct3DHelper::TransitionBarrier(m_spatialReservoir.ReservoirB.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		outBarriers[curr++] = Direct3DHelper::TransitionBarrier(m_spatialReservoir.ReservoirD.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		Assert(curr == ZetaArrayLen(outBarriers), "bug");
		computeCmdList.ResourceBarrier(outBarriers, ZetaArrayLen(outBarriers));
	}

	m_isTemporalReservoirValid = true;
	m_currTemporalReservoirIdx = 1 - m_currTemporalReservoirIdx;
	m_sampleIdx = (m_sampleIdx + 1) & 31;
	m_internalCounter++;

	// when checkerboarding, advance the sample index every other tracing frame
	if (!m_cbTemporal.CheckerboardTracing || (m_internalCounter & 0x1))
		m_sampleIdx = (m_sampleIdx + 1) & 31;
}

void ReSTIR_GI_Specular::CreateOutputs() noexcept
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
		func(m_temporalReservoirs[0].ReservoirA, ResourceFormats::RESERVOIR_A, "Spec_TemporalReservoir_0_A",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_A_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_A_UAV);
		func(m_temporalReservoirs[0].ReservoirB, ResourceFormats::RESERVOIR_B, "Spec_TemporalReservoir_0_B",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_B_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_B_UAV);
		func(m_temporalReservoirs[0].ReservoirC, ResourceFormats::RESERVOIR_C, "Spec_TemporalReservoir_0_C",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_C_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_C_UAV);
		func(m_temporalReservoirs[0].ReservoirD, ResourceFormats::RESERVOIR_D, "Spec_TemporalReservoir_0_D",
			DESC_TABLE::TEMPORAL_RESERVOIR_0_D_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_0_D_UAV);
		func(m_temporalReservoirs[1].ReservoirA, ResourceFormats::RESERVOIR_A, "Spec_TemporalReservoir_1_A",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_A_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_A_UAV);
		func(m_temporalReservoirs[1].ReservoirB, ResourceFormats::RESERVOIR_B, "Spec_TemporalReservoir_1_B",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_B_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_B_UAV);
		func(m_temporalReservoirs[1].ReservoirC, ResourceFormats::RESERVOIR_C, "Spec_TemporalReservoir_1_C",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_C_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_C_UAV);
		func(m_temporalReservoirs[1].ReservoirD, ResourceFormats::RESERVOIR_D, "Spec_TemporalReservoir_1_D",
			DESC_TABLE::TEMPORAL_RESERVOIR_1_D_SRV, DESC_TABLE::TEMPORAL_RESERVOIR_1_D_UAV);

		// spatial reservoirs
		func(m_spatialReservoir.ReservoirA, ResourceFormats::RESERVOIR_A, "Spec_SpatialReservoir_A",
			DESC_TABLE::SPATIAL_RESERVOIR_0_A_SRV, DESC_TABLE::SPATIAL_RESERVOIR_0_A_UAV);
		func(m_spatialReservoir.ReservoirB, ResourceFormats::RESERVOIR_B, "Spec_SpatialReservoir_B",
			DESC_TABLE::SPATIAL_RESERVOIR_0_B_SRV, DESC_TABLE::SPATIAL_RESERVOIR_0_B_UAV);
		func(m_spatialReservoir.ReservoirD, ResourceFormats::RESERVOIR_D, "Spec_SpatialReservoir_D",
			DESC_TABLE::SPATIAL_RESERVOIR_0_D_SRV, DESC_TABLE::SPATIAL_RESERVOIR_0_D_UAV);
	}

	// denoiser cache
	{
		func(m_dnsrTemporalCache[0], ResourceFormats::DNSR_TEMPORAL_CACHE, "SpecularDNSR_0",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_0_UAV);
		func(m_dnsrTemporalCache[1], ResourceFormats::DNSR_TEMPORAL_CACHE, "SpecularDNSR_1",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_1_UAV);
	}

	// curvature
	{
		func(m_curvature, ResourceFormats::CURVATURE, "Curvature",
			DESC_TABLE::CURVATURE_SRV, DESC_TABLE::CURVATURE_UAV);
	}
}

void ReSTIR_GI_Specular::DoTemporalResamplingCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporal.DoTemporalResampling = p.GetBool();
}

void ReSTIR_GI_Specular::DoSpatialResamplingCallback(const Support::ParamVariant& p) noexcept
{
	m_cbSpatial.DoSpatialResampling = p.GetBool();
}

void ReSTIR_GI_Specular::DoDenoisingCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSR.Denoise = p.GetBool();
}

void ReSTIR_GI_Specular::TsppCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSR.MaxTSPP = (uint16_t)p.GetInt().m_val;
}

void ReSTIR_GI_Specular::DNSRViewAngleExpCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSR.ViewAngleExp = p.GetFloat().m_val;
}

void ZetaRay::RenderPass::ReSTIR_GI_Specular::DNSRRoughnessExpScaleCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDNSR.RoughnessExpScale = p.GetFloat().m_val;
}

void ReSTIR_GI_Specular::PdfCorrectionCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporal.PdfCorrection = p.GetBool();
	m_cbSpatial.PdfCorrection = p.GetBool();
}

//void ReSTIR_GI_Specular::NormalExpCallback(const Support::ParamVariant& p) noexcept
//{
//	m_cbSpatial.NormalExp = p.GetFloat().m_val;
//}

void ReSTIR_GI_Specular::RoughnessCutoffCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporal.RoughnessCutoff = p.GetFloat().m_val;
	m_cbSpatial.RoughnessCutoff = m_cbTemporal.RoughnessCutoff;
	m_cbDNSR.RoughnessCutoff = m_cbTemporal.RoughnessCutoff;
}

void ReSTIR_GI_Specular::MaxTemporalMCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporal.M_max = p.GetInt().m_val;
}

void ReSTIR_GI_Specular::MaxSpatialMCallback(const Support::ParamVariant& p) noexcept
{
	m_cbSpatial.M_max = (uint16_t)p.GetInt().m_val;
}

void ReSTIR_GI_Specular::MinRoughnessResample(const Support::ParamVariant& p) noexcept
{
	m_cbTemporal.MinRoughnessResample = p.GetFloat().m_val;
	m_cbSpatial.MinRoughnessResample = p.GetFloat().m_val;
	m_cbDNSR.MinRoughnessResample = p.GetFloat().m_val;
}

void ReSTIR_GI_Specular::TemporalHistDistSigmaScaleCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporal.HitDistSigmaScale = p.GetFloat().m_val;
}

void ReSTIR_GI_Specular::SpatialHistDistSigmaScaleCallback(const Support::ParamVariant& p) noexcept
{
	m_cbSpatial.HitDistSigmaScale = p.GetFloat().m_val;
}

void ReSTIR_GI_Specular::NumIterationsCallback(const Support::ParamVariant& p) noexcept
{
	m_cbSpatial.NumIterations = (uint16_t)p.GetInt().m_val;
}

void ReSTIR_GI_Specular::SpatialRadiusCallback(const Support::ParamVariant& p) noexcept
{
	m_cbSpatial.Radius = (uint16_t)p.GetInt().m_val;
}

void ReSTIR_GI_Specular::CheckerboardingCallback(const Support::ParamVariant& p) noexcept
{
	m_cbTemporal.CheckerboardTracing = p.GetBool();
}

void ReSTIR_GI_Specular::ReloadTemporalPass() noexcept
{
	const int i = (int)SHADERS::TEMPORAL_RESAMPLE;

	s_rpObjs.m_psoLib.Reload(i, "IndirectSpecular\\ReSTIR_GI_Specular_Temporal.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void ReSTIR_GI_Specular::ReloadSpatialPass() noexcept
{
	const int i = (int)SHADERS::SPATIAL_RESAMPLE;

	s_rpObjs.m_psoLib.Reload(i, "IndirectSpecular\\ReSTIR_GI_Specular_Spatial.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void ReSTIR_GI_Specular::ReloadDNSR() noexcept
{
	const int i = (int)SHADERS::DNSR;

	s_rpObjs.m_psoLib.Reload(i, "IndirectSpecular\\SpecularDNSR_Temporal.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}


