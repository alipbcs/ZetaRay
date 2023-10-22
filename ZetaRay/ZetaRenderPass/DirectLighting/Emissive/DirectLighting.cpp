#include "DirectLighting.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>
#include <RayTracing/Sampler.h>
#include <Math/Sampling.h>
#include <Scene/SceneCore.h>
#include <Core/RenderGraph.h>
#include <Core/SharedShaderResources.h>

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

namespace
{
	void Normalize(MutableSpan<float> weights)
	{
		// compute the sum of weights
		const int64_t N = weights.size();
		const float sum = Math::KahanSum(weights);
		Assert(!IsNaN(sum), "sum of weights was NaN.");

		// multiply each probability by N so that mean becomes 1 instead of 1 / N
		const float sumRcp = N / sum;

		// align to 32 bytes
		float* curr = weights.data();
		while ((reinterpret_cast<uintptr_t>(curr) & 31) != 0)
		{
			*curr *= sumRcp;
			curr++;
		}

		const int64_t startOffset = curr - weights.data();

		// largest multiple of 8 that is smaller than N
		int64_t numToSumSIMD = N - startOffset;
		numToSumSIMD -= numToSumSIMD & 7;

		const float* end = curr + numToSumSIMD;
		__m256 vSumRcp = _mm256_broadcast_ss(&sumRcp);

		for (; curr < end; curr += 8)
		{
			__m256 V = _mm256_load_ps(curr);
			V = _mm256_mul_ps(V, vSumRcp);

			_mm256_store_ps(curr, V);
		}

		for (int64_t i = startOffset + numToSumSIMD; i < N; i++)
			weights[i] *= sumRcp;
	}

	// Ref: https://www.keithschwarz.com/darts-dice-coins/
	void BuildAliasTable(MutableSpan<float> probs, MutableSpan<EmissiveTriangleSample> table)
	{
		const int64_t N = probs.size();
		const float oneDivN = 1.0f / N;
		Normalize(probs);

		for (int64_t i = 0; i < N; i++)
		{
			table[i].CachedP_Orig = probs[i] * oneDivN;
#ifdef _DEBUG
			table[i].Alias = uint32_t(-1);
#endif
		}

		// maintain an index buffer since original ordering of elements must be preserved
		SmallVector<uint32_t, App::LargeFrameAllocator> larger;
		larger.reserve(N);

		SmallVector<uint32_t, App::LargeFrameAllocator> smaller;
		smaller.reserve(N);

		for (int64_t i = 0; i < N; i++)
		{
			if (probs[i] < 1.0f)
				smaller.push_back((uint32_t)i);
			else
				larger.push_back((uint32_t)i);
		}

#ifdef _DEBUG
		int64_t numInsertions = 0;
#endif // _DEBUG

		// in each iteration, pick two probabilities such that one is smaller than 1.0 and the other larger 
		// than 1.0. Use the latter to bring up the former to 1.0.
		while (!smaller.empty() && !larger.empty())
		{
			const uint32_t smallerIdx = smaller.back();
			smaller.pop_back();
			const float smallerProb = probs[smallerIdx];

			const uint32_t largerIdx = larger.back();
			float largerProb = probs[largerIdx];
			Assert(largerProb >= 1.0f, "should be >= 1.0");

			EmissiveTriangleSample& e = table[smallerIdx];
			Assert(e.Alias == -1, "Every element must be inserted exactly one time.");
			e.Alias = largerIdx;
			e.P_Curr = smallerProb;

			// = largerProb - (1.0f - smallerProb);
			largerProb = (smallerProb + largerProb) - 1.0f;
			probs[largerIdx] = largerProb;

			if (largerProb < 1.0f)
			{
				larger.pop_back();
				smaller.push_back(largerIdx);
			}

#ifdef _DEBUG
			numInsertions++;
#endif
		}

		while (!larger.empty())
		{
			size_t idx = larger.back();
			larger.pop_back();
			Assert(fabsf(1.0f - probs[idx]) <= 0.1f, "This should be ~1.0.");

			// alias should point to itself
			table[idx].Alias = (uint32_t)idx;
			table[idx].P_Curr = 1.0f;

#ifdef _DEBUG
			numInsertions++;
#endif
		}

		while (!smaller.empty())
		{
			size_t idx = smaller.back();
			smaller.pop_back();
			Assert(fabsf(1.0f - probs[idx]) <= 0.1f, "This should be ~1.0.");

			// alias should point to itself
			table[idx].Alias = (uint32_t)idx;
			table[idx].P_Curr = 1.0f;

#ifdef _DEBUG
			numInsertions++;
#endif
		}

		Assert(numInsertions == N, "Some elements were not inserted.");

		for (int64_t i = 0; i < N; i++)
			table[i].CachedP_Alias = table[table[i].Alias].CachedP_Orig;
	}
}

