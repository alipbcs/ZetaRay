#include "ReSTIR.h"
#include "../../Core/Renderer.h"
#include "../../Core/CommandList.h"
#include "../../Scene/SceneRenderer/SceneRenderer.h"
#include "../../Math/Sampling.h"
#include "../../Win32/App.h"
#include "../../Win32/Timer.h"

using namespace ZetaRay;
using namespace ZetaRay::Direct3DHelper;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;

//--------------------------------------------------------------------------------------
// ReSTIR
//--------------------------------------------------------------------------------------

ReSTIR::ReSTIR() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
}

ReSTIR::~ReSTIR() noexcept
{
	if (IsInitialized())
		s_rpObjs.Clear();
}

void ReSTIR::Init() noexcept
{
	Assert(!IsInitialized(), "attempting to reinitialize");

	//
	// root signature
	//

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

	// frame constants
	m_rootSig.InitAsCBV(0,												// root idx
		0,																// register
		0,																// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_ALL,									// visibility
		SceneRenderer::FRAME_CONSTANTS_BUFFER_NAME);

	// local
	m_rootSig.InitAsCBV(1,								// root idx
		1,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL);					// visibility

	// BVH
	m_rootSig.InitAsBufferSRV(2,						// root idx
		0,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		SceneRenderer::RT_SCENE_BVH);

	// env-map patches
	m_rootSig.InitAsBufferSRV(3,						// root idx
		1,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		SceneRenderer::ENV_LIGHT_PATCH_BUFFER);					

	// reservoir
	m_rootSig.InitAsBufferSRV(4,						// root idx
		2,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL);					// visibility

	// alias table
	m_rootSig.InitAsBufferSRV(5,						// root idx
		3,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		SceneRenderer::ENV_MAP_ALIAS_TABLE);					

	// halton sequence
	m_rootSig.InitAsBufferSRV(6,						// root idx
		4,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL);					// visibility

	// reservoir
	m_rootSig.InitAsBufferUAV(7,						// root idx
		0,												// register
		0,												// register-space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL);					// visibility

	s_rpObjs.Init("ReSTIR", m_rootSig, RendererConstants::NUM_STATIC_SAMPLERS, samplers, flags);

	//
	// shaders
	//

	m_psos[(int)SHADERS::TEMPORAL_FILTER] = s_rpObjs.m_psoLib.GetComputePSO((int)SHADERS::TEMPORAL_FILTER,
		s_rpObjs.m_rootSig.Get(), COMPILED_CS[(int)SHADERS::TEMPORAL_FILTER]);
	m_psos[(int)SHADERS::SPATIAL_FILTER] = s_rpObjs.m_psoLib.GetComputePSO((int)SHADERS::SPATIAL_FILTER,
		s_rpObjs.m_rootSig.Get(), COMPILED_CS[(int)SHADERS::SPATIAL_FILTER]);

	//
	// GPU resources
	//

	m_descTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(1);
	CreateSizeDependantResources();
	CreateSizeIndependantResources();

	//
	// params
	//

	// init params
	InitParams();

	m_cbReSTIR.NumRISCandidates = DefaultParamVals::NumRISCandidates;
	m_cbReSTIR.MaxMScale = DefaultParamVals::MaxMScale;
	m_cbReSTIR.NormalAngleThreshold = DefaultParamVals::NormalAngleThresh;
	m_cbReSTIR.DepthToleranceScale = DefaultParamVals::DepthToleranceScale;
	m_cbReSTIR.TemporalSampleBiasWeightThreshold = DefaultParamVals::TemporalSampleBiasThresh;
	m_cbReSTIR.NumSpatialSamples = DefaultParamVals::NumSpatialSamples;
	m_cbReSTIR.NumSpatialSamplesWhenTemporalReuseFailed = DefaultParamVals::NumSpatialSamplesDisocclusion;
	m_cbReSTIR.SpatialNeighborSearchRadius = DefaultParamVals::SpatialNeighborSearchRadius;
	m_cbReSTIR.HaltonSeqLength = k_haltonSeqLength;
	m_cbReSTIR.TileWidth = 16;
	m_cbReSTIR.Log2TileWidth = 4;
}

void ReSTIR::Reset() noexcept
{
	if (IsInitialized())
		s_rpObjs.Clear();

	memset(m_inputDesc, 0, (int)SHADER_IN_DESC::COUNT * sizeof(uint32_t));
	memset(m_psos, 0, (int)SHADERS::COUNT * sizeof(ID3D12PipelineState*));

	m_reservoirs[0].Reset();
	m_reservoirs[1].Reset();

	m_localCB.Reset();
	m_halton.Reset();
	m_descTable.Reset();
}

void ReSTIR::OnGBufferResized() noexcept
{
	Assert(IsInitialized(), "object hasnt't been initialized");
	CreateSizeDependantResources();
}

void ReSTIR::Render(CommandList& cmdList) noexcept
{
	Assert(IsInitialized(), "object hasnt't been initialized");

	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& gpuMem = App::GetRenderer().GetGpuMemory();
	const int w = App::GetRenderer().GetRenderWidth();
	const int h = App::GetRenderer().GetRenderHeight();
	const int outIdx = App::GetTimer().GetTotalFrameCount() & 0x1;
	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

	const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, ReSTIR_THREAD_GROUP_SIZE_X);
	const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, ReSTIR_THREAD_GROUP_SIZE_Y);

	// local CB
	Assert(m_inputDesc[(int)SHADER_IN_DESC::LINEAR_DEPTH_GRADIENT] != 0, "Input descriptor heap idx was not set.");
	m_cbReSTIR.LinearDepthGradDescHeapIdx = m_inputDesc[(int)SHADER_IN_DESC::LINEAR_DEPTH_GRADIENT];
	m_cbReSTIR.DispatchDimX = dispatchDimX;
	m_cbReSTIR.DispatchDimY = dispatchDimY;
	m_cbReSTIR.NumGroupsInTile = m_cbReSTIR.TileWidth * m_cbReSTIR.DispatchDimY;
	m_localCB = gpuMem.GetUploadHeapBuffer(sizeof(cbReSTIR), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
	m_localCB.Copy(0, sizeof(m_localCB), &m_cbReSTIR);
	m_rootSig.SetRootCBV(1, m_localCB.GetGpuVA());

	// halton
	m_rootSig.SetRootSRV(6, m_halton.GetGpuVA());

	m_rootSig.End(computeCmdList);

	// temporal-pass
	{
		computeCmdList.PIXBeginEvent("ReSTIR_TemporalPass");
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::TEMPORAL_FILTER]);

		D3D12_RESOURCE_BARRIER barriers[2];

		barriers[0] = TransitionBarrier(m_reservoirs[1 - outIdx].GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		barriers[1] = TransitionBarrier(m_reservoirs[outIdx].GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		computeCmdList.TransitionResource(barriers, 2);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		computeCmdList.PIXEndEvent();
	}

	// spatial-pass
	{
		computeCmdList.PIXBeginEvent("ReSTIR_SpatialPass");
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::SPATIAL_FILTER]);

		D3D12_RESOURCE_BARRIER barriers[3];

		barriers[0] = UAVBarrier(m_reservoirs[outIdx].GetResource());

		barriers[1] = TransitionBarrier(m_reservoirs[outIdx].GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		barriers[2] = TransitionBarrier(m_reservoirs[1 - outIdx].GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		computeCmdList.TransitionResource(barriers, 3);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		computeCmdList.PIXEndEvent();
	}
}

