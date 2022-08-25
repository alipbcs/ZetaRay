#include "SVGF.h"
#include "../../Core/Renderer.h"
#include "../../Core/CommandList.h"
#include "../../Scene/SceneRenderer/SceneRenderer.h"
#include "../../Win32/App.h"
#include "../../SupportSystem/Param.h"

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DHelper;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// SVGF
//--------------------------------------------------------------------------------------

SVGF::SVGF() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
}

SVGF::~SVGF() noexcept
{
	Reset();
}

void SVGF::Init() noexcept
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

	s_rpObjs.Init("SVGF", m_rootSig, RendererConstants::NUM_STATIC_SAMPLERS, samplers, flags);

	m_psos[(int)SHADERS::TEMPORAL_FILTER] = s_rpObjs.m_psoLib.GetComputePSO((int)SHADERS::TEMPORAL_FILTER,
		s_rpObjs.m_rootSig.Get(), COMPILED_CS[(int)SHADERS::TEMPORAL_FILTER]);
	m_psos[(int)SHADERS::SPATIAL_VARIANCE] = s_rpObjs.m_psoLib.GetComputePSO((int)SHADERS::SPATIAL_VARIANCE,
		s_rpObjs.m_rootSig.Get(), COMPILED_CS[(int)SHADERS::SPATIAL_VARIANCE]);
	m_psos[(int)SHADERS::GAUSSIAN_FILTER] = s_rpObjs.m_psoLib.GetComputePSO((int)SHADERS::GAUSSIAN_FILTER,
		s_rpObjs.m_rootSig.Get(), COMPILED_CS[(int)SHADERS::GAUSSIAN_FILTER]);
	m_psos[(int)SHADERS::ATROUS_WAVELET_TRANSFORM] = s_rpObjs.m_psoLib.GetComputePSO((int)SHADERS::ATROUS_WAVELET_TRANSFORM,
		s_rpObjs.m_rootSig.Get(), COMPILED_CS[(int)SHADERS::ATROUS_WAVELET_TRANSFORM]);

	m_descTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((int)DESC_TABLE::COUNT);
	CreateResources();

	InitParams();

	m_cbTemporalFilter.IsTemporalCacheValid = false;
	m_cbTemporalFilter.MaxTspp = DefaultParamVals::MaxTSPP;
	m_cbTemporalFilter.MinTsppToUseTemporalVar = DefaultParamVals::MinTSPPtoUseTemporalVar;
	m_cbTemporalFilter.MinConsistentWeight = DefaultParamVals::MinConsistentWeight;
	m_cbTemporalFilter.BilinearNormalScale = DefaultParamVals::BilinearNormalScale;
	m_cbTemporalFilter.BilinearNormalExp = DefaultParamVals::BilinearNormalExp;
	m_cbTemporalFilter.BilinearGeometryMaxPlaneDist = DefaultParamVals::BilinearGeometryMaxPlaneDist;
	//m_cbTemporalFilter.BilinearDepthCutoff = DefaultParamVals::BilinearDepthCutoff;
	//m_cbTemporalFilter.MinLumVariance = DefaultParamVals::MinLumVariance;
	//m_cbTemporalFilter.ClampHistory = DefaultParamVals::NeighborhoodClamping;
	//m_cbTemporalFilter.ClampingMinStd = DefaultParamVals::NeighborhoodClampingMinStd;
	//m_cbTemporalFilter.ClampingStdScale = DefaultParamVals::NeighborhoodClampingStdScale;
	//m_cbTemporalFilter.ClampingTsppAdjustmentScaleByDifference = DefaultParamVals::NeighborhoodClampingTSPPAdj;

	m_cbSpatialVar.Radius = DefaultParamVals::SpatialVarianceRadius;

	m_cbWaveletTransform.DepthWeightCutoff = DefaultParamVals::EdgeStoppingDepthWeightCutoff;
	m_cbWaveletTransform.DepthSigma = DefaultParamVals::EdgeStoppingDepthSigma;
	m_cbWaveletTransform.NormalSigma = DefaultParamVals::EdgeStoppingNormalSigma;
	m_cbWaveletTransform.LumSigma = DefaultParamVals::EdgeStoppingLumSigma;
	//m_cbWaveletTransform.MinVarianceToFilter = DefaultParamVals::MinVarianceToFilter;

	App::AddShaderReloadHandler("SVGF_SpatialVar", fastdelegate::MakeDelegate(this, &SVGF::ReloadSpatialVar));
	App::AddShaderReloadHandler("SVGF_Temporal", fastdelegate::MakeDelegate(this, &SVGF::ReloadTemporalPass));
	App::AddShaderReloadHandler("SVGF_GaussianFilter", fastdelegate::MakeDelegate(this, &SVGF::ReloadGaussianFilter));
	App::AddShaderReloadHandler("SVGF_WaveletTransform", fastdelegate::MakeDelegate(this, &SVGF::ReloadWaveletFilter));
}