//--------------------------------------------------------------------------------------
// EmissiveTriangleLumen
//--------------------------------------------------------------------------------------

EmissiveTriangleLumen::EmissiveTriangleLumen()
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// root constants
	m_rootSig.InitAsConstants(0,		// root idx
		NUM_CONSTS,						// num DWORDs
		0,								// register
		0);								// register space

	// frame constants
	m_rootSig.InitAsCBV(1,												// root idx
		1,																// register
		0,																// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_ALL,									// visibility
		GlobalResource::FRAME_CONSTANTS_BUFFER);

	// emissive triangles
	m_rootSig.InitAsBufferSRV(2,						// root idx
		0,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		GlobalResource::EMISSIVE_TRIANGLE_BUFFER);

	// halton
	m_rootSig.InitAsBufferSRV(3,						// root idx
		1,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		nullptr,
		true);

	// lumen
	m_rootSig.InitAsBufferUAV(4,						// root idx
		0,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE);
}

EmissiveTriangleLumen::~EmissiveTriangleLumen()
{
	Reset();
}

void EmissiveTriangleLumen::Init()
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
	s_rpObjs.Init("EmissiveTriangleLumen", m_rootSig, samplers.size(), samplers.data(), flags);

	for (int i = 0; i < (int)SHADERS::COUNT; i++)
	{
		m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
			s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[i]);
	}

	float2 samples[ESTIMATE_TRI_LUMEN_NUM_SAMPLES_PER_TRI];

	for (int i = 0; i < ESTIMATE_TRI_LUMEN_NUM_SAMPLES_PER_TRI; i++)
	{
		samples[i].x = Halton(i + 1, 2);
		samples[i].y = Halton(i + 1, 3);
	}

	m_halton = GpuMemory::GetDefaultHeapBufferAndInit("Halton",
		ESTIMATE_TRI_LUMEN_NUM_SAMPLES_PER_TRI * sizeof(float2),
		false,
		samples);
}

void EmissiveTriangleLumen::Reset()
{
	if (IsInitialized())
	{
		s_rpObjs.Clear();

		for (int i = 0; i < ZetaArrayLen(m_psos); i++)
			m_psos[i] = nullptr;

		m_halton.Reset();
		m_lumen.Reset();
		m_readback.Reset();
	}
}

void EmissiveTriangleLumen::Update()
{
	if (!App::GetScene().AreEmissivesStale())
	{
		m_lumen.Reset();
		m_readback.Reset();
		return;
	}

	const size_t currBuffLen = m_lumen.IsInitialized() ? m_lumen.Desc().Width / sizeof(float) : 0;
	m_currNumTris = (uint32_t)App::GetScene().NumEmissiveTriangles();
	Assert(m_currNumTris, "Redundant call.");

	if (currBuffLen < m_currNumTris)
	{
		const uint32_t sizeInBytes = m_currNumTris * sizeof(float);

		m_lumen = GpuMemory::GetDefaultHeapBuffer("TriLumen",
			sizeInBytes,
			D3D12_RESOURCE_STATE_COMMON,
			true);

		m_readback = GpuMemory::GetReadbackHeapBuffer(sizeInBytes);
	}
}

void EmissiveTriangleLumen::Render(CommandList& cmdList)
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	Assert(m_readback.IsInitialized(), "no readback buffer.");
	Assert(!m_readback.IsMapped(), "readback buffer can't be mapped while in use by the GPU.");
	Assert(m_lumen.IsInitialized(), "no lumen buffer.");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();

	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

	const uint32_t dispatchDimX = CeilUnsignedIntDiv(m_currNumTris, ESTIMATE_TRI_LUMEN_NUM_TRIS_PER_GROUP);
	Assert(dispatchDimX <= D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION, "#blocks exceeded maximum allowed.");

	// record the timestamp prior to execution
	const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "EstimateTriLumen");

	computeCmdList.PIXBeginEvent("EstimateTriLumen");
	computeCmdList.SetPipelineState(m_psos[(int)SHADERS::ESTIMATE_TRIANGLE_LUMEN]);

	computeCmdList.ResourceBarrier(m_lumen.Resource(),
		D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	cb_ReSTIR_DI_EstimateTriLumen cb;
	cb.TotalNumTris = m_currNumTris;
	m_rootSig.SetRootConstants(0, sizeof(cb_ReSTIR_DI_EstimateTriLumen) / sizeof(DWORD), &cb);
	
	m_rootSig.SetRootSRV(3, m_halton.GpuVA());
	m_rootSig.SetRootUAV(4, m_lumen.GpuVA());
	
	m_rootSig.End(computeCmdList);

	computeCmdList.Dispatch(dispatchDimX, 1, 1);

	computeCmdList.ResourceBarrier(m_lumen.Resource(), 
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COPY_SOURCE);

	// copy results to readback buffer, so alias table can be computed on the cpu
	computeCmdList.CopyBufferRegion(m_readback.Resource(),
		0,
		m_lumen.Resource(),
		0,
		m_currNumTris * sizeof(float));

	// record the timestamp after execution
	gpuTimer.EndQuery(computeCmdList, queryIdx);

	cmdList.PIXEndEvent();
}

