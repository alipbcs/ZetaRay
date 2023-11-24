#include "IndirectLighting.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>
#include <Scene/SceneCore.h>
#include <Core/RenderGraph.h>
#include <Support/Task.h>

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;
using namespace ZetaRay::RT;
using namespace ZetaRay::Util;
using namespace ZetaRay::App;

//--------------------------------------------------------------------------------------
// IndirectLighting
//--------------------------------------------------------------------------------------

IndirectLighting::IndirectLighting()
	: RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// frame constants
	m_rootSig.InitAsCBV(0,
		0,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		GlobalResource::FRAME_CONSTANTS_BUFFER);

	// root constants
	m_rootSig.InitAsConstants(1,
		NUM_CONSTS,
		1,
		0);

	// BVH
	m_rootSig.InitAsBufferSRV(2,
		0,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		GlobalResource::RT_SCENE_BVH);

	// mesh buffer
	m_rootSig.InitAsBufferSRV(3,
		1,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		GlobalResource::RT_FRAME_MESH_INSTANCES);

	// scene VB
	m_rootSig.InitAsBufferSRV(4,
		2,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		GlobalResource::SCENE_VERTEX_BUFFER);

	// scene IB
	m_rootSig.InitAsBufferSRV(5,
		3,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		GlobalResource::SCENE_INDEX_BUFFER);

	// material buffer
	m_rootSig.InitAsBufferSRV(6,
		4,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		GlobalResource::MATERIAL_BUFFER);

	// emissive triangles
	m_rootSig.InitAsBufferSRV(7,
		5,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		GlobalResource::EMISSIVE_TRIANGLE_BUFFER,
		true);

	// sample sets
	m_rootSig.InitAsBufferSRV(8,
		6,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		GlobalResource::PRESAMPLED_EMISSIVE_SETS,
		true);

	// alias table
	m_rootSig.InitAsBufferSRV(9,
		7,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		GlobalResource::EMISSIVE_TRIANGLE_ALIAS_TABLE,
		true);

	// light voxel grid
	m_rootSig.InitAsBufferSRV(10,
		8,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		GlobalResource::LIGHT_VOXEL_GRID,
		true);
}

IndirectLighting::~IndirectLighting()
{
	Reset();
}