void SVGF::Reset() noexcept
{
	if (IsInitialized())
	{
		s_rpObjs.Clear();
		App::RemoveShaderReloadHandler("SVGF_SpatialVar");
		App::RemoveShaderReloadHandler("SVGF_SpatialVar");
		App::RemoveShaderReloadHandler("SVGF_GaussianFilter");
		App::RemoveShaderReloadHandler("SVGF_WaveletTransform");
	}

	memset(m_inputGpuHeapIndices, 0, (int)SHADER_IN_RES::COUNT * sizeof(uint32_t));
	memset(m_psos, 0, (int)SHADERS::COUNT * sizeof(ID3D12PipelineState*));

	m_spatialLumVar.Reset();
	m_descTable.Reset();
	m_temporalCacheColLum[0].Reset();
	m_temporalCacheColLum[1].Reset();
	//m_temporalFilterBuff.Reset();
	//m_spatialVarBuff.Reset();
	//m_waveletTransformBuff->Reset();

	m_descTable.Reset();

	m_isTemporalCacheValid = false;
}

void SVGF::OnWindowResized() noexcept
{
	CreateResources();
}

void SVGF::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	const int w = renderer.GetRenderWidth();
	const int h = renderer.GetRenderHeight();
	const int outIdx = renderer.CurrOutIdx();
	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

	// spatial variance
	{
		Assert(m_inputGpuHeapIndices[(int)SHADER_IN_RES::INDIRECT_LI] != 0, "Input descriptor heap idx hasn't been set.");

		computeCmdList.PIXBeginEvent("SVGF_SpatialVariance");
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::SPATIAL_VARIANCE]);

		m_cbSpatialVar.IndirectLiRayTDescHeapIdx = m_inputGpuHeapIndices[(int)SHADER_IN_RES::INDIRECT_LI];
		m_cbSpatialVar.SpatialLumVarDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::SPATIAL_LUM_VAR_UAV);
		m_rootSig.SetRootConstants(0, sizeof(cbSpatialVar) / sizeof(DWORD), &m_cbSpatialVar);

		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch((UINT)CeilUnsignedIntDiv(w, TEMPORAL_FILTER_THREAD_GROUP_SIZE_X),
			(UINT)CeilUnsignedIntDiv(h, TEMPORAL_FILTER_THREAD_GROUP_SIZE_Y),
			TEMPORAL_FILTER_THREAD_GROUP_SIZE_Z);

		computeCmdList.PIXEndEvent();
	}

	// gaussian filter
	if (m_filterSpatialVariance)
	{
		computeCmdList.PIXBeginEvent("SVGF_GaussianFilter");
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::GAUSSIAN_FILTER]);

		m_cbGaussianFilter.SpatialLumVarDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::SPATIAL_LUM_VAR_UAV);
		m_cbGaussianFilter.SpatialLumVarFilteredDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::SPATIAL_LUM_VAR_FILTERED_UAV);

		m_rootSig.SetRootConstants(0, sizeof(cbGaussianFilter) / sizeof(DWORD), &m_cbGaussianFilter);
		m_rootSig.End(computeCmdList);

		for (int i = 0; i < 1; i++)
		{
			// issue a UAV barrier to make sure spatial variance pass is done writing to it
			auto barrier = Direct3DHelper::UAVBarrier(m_spatialLumVar.GetResource());
			computeCmdList.UAVBarrier(1, &barrier);

			computeCmdList.Dispatch((UINT)CeilUnsignedIntDiv(w, GAUSSAIN_FILT_THREAD_GROUP_SIZE_X),
				(UINT)CeilUnsignedIntDiv(h, GAUSSAIN_FILT_THREAD_GROUP_SIZE_Y),
				GAUSSAIN_FILT_THREAD_GROUP_SIZE_Z);
		}

		computeCmdList.PIXEndEvent();
	}

	const uint32_t prevTemporalCacheSRV = outIdx == 0 ? (int)DESC_TABLE::TEMPORAL_CACHE_COL_LUM_A_SRV : (int)DESC_TABLE::TEMPORAL_CACHE_COL_LUM_B_SRV;
	const uint32_t nextTemporalCacheUAV = outIdx == 0 ? (int)DESC_TABLE::TEMPORAL_CACHE_COL_LUM_B_UAV : (int)DESC_TABLE::TEMPORAL_CACHE_COL_LUM_A_UAV;


	// temporal filter
	{
		Assert(m_inputGpuHeapIndices[(int)SHADER_IN_RES::LINEAR_DEPTH_GRAD] != 0, "LINEAR_DEPTH_GRAD descriptor heap idx was not set.");

		computeCmdList.PIXBeginEvent("SVGF_TemporalFilter");
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::TEMPORAL_FILTER]);

		m_cbTemporalFilter.LinearDepthGradDescHeapIdx = m_inputGpuHeapIndices[(int)SHADER_IN_RES::LINEAR_DEPTH_GRAD];
		m_cbTemporalFilter.IndirectLiRayTDescHeapIdx = m_inputGpuHeapIndices[(int)SHADER_IN_RES::INDIRECT_LI];
		m_cbTemporalFilter.PrevTemporalCacheDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(prevTemporalCacheSRV);
		m_cbTemporalFilter.CurrTemporalCacheDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(nextTemporalCacheUAV);
		m_cbTemporalFilter.SpatialLumVarDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::SPATIAL_LUM_VAR_UAV);
		m_cbTemporalFilter.IsTemporalCacheValid = m_isTemporalCacheValid;

		m_rootSig.SetRootConstants(0, sizeof(cbTemporalFilter) / sizeof(DWORD), &m_cbTemporalFilter);
		m_rootSig.End(computeCmdList);

		// issue a UAV barrier to make sure spatial variance pass is done writing to it
		auto barrier = Direct3DHelper::UAVBarrier(m_spatialLumVar.GetResource());
		computeCmdList.UAVBarrier(1, &barrier);

		computeCmdList.Dispatch((uint32_t)CeilUnsignedIntDiv(w, TEMPORAL_FILTER_THREAD_GROUP_SIZE_X),
			(uint32_t)CeilUnsignedIntDiv(h, TEMPORAL_FILTER_THREAD_GROUP_SIZE_Y),
			TEMPORAL_FILTER_THREAD_GROUP_SIZE_Z);

		computeCmdList.PIXEndEvent();
	}

	// atrous wavelet transform
	if (m_waveletTransform)
	{
		computeCmdList.PIXBeginEvent("SVGF_AtrousWaveletTransform");

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::ATROUS_WAVELET_TRANSFORM]);

		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, WAVELET_TRANSFORM_THREAD_GROUP_SIZE_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, WAVELET_TRANSFORM_THREAD_GROUP_SIZE_Y);

		m_cbWaveletTransform.DispatchDimX = dispatchDimX;
		m_cbWaveletTransform.DispatchDimY = dispatchDimY;
		m_cbWaveletTransform.TileWidth = 16;
		m_cbWaveletTransform.Log2TileWidth = 4;
		m_cbWaveletTransform.NumGroupsInTile = m_cbWaveletTransform.TileWidth * m_cbWaveletTransform.DispatchDimY;

		Assert((1 << m_cbWaveletTransform.Log2TileWidth) == m_cbWaveletTransform.TileWidth, "these must be equal");

		m_cbWaveletTransform.LinearDepthGradDescHeapIdx = m_inputGpuHeapIndices[(int)SHADER_IN_RES::LINEAR_DEPTH_GRAD];
		m_cbWaveletTransform.IntegratedTemporalCacheDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(nextTemporalCacheUAV);
		m_cbWaveletTransform.LumVarianceDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::SPATIAL_LUM_VAR_UAV);

		Texture& nextTemporalCache = outIdx == 0 ? m_temporalCacheColLum[1] : m_temporalCacheColLum[0];

		for (int i = 0; i < m_numWaveletFilterPasses; i++)
		{
			D3D12_RESOURCE_BARRIER uavBarriers[] = {
				Direct3DHelper::UAVBarrier(m_spatialLumVar.GetResource()),
				Direct3DHelper::UAVBarrier(nextTemporalCache.GetResource()) };

			computeCmdList.UAVBarrier(ArraySize(uavBarriers), uavBarriers);

			m_cbWaveletTransform.Step = 1 << i;

			m_rootSig.SetRootConstants(0, sizeof(cbAtrousWaveletFilter) / sizeof(DWORD), &m_cbWaveletTransform);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
		}

		computeCmdList.PIXEndEvent();
	}

	// ping-pong between temporal caches
	//std::swap(m_temporalCache[0], m_temporalCache[1]);

	m_isTemporalCacheValid = true;
}