//--------------------------------------------------------------------------------------
// EmissiveTriangleAliasTable
//--------------------------------------------------------------------------------------

void EmissiveTriangleAliasTable::Update(ReadbackHeapBuffer* readback)
{
	Assert(readback, "null resource.");
	m_readback = readback;

	const size_t currBuffLen = m_aliasTable.IsInitialized() ? m_aliasTable.Desc().Width / sizeof(float) : 0;
	m_currNumTris = (uint32_t)App::GetScene().NumEmissiveTriangles();
	Assert(m_currNumTris, "redundant call.");

	if (currBuffLen < m_currNumTris)
	{
		m_aliasTable = GpuMemory::GetDefaultHeapBuffer("AliasTable",
			m_currNumTris * sizeof(EmissiveTriangleSample),
			D3D12_RESOURCE_STATE_COMMON,
			false);

		auto& r = App::GetRenderer().GetSharedShaderResources();
		r.InsertOrAssignDefaultHeapBuffer(GlobalResource::EMISSIVE_TRIANGLE_ALIAS_TABLE, m_aliasTable);
	}
}

void EmissiveTriangleAliasTable::SetEmissiveTriPassHandle(Core::RenderNodeHandle& emissiveTriHandle)
{
	Assert(emissiveTriHandle.IsValid(), "invalid handle.");
	m_emissiveTriHandle = emissiveTriHandle.Val;
}

void EmissiveTriangleAliasTable::Render(CommandList& cmdList)
{
	Assert(m_readback, "Readback buffer hasn't been set.");
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	auto& scene = App::GetScene();

	SmallVector<EmissiveTriangleSample, App::LargeFrameAllocator> table;
	table.resize(m_currNumTris);

	const uint64_t fence = scene.GetRenderGraph()->GetCompletionFence(RenderNodeHandle(m_emissiveTriHandle));
	Assert(fence != uint64_t(-1), "invalid fence value.");

	// wait until GPU finishes copying data to readback buffer
	//renderer.WaitForComputeQueueFenceCPU(fence);
	renderer.WaitForDirectQueueFenceCPU(fence);

	{
		// safe to map, related fence has passed
		m_readback->Map();

		float* data = reinterpret_cast<float*>(m_readback->MappedMemory());
		BuildAliasTable(MutableSpan(data, m_currNumTris), table);

		m_readback->Unmap();
	}

	auto& gpuTimer = renderer.GetGpuTimer();

	// record the timestamp prior to execution
	const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "UploadAliasTable");

	computeCmdList.PIXBeginEvent("UploadAliasTable");

	// schedule a copy
	const uint32_t sizeInBytes = sizeof(EmissiveTriangleSample) * m_currNumTris;
	m_aliasTableUpload = GpuMemory::GetUploadHeapBuffer(sizeInBytes);
	m_aliasTableUpload.Copy(0, sizeInBytes, table.data());
	computeCmdList.CopyBufferRegion(m_aliasTable.Resource(), 
		0,
		m_aliasTableUpload.Resource(),
		m_aliasTableUpload.Offset(),
		sizeInBytes);

	computeCmdList.ResourceBarrier(m_aliasTable.Resource(), 
		D3D12_RESOURCE_STATE_COPY_DEST, 
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

	// record the timestamp after execution
	gpuTimer.EndQuery(computeCmdList, queryIdx);

	cmdList.PIXEndEvent();
}

//--------------------------------------------------------------------------------------
// DirectLighting
//--------------------------------------------------------------------------------------

