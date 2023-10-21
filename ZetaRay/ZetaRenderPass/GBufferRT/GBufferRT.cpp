#include "GBufferRT.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>
#include <App/Filesystem.h>
#include <Utility/SmallVector.h>
#include <Scene/SceneCore.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::RT;
using namespace ZetaRay::Support;
using namespace ZetaRay::App;
using namespace ZetaRay::Util;

#define TRACE_INLINE 1

//--------------------------------------------------------------------------------------
// GBufferRT
//--------------------------------------------------------------------------------------

GBufferRT::GBufferRT()
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

	// material buffer
	m_rootSig.InitAsBufferSRV(3,
		1,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::MATERIAL_BUFFER);

	// mesh buffer
	m_rootSig.InitAsBufferSRV(4,
		2,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::RT_FRAME_MESH_INSTANCES);
	
	// scene VB
	m_rootSig.InitAsBufferSRV(5,
		3,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::SCENE_VERTEX_BUFFER);

	// scene IB
	m_rootSig.InitAsBufferSRV(6,
		4,
		0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::SCENE_INDEX_BUFFER);
}

GBufferRT::~GBufferRT()
{
	Reset();
}

void GBufferRT::Init()
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

#if TRACE_INLINE == 0
	CreateRTPSO();
	BuildShaderTable();
#else
	for (int i = 0; i < (int)SHADERS::COUNT; i++)
	{
		m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
			s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[i]);
	}
#endif

	memset(&m_localCB, 0, sizeof(m_localCB));

	//ParamVariant p1;
	//p1.InitEnum("Renderer", "G-Buffer", "Mipmap Selection", fastdelegate::MakeDelegate(this, &GBufferRT::MipmapSelectionCallback),
	//	DefaultParamVals::TextureFilter, (int)TextureFilter::COUNT, m_localCB.MipmapSelection);
	//App::AddParam(p1);

	//ParamVariant traceInline;
	//traceInline.InitBool("Renderer", "G-Buffer", "Trace Inline",
	//	fastdelegate::MakeDelegate(this, &GBufferRT::InlineTracingCallback), m_inline);
	//App::AddParam(traceInline);

#if TRACE_INLINE == 1
	App::AddShaderReloadHandler("GBufferRT", fastdelegate::MakeDelegate(this, &GBufferRT::ReloadGBufferInline));
#endif
}

void GBufferRT::Reset()
{
	if (IsInitialized())
	{
		s_rpObjs.Clear();
		m_shaderTable.ShaderRecords.Reset();
	}
}

void GBufferRT::Render(CommandList& cmdList)
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();
	const uint32_t w = renderer.GetRenderWidth();
	const uint32_t h = renderer.GetRenderHeight();

	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

	// record the timestamp prior to execution
	const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "GBufferRT");

	computeCmdList.PIXBeginEvent("GBufferRT");

	const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, GBUFFER_RT_GROUP_DIM_X);
	const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, GBUFFER_RT_GROUP_DIM_Y);

	m_localCB.DispatchDimX = (uint16_t)dispatchDimX;
	m_localCB.DispatchDimY = (uint16_t)dispatchDimY;
	m_localCB.NumGroupsInTile = GBUFFER_RT_TILE_WIDTH * m_localCB.DispatchDimY;

	m_rootSig.SetRootConstants(0, sizeof(m_localCB) / sizeof(DWORD), &m_localCB);
	m_rootSig.End(computeCmdList);

#if TRACE_INLINE == 0
	computeCmdList.SetPipelineState1(m_rtPSO.Get());

	const uint64_t shaderTableStartVA = m_shaderTable.ShaderRecords.GpuVA();

	computeCmdList.DispatchRays(shaderTableStartVA,
		D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES * ShaderTable::NUM_RAYGEN_SHADERS,
		shaderTableStartVA + m_shaderTable.MissRecordStartInBytes,
		D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES * ShaderTable::NUM_MISS_SHADERS,
		D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
		shaderTableStartVA + m_shaderTable.HitRecordStartInBytes,
		D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES * ShaderTable::NUM_HIT_GROUPS,
		D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
		w,
		h);