void IndirectLighting::Init()
{
	constexpr D3D12_ROOT_SIGNATURE_FLAGS flags =
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
	RenderPassBase::InitRenderPass("IndirectLighting", flags, samplers);

	TaskSet ts;

	for (int i = 0; i < (int)SHADERS::COUNT; i++)
	{
		StackStr(buff, n, "RGI_shader_%d", i);

		ts.EmplaceTask(buff, [i, this]()
			{
				m_psos[i] = m_psoLib.GetComputePSO_MT(i,
					m_rootSigObj.Get(),
					COMPILED_CS[i]);
			});
	}

	ts.Sort();
	ts.Finalize();
	App::Submit(ZetaMove(ts));

	m_descTable = renderer.GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);
	CreateOutputs();

	memset(&m_cbSpatioTemporal, 0, sizeof(m_cbSpatioTemporal));
	memset(&m_cbDnsrTemporal, 0, sizeof(m_cbDnsrTemporal));
	memset(&m_cbDnsrSpatial, 0, sizeof(m_cbDnsrSpatial));
	m_cbSpatioTemporal.M_max = DefaultParamVals::M_MAX;
	m_cbSpatioTemporal.NumBounces = 1;

	SET_CB_FLAG(m_cbSpatioTemporal, CB_IND_FLAGS::RUSSIAN_ROULETTE, true);
	SET_CB_FLAG(m_cbSpatioTemporal, CB_IND_FLAGS::STOCHASTIC_MULTI_BOUNCE, true);
	SET_CB_FLAG(m_cbSpatioTemporal, CB_IND_FLAGS::DENOISE, true);
	m_cbDnsrTemporal.MaxTsppDiffuse = m_cbDnsrSpatial.MaxTsppDiffuse = DefaultParamVals::DNSR_TSPP_DIFFUSE;
	m_cbDnsrTemporal.MaxTsppSpecular = m_cbDnsrSpatial.MaxTsppSpecular = DefaultParamVals::DNSR_TSPP_SPECULAR;
	m_cbDnsrTemporal.Denoise = m_cbDnsrSpatial.Denoise = true;
	m_cbDnsrSpatial.FilterDiffuse = true;
	m_cbDnsrSpatial.FilterSpecular = true;

	ParamVariant doTemporal;
	doTemporal.InitBool("Renderer", "Indirect Lighting", "Temporal Resample",
		fastdelegate::MakeDelegate(this, &IndirectLighting::TemporalResamplingCallback), m_doTemporalResampling);
	App::AddParam(doTemporal);

	ParamVariant rr;
	rr.InitBool("Renderer", "Indirect Lighting", "Russian Roulette",
		fastdelegate::MakeDelegate(this, &IndirectLighting::RussianRouletteCallback), 
		IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_IND_FLAGS::RUSSIAN_ROULETTE));
	App::AddParam(rr);

	ParamVariant stochasticMultibounce;
	stochasticMultibounce.InitBool("Renderer", "Indirect Lighting", "Stochastic Multi-bounce",
		fastdelegate::MakeDelegate(this, &IndirectLighting::StochasticMultibounceCallback), 
		IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_IND_FLAGS::STOCHASTIC_MULTI_BOUNCE));
	App::AddParam(stochasticMultibounce);

	ParamVariant numBounces;
	numBounces.InitInt("Renderer", "Indirect Lighting", "Max Num Bounces",
		fastdelegate::MakeDelegate(this, &IndirectLighting::NumBouncesCallback),
		m_cbSpatioTemporal.NumBounces,
		1,
		6,
		1);
	App::AddParam(numBounces);

	//ParamVariant doSpatial;
	//doSpatial.InitBool("Renderer", "Indirect Lighting", "Spatial Resample",
	//	fastdelegate::MakeDelegate(this, &IndirectLighting::SpatialResamplingCallback), m_doSpatialResampling);
	//App::AddParam(doSpatial);

	ParamVariant maxTemporalM;
	maxTemporalM.InitInt("Renderer", "Indirect Lighting", "M_max",
		fastdelegate::MakeDelegate(this, &IndirectLighting::MaxTemporalMCallback),
		m_cbSpatioTemporal.M_max,
		1,
		30,
		1);
	App::AddParam(maxTemporalM);

	ParamVariant denoise;
	denoise.InitBool("Renderer", "Indirect Lighting", "Denoise",
		fastdelegate::MakeDelegate(this, &IndirectLighting::DenoiseCallback), m_cbDnsrTemporal.Denoise);
	App::AddParam(denoise);

	ParamVariant tsppDiffuse;
	tsppDiffuse.InitInt("Renderer", "Indirect Lighting", "TSPP (Diffuse)",
		fastdelegate::MakeDelegate(this, &IndirectLighting::TsppDiffuseCallback),
		m_cbDnsrTemporal.MaxTsppDiffuse,			// val	
		1,											// min
		32,											// max
		1);											// step
	App::AddParam(tsppDiffuse);

	ParamVariant tsppSpecular;
	tsppSpecular.InitInt("Renderer", "Indirect Lighting", "TSPP (Specular)",
		fastdelegate::MakeDelegate(this, &IndirectLighting::TsppSpecularCallback),
		m_cbDnsrTemporal.MaxTsppSpecular,			// val	
		1,											// min
		32,											// max
		1);											// step
	App::AddParam(tsppSpecular);

	ParamVariant fireflyFilter;
	fireflyFilter.InitBool("Renderer", "Indirect Lighting", "Firefly Filter",
		fastdelegate::MakeDelegate(this, &IndirectLighting::FireflyFilterCallback), m_cbDnsrTemporal.FilterFirefly);
	App::AddParam(fireflyFilter);

	ParamVariant dnsrSpatialFilterDiffuse;
	dnsrSpatialFilterDiffuse.InitBool("Renderer", "Indirect Lighting", "Spatial Filter (Diffuse)",
		fastdelegate::MakeDelegate(this, &IndirectLighting::DnsrSpatialFilterDiffuseCallback), m_cbDnsrSpatial.FilterDiffuse);
	App::AddParam(dnsrSpatialFilterDiffuse);

	ParamVariant dnsrSpatialFilterSpecular;
	dnsrSpatialFilterSpecular.InitBool("Renderer", "Indirect Lighting", "Spatial Filter (Specular)",
		fastdelegate::MakeDelegate(this, &IndirectLighting::DnsrSpatialFilterSpecularCallback), m_cbDnsrSpatial.FilterSpecular);
	App::AddParam(dnsrSpatialFilterSpecular);

	App::AddShaderReloadHandler("ReSTIR_GI", fastdelegate::MakeDelegate(this, &IndirectLighting::ReloadSpatioTemporal));
	//App::AddShaderReloadHandler("IndirectDnsrTemporal", fastdelegate::MakeDelegate(this, &IndirectLighting::ReloadDnsrTemporal));
	//App::AddShaderReloadHandler("IndirectDnsrSpatial", fastdelegate::MakeDelegate(this, &IndirectLighting::ReloadDnsrSpatial));

	m_isTemporalReservoirValid = false;
}