DirectLighting::DirectLighting()
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// frame constants
	m_rootSig.InitAsCBV(0,
		0,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		D3D12_SHADER_VISIBILITY_ALL,
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
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::RT_SCENE_BVH);

	// emissive triangles
	m_rootSig.InitAsBufferSRV(3,
		1,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::EMISSIVE_TRIANGLE_BUFFER);

	// alias table
	m_rootSig.InitAsBufferSRV(4,
		2,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::EMISSIVE_TRIANGLE_ALIAS_TABLE);

	// sample set SRV
	m_rootSig.InitAsBufferSRV(5,
		3,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		D3D12_SHADER_VISIBILITY_ALL,
		nullptr,
		true);

	// mesh buffer
	m_rootSig.InitAsBufferSRV(6,
		4,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::RT_FRAME_MESH_INSTANCES);

	// sample set UAV
	m_rootSig.InitAsBufferUAV(7,
		0,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
		D3D12_SHADER_VISIBILITY_ALL,
		nullptr,
		true);
}

DirectLighting::~DirectLighting()
{
	Reset();
}

void DirectLighting::Init()
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
	s_rpObjs.Init("DirectLighting", m_rootSig, samplers.size(), samplers.data(), flags);

	for (int i = 0; i < (int)SHADERS::COUNT; i++)
	{
		m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
			s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[i]);
	}

	m_descTable = renderer.GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);
	CreateOutputs();

	memset(&m_cbSpatioTemporal, 0, sizeof(m_cbSpatioTemporal));
	memset(&m_cbDnsrTemporal, 0, sizeof(m_cbDnsrTemporal));
	memset(&m_cbDnsrSpatial, 0, sizeof(m_cbDnsrSpatial));
	m_cbSpatioTemporal.M_max = DefaultParamVals::M_MAX;
	m_cbSpatioTemporal.MaxRoughnessExtraBrdfSampling = 0.3;
	m_cbDnsrTemporal.MaxTsppDiffuse = m_cbDnsrSpatial.MaxTsppDiffuse = DefaultParamVals::DNSR_TSPP_DIFFUSE;
	m_cbDnsrTemporal.MaxTsppSpecular = m_cbDnsrSpatial.MaxTsppSpecular = DefaultParamVals::DNSR_TSPP_SPECULAR;
	m_cbSpatioTemporal.Denoise = m_cbDnsrTemporal.Denoise = m_cbDnsrSpatial.Denoise = true;
	m_cbDnsrSpatial.FilterDiffuse = true;
	m_cbDnsrSpatial.FilterSpecular = true;

	ParamVariant doTemporal;
	doTemporal.InitBool("Renderer", "Direct Lighting", "Temporal Resample",
		fastdelegate::MakeDelegate(this, &DirectLighting::TemporalResamplingCallback), m_doTemporalResampling);
	App::AddParam(doTemporal);

	ParamVariant doSpatial;
	doSpatial.InitBool("Renderer", "Direct Lighting", "Spatial Resample",
		fastdelegate::MakeDelegate(this, &DirectLighting::SpatialResamplingCallback), m_doSpatialResampling);
	App::AddParam(doSpatial);

	ParamVariant maxTemporalM;
	maxTemporalM.InitInt("Renderer", "Direct Lighting", "M_max",
		fastdelegate::MakeDelegate(this, &DirectLighting::MaxTemporalMCallback),
		m_cbSpatioTemporal.M_max,
		1,
		30,
		1);
	App::AddParam(maxTemporalM);

	ParamVariant denoise;
	denoise.InitBool("Renderer", "Direct Lighting", "Denoise",
		fastdelegate::MakeDelegate(this, &DirectLighting::DenoiseCallback), m_cbDnsrTemporal.Denoise);
	App::AddParam(denoise);

	ParamVariant tsppDiffuse;
	tsppDiffuse.InitInt("Renderer", "Direct Lighting", "TSPP (Diffuse)",
		fastdelegate::MakeDelegate(this, &DirectLighting::TsppDiffuseCallback),
		m_cbDnsrTemporal.MaxTsppDiffuse,			// val	
		1,											// min
		32,											// max
		1);											// step
	App::AddParam(tsppDiffuse);

	ParamVariant tsppSpecular;
	tsppSpecular.InitInt("Renderer", "Direct Lighting", "TSPP (Specular)",
		fastdelegate::MakeDelegate(this, &DirectLighting::TsppSpecularCallback),
		m_cbDnsrTemporal.MaxTsppSpecular,			// val	
		1,											// min
		32,											// max
		1);											// step
	App::AddParam(tsppSpecular);

	ParamVariant dnsrSpatialFilterDiffuse;
	dnsrSpatialFilterDiffuse.InitBool("Renderer", "Direct Lighting", "Spatial Filter (Diffuse)",
		fastdelegate::MakeDelegate(this, &DirectLighting::DnsrSpatialFilterDiffuseCallback), m_cbDnsrSpatial.FilterDiffuse);
	App::AddParam(dnsrSpatialFilterDiffuse);

	ParamVariant dnsrSpatialFilterSpecular;
	dnsrSpatialFilterSpecular.InitBool("Renderer", "Direct Lighting", "Spatial Filter (Specular)",
		fastdelegate::MakeDelegate(this, &DirectLighting::DnsrSpatialFilterSpecularCallback), m_cbDnsrSpatial.FilterSpecular);
	App::AddParam(dnsrSpatialFilterSpecular);

	//ParamVariant fireflyFilter;
	//fireflyFilter.InitBool("Renderer", "Direct Lighting", "Firefly Filter",
	//	fastdelegate::MakeDelegate(this, &DirectLighting::FireflyFilterCallback), m_cbDnsrTemporal.FilterFirefly);
	//App::AddParam(fireflyFilter);

	App::AddShaderReloadHandler("ReSTIR_DI_SpatioTemporal", fastdelegate::MakeDelegate(this, &DirectLighting::ReloadSpatioTemporal));
	App::AddShaderReloadHandler("ReSTIR_DI_DNSR_Temporal", fastdelegate::MakeDelegate(this, &DirectLighting::ReloadDnsrTemporal));
	App::AddShaderReloadHandler("ReSTIR_DI_DNSR_Spatial", fastdelegate::MakeDelegate(this, &DirectLighting::ReloadDnsrSpatial));

	m_isTemporalReservoirValid = false;
}