#else
	computeCmdList.SetPipelineState(m_psos[(int)SHADERS::GBUFFER_RT_INLINE]);
	computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#endif

	// record the timestamp after execution
	gpuTimer.EndQuery(computeCmdList, queryIdx);

	cmdList.PIXEndEvent();
}

void GBufferRT::CreateRTPSO()
{
	Filesystem::Path csoPath(App::GetCompileShadersDir());
	csoPath.Append(COMPILED_RTPSO);

	SmallVector<uint8_t> bytecode;
	Filesystem::LoadFromFile(csoPath.Get(), bytecode);

	D3D12_STATE_SUBOBJECT subobjects[2];

	// 1. DXIL lib
	D3D12_DXIL_LIBRARY_DESC libDesc;
	libDesc.DXILLibrary.BytecodeLength = bytecode.size();
	libDesc.DXILLibrary.pShaderBytecode = bytecode.data();
	libDesc.NumExports = 0;
	libDesc.pExports = nullptr;

	subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
	subobjects[0].pDesc = &libDesc;

	// 2. Global root signature
	D3D12_GLOBAL_ROOT_SIGNATURE rootSig;
	rootSig.pGlobalRootSignature = s_rpObjs.m_rootSig.Get();

	subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
	subobjects[1].pDesc = &rootSig;

	D3D12_STATE_OBJECT_DESC desc;
	desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
	desc.NumSubobjects = ZetaArrayLen(subobjects);
	desc.pSubobjects = subobjects;

	auto* device = App::GetRenderer().GetDevice();
	CheckHR(device->CreateStateObject(&desc, IID_PPV_ARGS(m_rtPSO.GetAddressOf())));

	ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
	CheckHR(m_rtPSO.As(&stateObjectProperties));
	m_shaderTable.HitGroupIdentifier = stateObjectProperties->GetShaderIdentifier(L"MyHitGroup");
	m_shaderTable.MissShaderIdentifier = stateObjectProperties->GetShaderIdentifier(L"Miss");
	m_shaderTable.RayGenShaderIdentifier = stateObjectProperties->GetShaderIdentifier(L"Raygen");
}

void GBufferRT::BuildShaderTable()
{
	// raygen
	m_shaderTable.RayGenRecordStartInBytes = 0;
	uint32_t sizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
	sizeInBytes = Math::AlignUp((int)sizeInBytes, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

	// miss
	m_shaderTable.MissRecordStartInBytes = sizeInBytes;
	sizeInBytes += D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
	sizeInBytes = Math::AlignUp((int)sizeInBytes, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

	// hit group
	m_shaderTable.HitRecordStartInBytes = sizeInBytes;
	sizeInBytes += D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

	SmallVector<unsigned char, App::FrameAllocator> m_sbt;
	m_sbt.resize(sizeInBytes);
	const uintptr_t start = reinterpret_cast<uintptr_t>(m_sbt.data());

	// copy one raygen record
	memcpy(reinterpret_cast<void*>(start), m_shaderTable.RayGenShaderIdentifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

	// copy one miss shader record
	memcpy(reinterpret_cast<void*>(start + m_shaderTable.MissRecordStartInBytes),
		m_shaderTable.MissShaderIdentifier,
		D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

	// copy hit group identifier
	memcpy(reinterpret_cast<void*>(start + m_shaderTable.HitRecordStartInBytes), 
		m_shaderTable.HitGroupIdentifier,
		D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

	// alignment must be D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT -- since all buffers are 64 kb aligned,
	// it'll have the correct alignment
	m_shaderTable.ShaderRecords = GpuMemory::GetDefaultHeapBufferAndInit("SBT", sizeInBytes, false, m_sbt.data());
}

void GBufferRT::ReloadGBufferInline()
{
	const int i = (int)SHADERS::GBUFFER_RT_INLINE;

	s_rpObjs.m_psoLib.Reload(i, "GBufferRT\\GBufferRT_Inline.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}