void IndirectLighting::Reset()
{
	if (IsInitialized())
	{
		for (int i = 0; i < ZetaArrayLen(m_psos); i++)
			m_psos[i] = nullptr;

		m_descTable.Reset();

		RenderPassBase::ResetRenderPass();
	}
}

void IndirectLighting::OnWindowResized()
{
	CreateOutputs();
	m_isTemporalReservoirValid = false;
	m_currTemporalIdx = 0;
}

void IndirectLighting::Render(CommandList& cmdList)
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();
	const uint32_t w = renderer.GetRenderWidth();
	const uint32_t h = renderer.GetRenderHeight();

	computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

	// resampling
	{
		const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_GI_TEMPORAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_GI_TEMPORAL_GROUP_DIM_Y);

		SmallVector<D3D12_TEXTURE_BARRIER, SystemAllocator, 6> textureBarriers;

		// transition current temporal reservoir into write state
		textureBarriers.push_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[m_currTemporalIdx].ReservoirA.Resource(),
			D3D12_BARRIER_SYNC_NONE,
			D3D12_BARRIER_SYNC_COMPUTE_SHADING,
			D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
			D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
			D3D12_BARRIER_ACCESS_NO_ACCESS,
			D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
		textureBarriers.push_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[m_currTemporalIdx].ReservoirB.Resource(),
			D3D12_BARRIER_SYNC_NONE,
			D3D12_BARRIER_SYNC_COMPUTE_SHADING,
			D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
			D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
			D3D12_BARRIER_ACCESS_NO_ACCESS,
			D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
		textureBarriers.push_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[m_currTemporalIdx].ReservoirC.Resource(),
			D3D12_BARRIER_SYNC_NONE,
			D3D12_BARRIER_SYNC_COMPUTE_SHADING,
			D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
			D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
			D3D12_BARRIER_ACCESS_NO_ACCESS,
			D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));

		// transition color outputs into write state
		if (IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_IND_FLAGS::DENOISE))
		{
			textureBarriers.push_back(Direct3DUtil::TextureBarrier(m_colorA.Resource(),
				D3D12_BARRIER_SYNC_NONE,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_NO_ACCESS,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
			textureBarriers.push_back(Direct3DUtil::TextureBarrier(m_colorB.Resource(),
				D3D12_BARRIER_SYNC_NONE,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_NO_ACCESS,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
		}

		// transition previous reservoirs into read state
		if (m_isTemporalReservoirValid)
		{
			textureBarriers.push_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[1 - m_currTemporalIdx].ReservoirA.Resource(),
				D3D12_BARRIER_SYNC_NONE,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_ACCESS_NO_ACCESS,
				D3D12_BARRIER_ACCESS_SHADER_RESOURCE));
			textureBarriers.push_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[1 - m_currTemporalIdx].ReservoirB.Resource(),
				D3D12_BARRIER_SYNC_NONE,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_ACCESS_NO_ACCESS,
				D3D12_BARRIER_ACCESS_SHADER_RESOURCE));
			textureBarriers.push_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[1 - m_currTemporalIdx].ReservoirC.Resource(),
				D3D12_BARRIER_SYNC_NONE,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_ACCESS_NO_ACCESS,
				D3D12_BARRIER_ACCESS_SHADER_RESOURCE));
		}

		computeCmdList.ResourceBarrier(textureBarriers.data(), (UINT)textureBarriers.size());

		m_cbSpatioTemporal.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbSpatioTemporal.NumGroupsInTile = (uint16_t)(RESTIR_GI_TEMPORAL_TILE_WIDTH * dispatchDimY);

		SET_CB_FLAG(m_cbSpatioTemporal, CB_IND_FLAGS::TEMPORAL_RESAMPLE, 
			m_doTemporalResampling && m_isTemporalReservoirValid);
		SET_CB_FLAG(m_cbSpatioTemporal, CB_IND_FLAGS::SPATIAL_RESAMPLE, 
			m_doSpatialResampling && m_isTemporalReservoirValid);

		Assert(!m_preSampling || (m_cbSpatioTemporal.NumSampleSets && m_cbSpatioTemporal.SampleSetSize), 
			"Presampled set params haven't been set.");

		{
			auto srvAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_0_A_SRV : DESC_TABLE::RESERVOIR_1_A_SRV;
			auto srvBIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_0_B_SRV : DESC_TABLE::RESERVOIR_1_B_SRV;
			auto srvCIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_0_C_SRV : DESC_TABLE::RESERVOIR_1_C_SRV;
			auto uavAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_1_A_UAV : DESC_TABLE::RESERVOIR_0_A_UAV;
			auto uavBIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_1_B_UAV : DESC_TABLE::RESERVOIR_0_B_UAV;
			auto uavCIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_1_C_UAV : DESC_TABLE::RESERVOIR_0_C_UAV;

			m_cbSpatioTemporal.PrevReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
			m_cbSpatioTemporal.PrevReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
			m_cbSpatioTemporal.PrevReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvCIdx);
			m_cbSpatioTemporal.CurrReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
			m_cbSpatioTemporal.CurrReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);
			m_cbSpatioTemporal.CurrReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavCIdx);
		}

		m_cbSpatioTemporal.FinalOrColorAUavDescHeapIdx = IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_IND_FLAGS::DENOISE) ? 
			m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_A_UAV) : 
			m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::DNSR_FINAL_UAV);
		m_cbSpatioTemporal.ColorBUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_B_UAV);
		m_rootSig.SetRootConstants(0, sizeof(m_cbSpatioTemporal) / sizeof(DWORD), &m_cbSpatioTemporal);
		m_rootSig.End(computeCmdList);

		computeCmdList.PIXBeginEvent("ReSTIR_GI");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_GI");

		auto sh = SHADERS::SPATIO_TEMPORAL;
		if (m_preSampling)
			sh = SHADERS::SPATIO_TEMPORAL_LVG;
		else if(App::GetScene().NumEmissiveInstances() > 0)
			sh = SHADERS::SPATIO_TEMPORAL_NPS;

		computeCmdList.SetPipelineState(m_psos[(int)sh]);
		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();
	}

	if (IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_IND_FLAGS::DENOISE))
	{
		// denoiser - temporal
		{
			computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_TEMPORAL]);

			// record the timestamp prior to execution
			const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "IndirectDnsrTemporal");

			computeCmdList.PIXBeginEvent("IndirectDnsrTemporal");

			D3D12_TEXTURE_BARRIER barriers[4];

			// transition color into read state
			barriers[0] = Direct3DUtil::TextureBarrier(m_colorA.Resource(),
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
			barriers[1] = Direct3DUtil::TextureBarrier(m_colorB.Resource(),
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
			// transition current denoiser caches into write
			barriers[2] = Direct3DUtil::TextureBarrier(m_dnsrCache[m_currTemporalIdx].Diffuse.Resource(),
				D3D12_BARRIER_SYNC_NONE,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_NO_ACCESS,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
			barriers[3] = Direct3DUtil::TextureBarrier(m_dnsrCache[m_currTemporalIdx].Specular.Resource(),
				D3D12_BARRIER_SYNC_NONE,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_NO_ACCESS,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);

			computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

			auto srvDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV :
				DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV;
			auto srvSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV :
				DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV;
			auto uavDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV :
				DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV;
			auto uavSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV :
				DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV;

			m_cbDnsrTemporal.ColorASrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_A_SRV);
			m_cbDnsrTemporal.ColorBSrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_B_SRV);
			m_cbDnsrTemporal.PrevTemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvDiffuseIdx);
			m_cbDnsrTemporal.PrevTemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvSpecularIdx);
			m_cbDnsrTemporal.CurrTemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavDiffuseIdx);
			m_cbDnsrTemporal.CurrTemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavSpecularIdx);
			m_cbDnsrTemporal.IsTemporalCacheValid = m_isDnsrTemporalCacheValid;
			m_cbDnsrTemporal.CurvatureDescHeapIdx = m_cbSpatioTemporal.CurvatureDescHeapIdx;
			m_cbDnsrTemporal.PrevReservoir_A_DescHeapIdx = m_cbSpatioTemporal.PrevReservoir_A_DescHeapIdx;

			m_rootSig.SetRootConstants(0, sizeof(m_cbDnsrTemporal) / sizeof(DWORD), &m_cbDnsrTemporal);
			m_rootSig.End(computeCmdList);

			const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, INDIRECT_DNSR_TEMPORAL_GROUP_DIM_X);
			const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, INDIRECT_DNSR_TEMPORAL_GROUP_DIM_Y);
			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

			// record the timestamp after execution
			gpuTimer.EndQuery(computeCmdList, queryIdx);

			cmdList.PIXEndEvent();
		}

		// denoiser - spatial
		{
			computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_SPATIAL]);

			const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, INDIRECT_DNSR_SPATIAL_GROUP_DIM_X);
			const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, INDIRECT_DNSR_SPATIAL_GROUP_DIM_Y);

			// record the timestamp prior to execution
			const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "IndirectDnsrSpatial");

			computeCmdList.PIXBeginEvent("IndirectDnsrSpatial");

			D3D12_TEXTURE_BARRIER barriers[2];

			// transition color into read state
			barriers[0] = Direct3DUtil::TextureBarrier(m_dnsrCache[m_currTemporalIdx].Diffuse.Resource(),
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
			barriers[1] = Direct3DUtil::TextureBarrier(m_dnsrCache[m_currTemporalIdx].Specular.Resource(),
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

			computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

			auto srvDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV :
				DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV;
			auto srvSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV :
				DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV;

			m_cbDnsrSpatial.TemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvDiffuseIdx);
			m_cbDnsrSpatial.TemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvSpecularIdx);
			m_cbDnsrSpatial.ColorBSrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_B_SRV);
			m_cbDnsrSpatial.FinalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::DNSR_FINAL_UAV);
			m_cbDnsrSpatial.DispatchDimX = (uint16_t)dispatchDimX;
			m_cbDnsrSpatial.DispatchDimY = (uint16_t)dispatchDimY;
			m_cbDnsrSpatial.NumGroupsInTile = INDIRECT_DNSR_SPATIAL_TILE_WIDTH * m_cbDnsrSpatial.DispatchDimY;

			m_rootSig.SetRootConstants(0, sizeof(m_cbDnsrSpatial) / sizeof(DWORD), &m_cbDnsrSpatial);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

			// record the timestamp after execution
			gpuTimer.EndQuery(computeCmdList, queryIdx);

			cmdList.PIXEndEvent();
		}
	}

	m_isTemporalReservoirValid = true;
	m_currTemporalIdx = 1 - m_currTemporalIdx;
	m_isDnsrTemporalCacheValid = m_cbDnsrTemporal.Denoise;
	//m_currFrameCounter = m_currFrameCounter + 1 <= m_validationPeriod ? m_currFrameCounter + 1 : 0;
}