void DirectLighting::Reset()
{
	if (IsInitialized())
	{
		s_rpObjs.Clear();

		for (int i = 0; i < ZetaArrayLen(m_psos); i++)
			m_psos[i] = nullptr;

		m_descTable.Reset();
	}
}

void DirectLighting::OnWindowResized()
{
	CreateOutputs();
	m_isTemporalReservoirValid = false;
	m_currTemporalIdx = 0;
}

void DirectLighting::Update()
{
	m_currNumTris = (uint32_t)App::GetScene().NumEmissiveTriangles();

	if (!m_currNumTris || m_currNumTris < DefaultParamVals::MIN_NUM_LIGHTS_PRESAMPLING)
		return;

	const size_t currBuffLen = m_sampleSets.IsInitialized() ? m_sampleSets.Desc().Width / sizeof(LightSample) : 0;

	if (currBuffLen < m_currNumTris)
	{
		const size_t sizeInBytes = DefaultParamVals::NUM_SAMPLE_SETS * DefaultParamVals::SAMPLE_SET_SIZE * sizeof(LightSample);

		m_sampleSets = GpuMemory::GetDefaultHeapBuffer("EmissiveSampleSets",
			sizeInBytes,
			D3D12_RESOURCE_STATE_COMMON,
			true);
	}
}

void DirectLighting::Render(CommandList& cmdList)
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();
	const uint32_t w = renderer.GetRenderWidth();
	const uint32_t h = renderer.GetRenderHeight();

	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	const bool doPresampling = m_sampleSets.IsInitialized();

	// presampling
	if (doPresampling)
	{
		constexpr uint32_t numSamples = DefaultParamVals::NUM_SAMPLE_SETS * DefaultParamVals::SAMPLE_SET_SIZE;
		const uint32_t dispatchDimX = CeilUnsignedIntDiv(numSamples, RESTIR_DI_PRESAMPLE_GROUP_DIM_X);
		Assert(dispatchDimX <= D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION, "#blocks exceeded maximum allowed.");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_DI_Presample");

		computeCmdList.PIXBeginEvent("ReSTIR_DI_Presample");
		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::PRESAMPLING]);

		// "buffers MAY be initially accessed in an ExecuteCommandLists scope without a Barrier...Additionally, a buffer 
		// or texture using a queue-specific common layout can use D3D12_BARRIER_ACCESS_UNORDERED_ACCESS without a barrier."
		// Ref: https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html
#if 0
		D3D12_BUFFER_BARRIER barrier = Direct3DUtil::BufferBarrier(m_sampleSets.Resource(),
			D3D12_BARRIER_SYNC_NONE,
			D3D12_BARRIER_SYNC_COMPUTE_SHADING,
			D3D12_BARRIER_ACCESS_NO_ACCESS,
			D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);

		computeCmdList.ResourceBarrier(barrier);