void ReSTIR::CreateSizeDependantResources() noexcept
{
	auto& renderer = App::GetRenderer();
	auto* device = App::GetRenderer().GetDevice();

	// reservoirs
	const size_t sizeInBytes = sizeof(Reservoir) * renderer.GetRenderWidth() * renderer.GetRenderHeight();

	for (int i = 0; i < 2; i++)
	{
		StackStr(buff, nR, "ReSTIR_Reservoir_%d", i);

		m_reservoirs[i] = renderer.GetGpuMemory().GetDefaultHeapBuffer(buff, sizeInBytes,
			D3D12_RESOURCE_STATE_COMMON, true, true);	
	}

	// output color

	m_outputColor = renderer.GetGpuMemory().GetTexture2D("ReSTIR_OutColor",
		renderer.GetRenderWidth(), renderer.GetRenderHeight(),
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	// UAV
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Texture2D.MipSlice = 0;
	uavDesc.Texture2D.PlaneSlice = 0;
	uavDesc.Format = m_outputColor.GetResource()->GetDesc().Format;

	device->CreateUnorderedAccessView(m_outputColor.GetResource(), nullptr, &uavDesc,
		m_descTable.CPUHandle(0));

	m_cbReSTIR.OutputDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(0);
}

void ReSTIR::CreateSizeIndependantResources() noexcept
{
	auto& renderer = App::GetRenderer();

	// halton seq.
	float2 haltonSeq[k_haltonSeqLength];
	for (int i = 0; i < k_haltonSeqLength; i++)
	{
		haltonSeq[i].x = Halton(i, 2);
		haltonSeq[i].y = Halton(i, 3);
	}

	m_halton = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit("HaltonSeq", sizeof(float2) * k_haltonSeqLength,
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, false, haltonSeq);
}

void ReSTIR::InitParams() noexcept
{
	ParamVariant numRISCandidates;
	numRISCandidates.InitInt("RenderPass", "ReSTIR", "NumRISCandidates", fastdelegate::MakeDelegate(this, &ReSTIR::NumRISCandidatesCallback),
		DefaultParamVals::NumRISCandidates,		// val	
		4,										// min
		64,										// max
		1);										// step
	App::AddParam(numRISCandidates);

	ParamVariant maxMScale;
	maxMScale.InitFloat("RenderPass", "ReSTIR", "MaxMScale", fastdelegate::MakeDelegate(this, &ReSTIR::MaxMScaleCallback),
		DefaultParamVals::MaxMScale,		// val	
		1.0f,								// min
		40.0f,								// max
		1.0f);								// step
	App::AddParam(maxMScale);

	ParamVariant normalAngleThresh;
	normalAngleThresh.InitFloat("RenderPass", "ReSTIR", "NormalAngleThresh", fastdelegate::MakeDelegate(this, &ReSTIR::NormalAngleThreshCallback),
		DefaultParamVals::NormalAngleThresh,		// val	
		0.0f,										// min
		1.0f,										// max
		0.1f);										// step
	App::AddParam(normalAngleThresh);

	ParamVariant depthToleranceScale;
	depthToleranceScale.InitFloat("RenderPass", "ReSTIR", "DepthToleranceScale", fastdelegate::MakeDelegate(this, &ReSTIR::DepthToleranceScaleCallback),
		DefaultParamVals::DepthToleranceScale,		// val	
		1.0f,										// min
		2.0f,										// max
		0.1f);										// step	
	App::AddParam(depthToleranceScale);

	ParamVariant temporalSampleBiasThresh;
	temporalSampleBiasThresh.InitFloat("RenderPass", "ReSTIR", "TemporalSampleBiasThresh",
		fastdelegate::MakeDelegate(this, &ReSTIR::TemporalSampleBiasThreshCallback),
		DefaultParamVals::TemporalSampleBiasThresh,		// val	
		0.0f,											// min
		0.1f,											// max
		0.01f);											// step
	App::AddParam(temporalSampleBiasThresh);

	ParamVariant numSpatialSamples;
	numSpatialSamples.InitInt("RenderPass", "ReSTIR", "NumSpatialSamples", fastdelegate::MakeDelegate(this, &ReSTIR::NumSpatialSamplesCallback),
		DefaultParamVals::NumSpatialSamples,		// val	
		1,											// min
		16,											// max
		1);											// step
	App::AddParam(numSpatialSamples);

	ParamVariant numSpatialSamplesDisocclusion;
	numSpatialSamplesDisocclusion.InitInt("RenderPass", "ReSTIR", "NumSpatialSamplesDisocclusion",
		fastdelegate::MakeDelegate(this, &ReSTIR::NumSpatialSamplesDisocclusionCallback),
		DefaultParamVals::NumSpatialSamplesDisocclusion,		// val	
		1,														// min
		16,														// max
		1);														// step
	App::AddParam(numSpatialSamplesDisocclusion);

	ParamVariant spatialNeighborSearchRadius;
	spatialNeighborSearchRadius.InitFloat("RenderPass", "ReSTIR", "SpatialNeighborSearchRadius",
		fastdelegate::MakeDelegate(this, &ReSTIR::SpatialNeighborSearchRadiusCallback),
		DefaultParamVals::SpatialNeighborSearchRadius,		// val	
		16.0f,												// min
		128.0f,												// max
		4.0f);												// step
	App::AddParam(spatialNeighborSearchRadius);
}

void ReSTIR::NumRISCandidatesCallback(const ParamVariant& p) noexcept
{
	m_cbReSTIR.NumRISCandidates = p.GetInt().m_val;
}

void ReSTIR::MaxMScaleCallback(const ParamVariant& p) noexcept
{
	m_cbReSTIR.MaxMScale = p.GetFloat().m_val;
}

void ReSTIR::NormalAngleThreshCallback(const ParamVariant& p) noexcept
{
	m_cbReSTIR.NormalAngleThreshold = p.GetFloat().m_val;
}

void ReSTIR::DepthToleranceScaleCallback(const ParamVariant& p) noexcept
{
	m_cbReSTIR.DepthToleranceScale = p.GetFloat().m_val;
}

void ReSTIR::TemporalSampleBiasThreshCallback(const ParamVariant& p) noexcept
{
	m_cbReSTIR.TemporalSampleBiasWeightThreshold = p.GetFloat().m_val;
}

void ReSTIR::NumSpatialSamplesCallback(const ParamVariant& p) noexcept
{
	m_cbReSTIR.NumSpatialSamples = p.GetInt().m_val;
}

void ReSTIR::NumSpatialSamplesDisocclusionCallback(const ParamVariant& p) noexcept
{
	m_cbReSTIR.NumSpatialSamplesWhenTemporalReuseFailed = p.GetInt().m_val;
}

void ReSTIR::SpatialNeighborSearchRadiusCallback(const ParamVariant& p) noexcept
{
	m_cbReSTIR.SpatialNeighborSearchRadius = p.GetFloat().m_val;
}