void IndirectLighting::CreateOutputs()
{
	auto& renderer = App::GetRenderer();

	auto func = [&renderer, this](Texture& tex, DXGI_FORMAT format, const char* name, 
		DESC_TABLE srv, DESC_TABLE uav)
	{
		tex = GpuMemory::GetTexture2D(name,
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			format,
			D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
			CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DUtil::CreateTexture2DSRV(tex, m_descTable.CPUHandle((int)srv));
		Direct3DUtil::CreateTexture2DUAV(tex, m_descTable.CPUHandle((int)uav));
	};

	// reservoirs
	{
		func(m_temporalReservoir[0].ReservoirA, ResourceFormats::RESERVOIR_A, "RGI_Reservoir_0_A",
			DESC_TABLE::RESERVOIR_0_A_SRV, DESC_TABLE::RESERVOIR_0_A_UAV);
		func(m_temporalReservoir[0].ReservoirB, ResourceFormats::RESERVOIR_B, "RGI_Reservoir_0_B",
			DESC_TABLE::RESERVOIR_0_B_SRV, DESC_TABLE::RESERVOIR_0_B_UAV);
		func(m_temporalReservoir[0].ReservoirC, ResourceFormats::RESERVOIR_C, "RGI_Reservoir_0_C",
			DESC_TABLE::RESERVOIR_0_C_SRV, DESC_TABLE::RESERVOIR_0_C_UAV);
		func(m_temporalReservoir[1].ReservoirA, ResourceFormats::RESERVOIR_A, "RGI_Reservoir_1_A",
			DESC_TABLE::RESERVOIR_1_A_SRV, DESC_TABLE::RESERVOIR_1_A_UAV);
		func(m_temporalReservoir[1].ReservoirB, ResourceFormats::RESERVOIR_B, "RGI_Reservoir_1_B",
			DESC_TABLE::RESERVOIR_1_B_SRV, DESC_TABLE::RESERVOIR_1_B_UAV);
		func(m_temporalReservoir[1].ReservoirC, ResourceFormats::RESERVOIR_C, "RGI_Reservoir_1_C",
			DESC_TABLE::RESERVOIR_1_C_SRV, DESC_TABLE::RESERVOIR_1_C_UAV);
	}

	{
		func(m_colorA, ResourceFormats::COLOR_A, "RGI_COLOR_A", DESC_TABLE::COLOR_A_SRV, DESC_TABLE::COLOR_A_UAV);
		func(m_colorB, ResourceFormats::COLOR_B, "RGI_COLOR_B", DESC_TABLE::COLOR_B_SRV, DESC_TABLE::COLOR_B_UAV);
	}

	// denoiser
	{
		func(m_dnsrCache[0].Diffuse, ResourceFormats::DNSR_TEMPORAL_CACHE, "IndirectDnsrDiffuse_0",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV);
		func(m_dnsrCache[1].Diffuse, ResourceFormats::DNSR_TEMPORAL_CACHE, "IndirectDnsrDiffuse_1",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV);
		func(m_dnsrCache[0].Specular, ResourceFormats::DNSR_TEMPORAL_CACHE, "IndirectDnsrSpecular_0",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV);
		func(m_dnsrCache[1].Specular, ResourceFormats::DNSR_TEMPORAL_CACHE, "IndirectDnsrSpecular_1",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV);

		m_denoised = GpuMemory::GetTexture2D("IndirectDenoised",
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			ResourceFormats::DNSR_TEMPORAL_CACHE,
			D3D12_RESOURCE_STATE_COMMON,
			CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DUtil::CreateTexture2DUAV(m_denoised, m_descTable.CPUHandle((int)DESC_TABLE::DNSR_FINAL_UAV));
	}
}

void IndirectLighting::NumBouncesCallback(const Support::ParamVariant& p)
{
	m_cbSpatioTemporal.NumBounces = (uint16_t)p.GetInt().m_val;
}

void IndirectLighting::StochasticMultibounceCallback(const Support::ParamVariant& p)
{
	SET_CB_FLAG(m_cbSpatioTemporal, CB_IND_FLAGS::STOCHASTIC_MULTI_BOUNCE, p.GetBool());
}

void IndirectLighting::RussianRouletteCallback(const Support::ParamVariant& p)
{
	SET_CB_FLAG(m_cbSpatioTemporal, CB_IND_FLAGS::RUSSIAN_ROULETTE, p.GetBool());
}

void IndirectLighting::TemporalResamplingCallback(const Support::ParamVariant& p)
{
	m_doTemporalResampling = p.GetBool();
}

void IndirectLighting::SpatialResamplingCallback(const Support::ParamVariant& p)
{
	m_doSpatialResampling = p.GetBool();
}

void IndirectLighting::MaxTemporalMCallback(const Support::ParamVariant& p)
{
	m_cbSpatioTemporal.M_max = (uint16_t) p.GetInt().m_val;
}

void IndirectLighting::DenoiseCallback(const Support::ParamVariant& p)
{
	m_isDnsrTemporalCacheValid = !m_cbDnsrTemporal.Denoise ? false : m_isDnsrTemporalCacheValid;
	SET_CB_FLAG(m_cbSpatioTemporal, CB_IND_FLAGS::DENOISE, p.GetBool());
	m_cbDnsrTemporal.Denoise = p.GetBool();
	m_cbDnsrSpatial.Denoise = p.GetBool();
	m_isDnsrTemporalCacheValid = !m_cbDnsrTemporal.Denoise ? false : m_isDnsrTemporalCacheValid;
}

void IndirectLighting::TsppDiffuseCallback(const Support::ParamVariant& p)
{
	m_cbDnsrTemporal.MaxTsppDiffuse = (uint16_t)p.GetInt().m_val;
	m_cbDnsrSpatial.MaxTsppDiffuse = (uint16_t)p.GetInt().m_val;
}

void IndirectLighting::TsppSpecularCallback(const Support::ParamVariant& p)
{
	m_cbDnsrTemporal.MaxTsppSpecular = (uint16_t)p.GetInt().m_val;
	m_cbDnsrSpatial.MaxTsppSpecular = (uint16_t)p.GetInt().m_val;
}

void IndirectLighting::FireflyFilterCallback(const Support::ParamVariant& p)
{
	m_cbDnsrTemporal.FilterFirefly = p.GetBool();
}

void IndirectLighting::DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p)
{
	m_cbDnsrSpatial.FilterDiffuse = p.GetBool();
}

void IndirectLighting::DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p)
{
	m_cbDnsrSpatial.FilterSpecular = p.GetBool();
}

void IndirectLighting::ReloadSpatioTemporal()
{
	auto sh = SHADERS::SPATIO_TEMPORAL;
	const char* p = "IndirectLighting\\ReSTIR_GI.hlsl";

	if (m_preSampling)
	{
		p = "IndirectLighting\\ReSTIR_GI_LVG.hlsl";
		sh = SHADERS::SPATIO_TEMPORAL_LVG;
	}
	else if (App::GetScene().NumEmissiveInstances() > 0)
	{
		p = "IndirectLighting\\ReSTIR_GI_NPS.hlsl";
		sh = SHADERS::SPATIO_TEMPORAL_NPS;
	}

	const int i = (int)sh;
	
	m_psoLib.Reload(i, p, true);
	m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}

void IndirectLighting::ReloadDnsrTemporal()
{
	const int i = (int)SHADERS::DNSR_TEMPORAL;

	m_psoLib.Reload(i, "IndirectLighting\\IndirectDnsr_Temporal.hlsl", true);
	m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}

void IndirectLighting::ReloadDnsrSpatial()
{
	const int i = (int)SHADERS::DNSR_SPATIAL;

	m_psoLib.Reload(i, "IndirectLighting\\IndirectDnsr_Spatial.hlsl", true);
	m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}