#endif

		cb_ReSTIR_DI_Presampling cb;
		cb.NumTotalSamples = DefaultParamVals::NUM_SAMPLE_SETS * DefaultParamVals::SAMPLE_SET_SIZE;
		cb.NumEmissiveTriangles = m_currNumTris;

		m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
		m_rootSig.SetRootUAV(7, m_sampleSets.GpuVA());

		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, 1, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();
	}

	// resampling
	{
		const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_DI_TEMPORAL_GROUP_DIM_X);
		const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_DI_TEMPORAL_GROUP_DIM_Y);

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_DI_SpatioTemporal");

		computeCmdList.PIXBeginEvent("ReSTIR_DI_SpatioTemporal");

		computeCmdList.SetPipelineState(doPresampling ? 
			m_psos[(int)SHADERS::SPATIO_TEMPORAL_LIGHT_PRESAMPLING] : 
			m_psos[(int)SHADERS::SPATIO_TEMPORAL]);

		D3D12_BUFFER_BARRIER bufferBarrier = Direct3DUtil::BufferBarrier(doPresampling ? m_sampleSets.Resource() : nullptr,
			D3D12_BARRIER_SYNC_COMPUTE_SHADING,
			D3D12_BARRIER_SYNC_COMPUTE_SHADING,
			D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
			D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

		SmallVector<D3D12_TEXTURE_BARRIER, SystemAllocator, 6> textureBarriers;

		// transition current temporal reservoir into write state
		textureBarriers.emplace_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[m_currTemporalIdx].ReservoirA.Resource(),
			D3D12_BARRIER_SYNC_NONE,
			D3D12_BARRIER_SYNC_COMPUTE_SHADING,
			D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
			D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
			D3D12_BARRIER_ACCESS_NO_ACCESS,
			D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
		textureBarriers.emplace_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[m_currTemporalIdx].ReservoirB.Resource(),
			D3D12_BARRIER_SYNC_NONE,
			D3D12_BARRIER_SYNC_COMPUTE_SHADING,
			D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
			D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS, 
			D3D12_BARRIER_ACCESS_NO_ACCESS,
			D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));

		// transition color outputs into write state
		if (m_cbSpatioTemporal.Denoise)
		{
			textureBarriers.emplace_back(Direct3DUtil::TextureBarrier(m_colorA.Resource(),
				D3D12_BARRIER_SYNC_NONE,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_NO_ACCESS,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
			textureBarriers.emplace_back(Direct3DUtil::TextureBarrier(m_colorB.Resource(),
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
			textureBarriers.emplace_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[1 - m_currTemporalIdx].ReservoirA.Resource(),
				D3D12_BARRIER_SYNC_NONE,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_ACCESS_NO_ACCESS,
				D3D12_BARRIER_ACCESS_SHADER_RESOURCE));
			textureBarriers.emplace_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[1 - m_currTemporalIdx].ReservoirB.Resource(),
				D3D12_BARRIER_SYNC_NONE,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_ACCESS_NO_ACCESS,
				D3D12_BARRIER_ACCESS_SHADER_RESOURCE));
		}

		// skip transitioning presample buffer if not used
		D3D12_BARRIER_GROUP barrierGroups[2] = { Direct3DUtil::BarrierGroup(textureBarriers.data(), (UINT)textureBarriers.size()),
			Direct3DUtil::BarrierGroup(bufferBarrier) };
		computeCmdList.ResourceBarrier(barrierGroups, doPresampling ? 2 : 1);

		m_cbSpatioTemporal.DispatchDimX = (uint16_t)dispatchDimX;
		m_cbSpatioTemporal.DispatchDimY = (uint16_t)dispatchDimY;
		m_cbSpatioTemporal.NumGroupsInTile = RESTIR_DI_TEMPORAL_TILE_WIDTH * m_cbSpatioTemporal.DispatchDimY;
		m_cbSpatioTemporal.TemporalResampling = m_doTemporalResampling && m_isTemporalReservoirValid;
		m_cbSpatioTemporal.SpatialResampling = m_doSpatialResampling && m_isTemporalReservoirValid;
		m_cbSpatioTemporal.NumEmissiveTriangles = m_currNumTris;
		m_cbSpatioTemporal.OneDivNumEmissiveTriangles = 1.0f / float(m_cbSpatioTemporal.NumEmissiveTriangles);
		m_cbSpatioTemporal.NumSampleSets = DefaultParamVals::NUM_SAMPLE_SETS;
		m_cbSpatioTemporal.SampleSetSize = DefaultParamVals::SAMPLE_SET_SIZE;
		
		{
			auto srvAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_0_A_SRV : DESC_TABLE::RESERVOIR_1_A_SRV;
			auto srvBIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_0_B_SRV : DESC_TABLE::RESERVOIR_1_B_SRV;
			auto uavAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_1_A_UAV : DESC_TABLE::RESERVOIR_0_A_UAV;
			auto uavBIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_1_B_UAV : DESC_TABLE::RESERVOIR_0_B_UAV;

			m_cbSpatioTemporal.PrevReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
			m_cbSpatioTemporal.PrevReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
			m_cbSpatioTemporal.CurrReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
			m_cbSpatioTemporal.CurrReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);
		}

		m_cbSpatioTemporal.ColorAUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_A_UAV);
		m_cbSpatioTemporal.ColorBUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_B_UAV);
		m_cbSpatioTemporal.FinalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::DNSR_FINAL_UAV);

		m_rootSig.SetRootConstants(0, sizeof(m_cbSpatioTemporal) / sizeof(DWORD), &m_cbSpatioTemporal);
		if (doPresampling)
			m_rootSig.SetRootSRV(5, m_sampleSets.GpuVA());

		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		cmdList.PIXEndEvent();
	}

	if (m_cbSpatioTemporal.Denoise)
	{
		// denoiser - temporal
		{
			computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_TEMPORAL]);

			// record the timestamp prior to execution
			const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_DI_DNSR_Temporal");

			computeCmdList.PIXBeginEvent("ReSTIR_DI_DNSR_Temporal");

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

			m_rootSig.SetRootConstants(0, sizeof(m_cbDnsrTemporal) / sizeof(DWORD), &m_cbDnsrTemporal);
			m_rootSig.End(computeCmdList);

			const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_DI_DNSR_TEMPORAL_GROUP_DIM_X);
			const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_DI_DNSR_TEMPORAL_GROUP_DIM_Y);
			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

			// record the timestamp after execution
			gpuTimer.EndQuery(computeCmdList, queryIdx);

			cmdList.PIXEndEvent();
		}

		// denoiser - spatial
		{
			computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_SPATIAL]);

			const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_DI_DNSR_SPATIAL_GROUP_DIM_X);
			const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_DI_DNSR_SPATIAL_GROUP_DIM_Y);

			// record the timestamp prior to execution
			const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_DI_DNSR_Spatial");

			computeCmdList.PIXBeginEvent("ReSTIR_DI_DNSR_Spatial");

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
			m_cbDnsrSpatial.NumGroupsInTile = RESTIR_DI_DNSR_SPATIAL_TILE_WIDTH * m_cbDnsrSpatial.DispatchDimY;

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
}