void SVGF::CreateResources() noexcept
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
		m_temporalCacheColLum[0] = renderer.GetGpuMemory().GetTexture2D("SVGF_TEMPORAL_CACHE_A",
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			ResourceFormats::TEMPORAL_CACHE_COLOR_LUM,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		m_temporalCacheColLum[1] = renderer.GetGpuMemory().GetTexture2D("SVGF_TEMPORAL_CACHE_B",
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			ResourceFormats::TEMPORAL_CACHE_COLOR_LUM,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		srvDesc.Format = ResourceFormats::TEMPORAL_CACHE_COLOR_LUM;
		uavDesc.Format = ResourceFormats::TEMPORAL_CACHE_COLOR_LUM;

		device->CreateShaderResourceView(m_temporalCacheColLum[0].GetResource(),
			&srvDesc,
			m_descTable.CPUHandle((int)DESC_TABLE::TEMPORAL_CACHE_COL_LUM_A_SRV));
		device->CreateUnorderedAccessView(m_temporalCacheColLum[0].GetResource(),
			nullptr,
			&uavDesc,
			m_descTable.CPUHandle((int)DESC_TABLE::TEMPORAL_CACHE_COL_LUM_A_UAV));

		device->CreateShaderResourceView(m_temporalCacheColLum[1].GetResource(),
			&srvDesc,
			m_descTable.CPUHandle((int)DESC_TABLE::TEMPORAL_CACHE_COL_LUM_B_SRV));
		device->CreateUnorderedAccessView(m_temporalCacheColLum[1].GetResource(),
			nullptr,
			&uavDesc,
			m_descTable.CPUHandle((int)DESC_TABLE::TEMPORAL_CACHE_COL_LUM_B_UAV));
	}

	// color mean-variance + lum variance
	{
		m_spatialLumVar = renderer.GetGpuMemory().GetTexture2D("SVGF_LUM_VAR",
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			ResourceFormats::SPATIAL_LUM_VAR,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		// UAV
		uavDesc.Format = ResourceFormats::SPATIAL_LUM_VAR;

		device->CreateUnorderedAccessView(m_spatialLumVar.GetResource(), nullptr, &uavDesc,
			m_descTable.CPUHandle((int)DESC_TABLE::SPATIAL_LUM_VAR_UAV));
	}

	// gaussian filter
	{
		m_spatialLumVarFiltered = renderer.GetGpuMemory().GetTexture2D("SVGF_LUM_VAR_FILTERED",
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			ResourceFormats::SPATIAL_LUM_VAR,
			D3D12_RESOURCE_STATE_COMMON,
			TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		uavDesc.Texture2D.PlaneSlice = 0;
		uavDesc.Format = ResourceFormats::SPATIAL_LUM_VAR;

		device->CreateUnorderedAccessView(m_spatialLumVarFiltered.GetResource(), nullptr, &uavDesc,
			m_descTable.CPUHandle((int)DESC_TABLE::SPATIAL_LUM_VAR_FILTERED_UAV));
	}
}

void SVGF::InitParams() noexcept
{
	ParamVariant waveletTransformPass;
	waveletTransformPass.InitBool("Renderer", "SVGF", "AtrousWaveletTransform",
		fastdelegate::MakeDelegate(this, &SVGF::WaveletFilterCallback),
		m_waveletTransform);
	App::AddParam(waveletTransformPass);

	ParamVariant maxTSPP;
	maxTSPP.InitInt("Renderer", "SVGF", "MaxTSPP", fastdelegate::MakeDelegate(this, &SVGF::MaxTSPPCallback),
		DefaultParamVals::MaxTSPP,		// val
		1,								// min
		32,								// max
		1);								// step
	App::AddParam(maxTSPP);

	ParamVariant minTSPPtoUseTemporalVar;
	minTSPPtoUseTemporalVar.InitInt("Renderer", "SVGF", "MinTSPPTemporalVar",
		fastdelegate::MakeDelegate(this, &SVGF::MinTSPPForTemporalVarCallback),
		DefaultParamVals::MinTSPPtoUseTemporalVar,		// val	
		1,												// min
		40,												// max
		1);												// step
	App::AddParam(minTSPPtoUseTemporalVar);

	// adjustment for this wasn't exposed (fixed at 1.1) 
	// TemporalSupersampling_ReverseReprojectCS.hlsl, line 56
	ParamVariant bilateralNormalScale;
	bilateralNormalScale.InitFloat("Renderer", "SVGF", "BilinearNormalScale",
		fastdelegate::MakeDelegate(this, &SVGF::BilinearNormalScaleCallback),
		DefaultParamVals::BilinearNormalScale,		// val	
		1.0f,										// min
		5.0f,										// max
		0.1f);										// step
	App::AddParam(bilateralNormalScale);

	// adjustment for this wasn't exposed (fixed at 32) 
	// TemporalSupersampling_ReverseReprojectCS.hlsl, line 57
	ParamVariant bilateralNormalExp;
	bilateralNormalExp.InitFloat("Renderer", "SVGF", "BilinearNormalExp",
		fastdelegate::MakeDelegate(this, &SVGF::BilinearNormalExpCallback),
		DefaultParamVals::BilinearNormalExp,		// val	
		16.0f,										// min
		128.0f,										// max
		1.0f);										// step	
	App::AddParam(bilateralNormalExp);

	ParamVariant bilateralDepthScale;
	bilateralDepthScale.InitFloat("Renderer", "SVGF", "MaxPlaneDist",
		fastdelegate::MakeDelegate(this, &SVGF::BilinearGeometryMaxPlaneDistCallback),
		DefaultParamVals::BilinearGeometryMaxPlaneDist,		// val	
		0.1e-3f,											// min
		1.0f,												// max
		0.1e-3f);											// step
	App::AddParam(bilateralDepthScale);

	// adjustment for this wasn't exposed (fixed at 0.5) 
	// TemporalSupersampling_ReverseReprojectCS.hlsl, line 53
	//ParamVariant bilateralDepthCutoff;
	//bilateralDepthCutoff.InitFloat("Renderer", "SVGF", "DepthConsCutoff",
	//	fastdelegate::MakeDelegate(this, &SVGF::BilateralDepthCutoffCallback),
	//	DefaultParamVals::BilinearDepthCutoff,		// val	
	//	0.0f,										// min
	//	1.0f,										// max
	//	0.01f);										// step
	//App::AddParam(bilateralDepthCutoff);

	//	ParamVariant neighborhoodClamping;
	//	neighborhoodClamping.InitBool("Renderer", "SVGF", "NeighborhoodClamping", 
	//		fastdelegate::MakeDelegate(this, &SVGF::NeighborhoodClampingCallback),
	//		DefaultParamVals::NeighborhoodClamping);
	//	App::AddParam(neighborhoodClamping);

	//	ParamVariant neighborhoodClampingStdScale;
	//	neighborhoodClampingStdScale.InitFloat("Renderer", "SVGF", "NeighborhoodClampingStdScale", 
	//		fastdelegate::MakeDelegate(this, &SVGF::NeighborhoodClampingStdScaleCallback),
	//		DefaultParamVals::NeighborhoodClampingStdScale,		// val	
	//		0.1f,												// min
	//		10.0f,												// max
	//		0.1f);												// step
	//	App::AddParam(neighborhoodClampingStdScale);

	//	ParamVariant neighborhoodClampingMinStd;
	//	neighborhoodClampingMinStd.InitFloat("Renderer", "SVGF", "NeighborhoodClampingMinStd", 
	//		fastdelegate::MakeDelegate(this, &SVGF::NeighborhoodClampingMinStdCallback),
	//		DefaultParamVals::NeighborhoodClampingMinStd,		// val	
	//		0.0f,												// min
	//		1.0f,												// max
	//		0.01f);												// step
	//	App::AddParam(neighborhoodClampingMinStd);

	//	ParamVariant neighborhoodClampingTSPP;
	//	neighborhoodClampingTSPP.InitFloat("Renderer", "SVGF", "NeighborhoodClampingTSPPAdjustment", 
	//		fastdelegate::MakeDelegate(this, &SVGF::NeighborhoodClampingTSPPAdjCallback),
	//		DefaultParamVals::NeighborhoodClampingTSPPAdj,		// val	
	//		0.0f,												// min
	//		10.0f,												// max
	//		0.05f);												// step
	//	App::AddParam(neighborhoodClampingTSPP);

		// adjustment for this wasn't exposed (fixed at 0.1) 
		// TemporalSupersampling_BlendWithCurrentFrameCS.hlsl, line 95
		//ParamVariant minLumVariance;
		//minLumVariance.InitFloat("Renderer", "SVGF", "MinLumVar", 
		//	fastdelegate::MakeDelegate(this, &SVGF::MinLumVarCallback),
		//	DefaultParamVals::MinLumVariance,		// val	
		//	0.0f,									// min
		//	5.0f,									// max
		//	0.1f);									// step
		//App::AddParam(minLumVariance);

		// adjustment for this wasn't exposed (fixed at 1e-3) 
		// TemporalSupersampling_ReverseReprojectCS.hlsl, line 154
	ParamVariant minConsistentWeight;
	minConsistentWeight.InitFloat("Renderer", "SVGF", "MinConsWeight",
		fastdelegate::MakeDelegate(this, &SVGF::MinConsistentWeightCallback),
		DefaultParamVals::MinConsistentWeight,		// val	
		0.0f,										// min
		0.1f,										// max
		1e-2f);										// step
	App::AddParam(minConsistentWeight);

	ParamVariant spatialVarianceRadius;
	spatialVarianceRadius.InitInt("Renderer", "SVGF", "SpatialVarRadius",
		fastdelegate::MakeDelegate(this, &SVGF::SpatialVarRadiusCallback),
		DefaultParamVals::SpatialVarianceRadius,		// val	
		1,												// min
		4,												// max
		1);												// step
	App::AddParam(spatialVarianceRadius);

	ParamVariant filterSpatialVariance;
	filterSpatialVariance.InitBool("Renderer", "SVGF", "FilterSpatialVariance",
		fastdelegate::MakeDelegate(this, &SVGF::FilterSpatialVarCallback),
		m_filterSpatialVariance);
	App::AddParam(filterSpatialVariance);

	//ParamVariant minVarianceToFilter;
	//minVarianceToFilter.InitFloat("Renderer", "SVGF", "MinVarToFilter", 
	//	fastdelegate::MakeDelegate(this, &SVGF::MinVarToFilterCallback),
	//	DefaultParamVals::MinVarianceToFilter,		// val	
	//	0.0f,										// min
	//	1.0f,										// max
	//	0.01f);										// step
	//App::AddParam(minVarianceToFilter);

	ParamVariant edgeStoppingDepthWeightCutoff;
	edgeStoppingDepthWeightCutoff.InitFloat("Renderer", "SVGF", "EdgeStoppingDepthWeightCutoff",
		fastdelegate::MakeDelegate(this, &SVGF::EdgeStoppingDepthWeightCutoffCallback),
		DefaultParamVals::EdgeStoppingDepthWeightCutoff,		// val	
		0.0f,													// min
		2.0f,													// max
		0.01f);													// step
	App::AddParam(edgeStoppingDepthWeightCutoff);

	ParamVariant edgeStoppingLumSigma;
	edgeStoppingLumSigma.InitFloat("Renderer", "SVGF", "EdgeStoppingLumSigma",
		fastdelegate::MakeDelegate(this, &SVGF::EdgeStoppingLumSigmaCallback),
		DefaultParamVals::EdgeStoppingLumSigma,		// val	
		0.1f,										// min
		30.0f,										// max
		0.1f);										// step
	App::AddParam(edgeStoppingLumSigma);

	ParamVariant edgeStoppingNormalSigma;
	edgeStoppingNormalSigma.InitFloat("Renderer", "SVGF", "EdgeStoppingNormalSigma",
		fastdelegate::MakeDelegate(this, &SVGF::EdgeStoppingNormalSigmaCallback),
		DefaultParamVals::EdgeStoppingNormalSigma,		// val	
		1.0f,											// min
		256.0f,											// max
		4.0f);											// step
	App::AddParam(edgeStoppingNormalSigma);

	ParamVariant edgeStoppingDepthSigma;
	edgeStoppingDepthSigma.InitFloat("Renderer", "SVGF", "EdgeStoppingDepthSigma",
		fastdelegate::MakeDelegate(this, &SVGF::EdgeStoppingDepthSigmaCallback),
		DefaultParamVals::EdgeStoppingDepthSigma,		// val	
		0.0f,											// min
		10.0f,											// max
		0.02f);											// step
	App::AddParam(edgeStoppingDepthSigma);

	ParamVariant numWaveletFilterPasses;
	numWaveletFilterPasses.InitInt("Renderer", "SVGF", "#WaveletTransformPasses",
		fastdelegate::MakeDelegate(this, &SVGF::NumWaveletPassesCallback),
		DefaultParamVals::NumWaveletTransformPasses,		// val	
		1,													// min
		5,													// max
		1);													// step
	App::AddParam(numWaveletFilterPasses);
}

void SVGF::MaxTSPPCallback(const ParamVariant& p) noexcept
{
	m_cbTemporalFilter.MaxTspp = p.GetInt().m_val;
}

void SVGF::MinTSPPForTemporalVarCallback(const ParamVariant& p) noexcept
{
	m_cbTemporalFilter.MinTsppToUseTemporalVar = p.GetInt().m_val;
}

void SVGF::BilinearNormalScaleCallback(const ParamVariant& p) noexcept
{
	m_cbTemporalFilter.BilinearNormalScale = p.GetFloat().m_val;
}

void SVGF::BilinearNormalExpCallback(const ParamVariant& p) noexcept
{
	m_cbTemporalFilter.BilinearNormalExp = p.GetFloat().m_val;
}

void SVGF::BilinearGeometryMaxPlaneDistCallback(const ParamVariant& p) noexcept
{
	m_cbTemporalFilter.BilinearGeometryMaxPlaneDist = p.GetFloat().m_val;
}

void SVGF::MinLumVarCallback(const ParamVariant& p) noexcept
{
	m_cbTemporalFilter.MinLumVariance = p.GetFloat().m_val;
}

void SVGF::MinConsistentWeightCallback(const ParamVariant& p) noexcept
{
	m_cbTemporalFilter.MinConsistentWeight = p.GetFloat().m_val;
}

void SVGF::SpatialVarRadiusCallback(const ParamVariant& p) noexcept
{
	m_cbSpatialVar.Radius = p.GetInt().m_val;
}

void SVGF::FilterSpatialVarCallback(const ParamVariant& p) noexcept
{
	m_filterSpatialVariance = p.GetBool();
}

void SVGF::MinVarToFilterCallback(const ParamVariant& p) noexcept
{
	m_cbWaveletTransform.MinVarianceToFilter = p.GetFloat().m_val;
}

void SVGF::EdgeStoppingDepthWeightCutoffCallback(const ParamVariant& p) noexcept
{
	m_cbWaveletTransform.DepthWeightCutoff = p.GetFloat().m_val;
}

void SVGF::EdgeStoppingLumSigmaCallback(const ParamVariant& p) noexcept
{
	m_cbWaveletTransform.LumSigma = p.GetFloat().m_val;
}

void SVGF::EdgeStoppingNormalSigmaCallback(const ParamVariant& p) noexcept
{
	m_cbWaveletTransform.NormalSigma = p.GetFloat().m_val;
}

void SVGF::EdgeStoppingDepthSigmaCallback(const ParamVariant& p) noexcept
{
	m_cbWaveletTransform.DepthSigma = p.GetFloat().m_val;
}

void SVGF::NumWaveletPassesCallback(const ParamVariant& p) noexcept
{
	m_numWaveletFilterPasses = p.GetInt().m_val;
}

void SVGF::WaveletFilterCallback(const ParamVariant& p) noexcept
{
	m_waveletTransform = p.GetBool();
}

void SVGF::ReloadSpatialVar() noexcept
{
	const int i = (int)SHADERS::SPATIAL_VARIANCE;

	s_rpObjs.m_psoLib.Reload(i, "SVGF\\SVGF_SpatialVariance.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void SVGF::ReloadTemporalPass() noexcept
{
	const int i = (int)SHADERS::TEMPORAL_FILTER;

	s_rpObjs.m_psoLib.Reload(i, "SVGF\\SVGF_TemporalFilter.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void SVGF::ReloadGaussianFilter() noexcept
{
	const int i = (int)SHADERS::GAUSSIAN_FILTER;

	s_rpObjs.m_psoLib.Reload(i, "SVGF\\SVGF_GaussianFilter.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void SVGF::ReloadWaveletFilter() noexcept
{
	const int i = (int)SHADERS::ATROUS_WAVELET_TRANSFORM;

	s_rpObjs.m_psoLib.Reload(i, "SVGF\\SVGF_AtrousWaveletTransform.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}