void DirectLighting::CreateOutputs()
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
		func(m_temporalReservoir[0].ReservoirA, ResourceFormats::RESERVOIR_A, "RDI_Reservoir_0_A",
			DESC_TABLE::RESERVOIR_0_A_SRV, DESC_TABLE::RESERVOIR_0_A_UAV);
		func(m_temporalReservoir[0].ReservoirB, ResourceFormats::RESERVOIR_B, "RDI_Reservoir_0_B",
			DESC_TABLE::RESERVOIR_0_B_SRV, DESC_TABLE::RESERVOIR_0_B_UAV);
		func(m_temporalReservoir[1].ReservoirA, ResourceFormats::RESERVOIR_A, "RDI_Reservoir_1_A",
			DESC_TABLE::RESERVOIR_1_A_SRV, DESC_TABLE::RESERVOIR_1_A_UAV);
		func(m_temporalReservoir[1].ReservoirB, ResourceFormats::RESERVOIR_B, "RDI_Reservoir_1_B",
			DESC_TABLE::RESERVOIR_1_B_SRV, DESC_TABLE::RESERVOIR_1_B_UAV);
	}

	{
		func(m_colorA, ResourceFormats::COLOR_A, "RDI_COLOR_A", DESC_TABLE::COLOR_A_SRV, DESC_TABLE::COLOR_A_UAV);
		func(m_colorB, ResourceFormats::COLOR_B, "RDI_COLOR_B", DESC_TABLE::COLOR_B_SRV, DESC_TABLE::COLOR_B_UAV);
	}

	// denoiser
	{
		func(m_dnsrCache[0].Diffuse, ResourceFormats::DNSR_TEMPORAL_CACHE, "ReSTIR_DI_DNSR_Diffuse_0",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV);
		func(m_dnsrCache[1].Diffuse, ResourceFormats::DNSR_TEMPORAL_CACHE, "ReSTIR_DI_DNSR_Diffuse_1",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV);
		func(m_dnsrCache[0].Specular, ResourceFormats::DNSR_TEMPORAL_CACHE, "ReSTIR_DI_DNSR_Specular_0",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV);
		func(m_dnsrCache[1].Specular, ResourceFormats::DNSR_TEMPORAL_CACHE, "ReSTIR_DI_DNSR_Specular_1",
			DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV);

		m_denoised = GpuMemory::GetTexture2D("ReSTIR_DI_Denoised",
			renderer.GetRenderWidth(), renderer.GetRenderHeight(),
			ResourceFormats::DNSR_TEMPORAL_CACHE,
			D3D12_RESOURCE_STATE_COMMON,
			CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

		Direct3DUtil::CreateTexture2DUAV(m_denoised, m_descTable.CPUHandle((int)DESC_TABLE::DNSR_FINAL_UAV));
	}
}

void DirectLighting::TemporalResamplingCallback(const Support::ParamVariant& p)
{
	m_doTemporalResampling = p.GetBool();
}

void DirectLighting::SpatialResamplingCallback(const Support::ParamVariant& p)
{
	m_doSpatialResampling = p.GetBool();
}

void DirectLighting::MaxTemporalMCallback(const Support::ParamVariant& p)
{
	m_cbSpatioTemporal.M_max = (uint16_t) p.GetInt().m_val;
}

void DirectLighting::MaxRoughessExtraBrdfSamplingCallback(const Support::ParamVariant& p)
{
	m_cbSpatioTemporal.MaxRoughnessExtraBrdfSampling = Math::Max(p.GetFloat().m_val, m_cbSpatioTemporal.MaxRoughnessExtraBrdfSampling);
}

void DirectLighting::DenoiseCallback(const Support::ParamVariant& p)
{
	m_isDnsrTemporalCacheValid = !m_cbDnsrTemporal.Denoise ? false : m_isDnsrTemporalCacheValid;
	m_cbSpatioTemporal.Denoise = p.GetBool();
	m_cbDnsrTemporal.Denoise = p.GetBool();
	m_cbDnsrSpatial.Denoise = p.GetBool();
	m_isDnsrTemporalCacheValid = !m_cbDnsrTemporal.Denoise ? false : m_isDnsrTemporalCacheValid;
}

void DirectLighting::TsppDiffuseCallback(const Support::ParamVariant& p)
{
	m_cbDnsrTemporal.MaxTsppDiffuse = (uint16_t)p.GetInt().m_val;
	m_cbDnsrSpatial.MaxTsppDiffuse = (uint16_t)p.GetInt().m_val;
}

void DirectLighting::TsppSpecularCallback(const Support::ParamVariant& p)
{
	m_cbDnsrTemporal.MaxTsppSpecular = (uint16_t)p.GetInt().m_val;
	m_cbDnsrSpatial.MaxTsppSpecular = (uint16_t)p.GetInt().m_val;
}

void DirectLighting::DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p)
{
	m_cbDnsrSpatial.FilterDiffuse = p.GetBool();
}

void DirectLighting::DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p)
{
	m_cbDnsrSpatial.FilterSpecular = p.GetBool();
}

void DirectLighting::ReloadSpatioTemporal()
{
	const bool presampling = m_sampleSets.IsInitialized();
	const int i = presampling ? (int)SHADERS::SPATIO_TEMPORAL_LIGHT_PRESAMPLING :
		(int)SHADERS::SPATIO_TEMPORAL;

	s_rpObjs.m_psoLib.Reload(i, presampling ?
		"DirectLighting\\Emissive\\ReSTIR_DI_SpatioTemporal_LP.hlsl" :
		"DirectLighting\\Emissive\\ReSTIR_DI_SpatioTemporal.hlsl",
		true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void DirectLighting::ReloadDnsrTemporal()
{
	const int i = (int)SHADERS::DNSR_TEMPORAL;

	s_rpObjs.m_psoLib.Reload(i, "DirectLighting\\Emissive\\ReSTIR_DI_DNSR_Temporal.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

void DirectLighting::ReloadDnsrSpatial()
{
	const int i = (int)SHADERS::DNSR_SPATIAL;

	s_rpObjs.m_psoLib.Reload(i, "DirectLighting\\Emissive\\ReSTIR_DI_DNSR_Spatial.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}

