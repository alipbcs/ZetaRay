#include "GBufferPass.h"
#include <Core/RendererCore.h>
#include <Core/Vertex.h>
#include <Core/CommandList.h>
#include <Scene/SceneCore.h>
#include <Math/MatrixFuncs.h>
#include <Model/Mesh.h>
#include <App/Timer.h>
#include <Scene/Camera.h>
#include <Support/Param.h>
#include <algorithm>

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Util;
using namespace ZetaRay::Model;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// GBufferPass
//--------------------------------------------------------------------------------------

GBufferPass::GBufferPass() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// frame constants
	m_rootSig.InitAsCBV(0,							// root idx
		0,											// register num
		0,											// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::FRAME_CONSTANTS_BUFFER_NAME);

	// root constants
	m_rootSig.InitAsConstants(1,		// root idx
		NUM_CONSTS,						// num DWORDs
		1,								// register num
		0);								// register space

	// mesh buffer
	m_rootSig.InitAsBufferSRV(2,						// root idx
		0,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);		// flags

	// scene VB
	m_rootSig.InitAsBufferSRV(3,						// root idx
		1,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		GlobalResource::SCENE_VERTEX_BUFFER);

	// scene IB
	m_rootSig.InitAsBufferSRV(4,						// root idx
		2,												// register
		0,												// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,			// flags
		D3D12_SHADER_VISIBILITY_ALL,					// visibility
		GlobalResource::SCENE_INDEX_BUFFER);

	// material buffer
	m_rootSig.InitAsBufferSRV(5,					// root idx
		3,											// register num
		0,											// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
		D3D12_SHADER_VISIBILITY_ALL,
		GlobalResource::MATERIAL_BUFFER);	
	
	// visibility buffer (SRV)
	m_rootSig.InitAsBufferSRV(6,					// root idx
		4,											// register num
		0,											// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
		D3D12_SHADER_VISIBILITY_ALL,
		nullptr,
		true);

	// indirect args
	m_rootSig.InitAsBufferUAV(7,					// root idx
		0,											// register num
		0,											// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
		D3D12_SHADER_VISIBILITY_ALL,
		nullptr,
		true);

	// visibility buffer (UAV)
	m_rootSig.InitAsBufferUAV(8,					// root idx
		1,											// register num
		0,											// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
		D3D12_SHADER_VISIBILITY_ALL,
		nullptr,
		true);

	// SPD counter
	m_rootSig.InitAsBufferUAV(9,				// root idx
		2,										// register num
		0,										// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
		D3D12_SHADER_VISIBILITY_ALL,
		nullptr,
		true);
}

GBufferPass::~GBufferPass() noexcept
{
	Reset();
}

void GBufferPass::Init(Span<DXGI_FORMAT> rtvs) noexcept
{
	const D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	auto& renderer = App::GetRenderer();
	auto samplers = renderer.GetStaticSamplers();
	s_rpObjs.Init("GBufferPass", m_rootSig, samplers.size(), samplers.data(), flags);
	CreatePSOs(rtvs);

	// command signature
	D3D12_INDIRECT_ARGUMENT_DESC indirectCallArgs[2];
	indirectCallArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
	indirectCallArgs[0].Constant.RootParameterIndex = 1;
	indirectCallArgs[0].Constant.Num32BitValuesToSet = sizeof(cbGBuffer) / sizeof(DWORD);
	indirectCallArgs[0].Constant.DestOffsetIn32BitValues = 0;

	indirectCallArgs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

	D3D12_COMMAND_SIGNATURE_DESC desc;
	desc.ByteStride = sizeof(CommandSig);
	desc.NumArgumentDescs = ZetaArrayLen(indirectCallArgs);
	desc.pArgumentDescs = indirectCallArgs;
	desc.NodeMask = 0;

	CheckHR(renderer.GetDevice()->CreateCommandSignature(&desc, s_rpObjs.m_rootSig.Get(), 
		IID_PPV_ARGS(m_cmdSig.GetAddressOf())));

	m_descTable = renderer.GetCbvSrvUavDescriptorHeapGpu().Allocate((uint32_t)DESC_TABLE::COUNT);

	CreateDepthPyramid();

	// create a zero-initialized buffer for resetting the counter
	m_zeroBuffer = renderer.GetGpuMemory().GetDefaultHeapBuffer("Zero",
		sizeof(uint32_t),
		D3D12_RESOURCE_STATE_COMMON,
		false,
		true);

	m_spdCounter = renderer.GetGpuMemory().GetDefaultHeapBuffer("SpdCounter",
		sizeof(uint32_t),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		true,
		true);	// init the counter to zero for first frame

	// for retrieving stats from the GPU
	m_readbackBuff = renderer.GetGpuMemory().GetReadbackHeapBuffer(
		sizeof(uint32_t) * 4 * Constants::NUM_BACK_BUFFERS);		// four counters each frame
	CheckHR(renderer.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));

	m_occlusionTestDepthThresh = DefaultParamVals::DepthThresh;

	ParamVariant depthThresh;
	depthThresh.InitFloat("Renderer", "OcclusionCulling", "DepthThresh", fastdelegate::MakeDelegate(this, &GBufferPass::DepthThreshCallback),
		m_occlusionTestDepthThresh,				// val	
		1e-3f,									// min
		1e-1f,									// max
		1e-2f);									// step
	App::AddParam(depthThresh);

	App::AddShaderReloadHandler("OcclusionCulling", fastdelegate::MakeDelegate(this, &GBufferPass::ReloadShader));
}

void GBufferPass::Reset() noexcept
{
	if (m_graphicsPso[0])
	{
		s_rpObjs.Clear();
		//memset(m_graphicsPso, 0, sizeof(ID3D12PipelineState*) * ZetaArrayLen(m_graphicsPso));
	}

	m_meshInstances.Reset();
	m_indirectDrawArgs.Reset();
	m_zeroBuffer.Reset();
	//m_maxNumDrawCallsSoFar = 0;
	m_depthPyramid.Reset();
	m_readbackBuff.Reset();
	m_visibilityBuffer.Reset();
}

void GBufferPass::OnWindowResized() noexcept
{
	CreateDepthPyramid();
}

void GBufferPass::Update(Span<MeshInstance> instances, ID3D12Resource* currDepthBuffer) noexcept
{
	m_currDepthBuffer = currDepthBuffer;
	m_numMeshesThisFrame = (uint32_t)instances.size();

	if (m_numMeshesThisFrame == 0)
		return;

	if (App::GetTimer().GetTotalFrameCount() > 1)
	{
		// at this point, we know previous frame's commands have been submitted
		m_fenceVals[m_currFrameIdx] = m_nextFenceVal;
		App::GetRenderer().SignalDirectQueue(m_fence.Get(), m_nextFenceVal++);
		// advance the frame index
		m_currFrameIdx = m_currFrameIdx < Constants::NUM_BACK_BUFFERS - 1 ? m_currFrameIdx + 1 : 0;
	}

	// make sure instances are seperated by their PSO
	auto split = std::partition(instances.begin(), instances.end(),
		[](const MeshInstance& mesh)
		{
			return !mesh.IsDoubleSided;
		});

	m_numSingleSidedMeshes = (uint32_t)(split - instances.begin());

	// sort each PSO group by their visibility index
	std::sort(instances.begin(), split,
		[](const MeshInstance& lhs, const MeshInstance& rhs)
		{
			return lhs.VisibilityIdx <= rhs.VisibilityIdx;
		});

	std::sort(split, instances.end(),
		[](const MeshInstance& lhs, const MeshInstance& rhs)
		{
			return lhs.VisibilityIdx <= rhs.VisibilityIdx;
		});

	auto& renderer = App::GetRenderer();
	auto& gpuMem = renderer.GetGpuMemory();

	const size_t meshInsBuffSizeInBytes = sizeof(MeshInstance) * m_numMeshesThisFrame;

	// reuse the current buffer if possible
	if (!m_meshInstances.IsInitialized() || m_meshInstances.GetDesc().Width < meshInsBuffSizeInBytes)
	{
		m_meshInstances = gpuMem.GetDefaultHeapBufferAndInit("GBufferMeshInstances",
			meshInsBuffSizeInBytes,
			D3D12_RESOURCE_STATE_COMMON,
			false,
			instances.data());
	}
	else
		// this is recorded now but submitted after the last frame's submissions
		gpuMem.UploadToDefaultHeapBuffer(m_meshInstances, meshInsBuffSizeInBytes, instances.data());

	// reuse the existing indirect args buffer if possible
	if (m_maxNumDrawCallsSoFar < m_numMeshesThisFrame)
	{
		m_maxNumDrawCallsSoFar = m_numMeshesThisFrame;

		// extra 16 bytes for the counters
		m_counterSingleSidedBufferOffsetFirst = sizeof(CommandSig) * m_maxNumDrawCallsSoFar;
		m_counterDoubleSidedBufferOffsetFirst = m_counterSingleSidedBufferOffsetFirst + sizeof(uint32_t);
		m_counterSingleSidedBufferOffsetSecond = m_counterDoubleSidedBufferOffsetFirst + sizeof(uint32_t);
		m_counterDoubleSidedBufferOffsetSecond = m_counterSingleSidedBufferOffsetSecond + sizeof(uint32_t);

		const size_t indDrawArgsBuffSizeInBytes = m_counterDoubleSidedBufferOffsetSecond + sizeof(uint32_t);

		m_indirectDrawArgs = gpuMem.GetDefaultHeapBuffer("IndirectDrawArgs",
			indDrawArgsBuffSizeInBytes,
			D3D12_RESOURCE_STATE_COMMON,
			true);
	}

	// TODO detect when scene instances have changed, so visibility is reset to zero
	if (!m_visibilityBuffer.IsInitialized())
	{
		const uint32_t numTotalInstances = App::GetScene().GetTotalNumInstances();
		Assert(numTotalInstances, "this must be greater than zero.");
		const size_t sizeInBytes = Math::CeilUnsignedIntDiv(numTotalInstances, 32) * sizeof(uint32_t);

		m_visibilityBuffer = gpuMem.GetDefaultHeapBuffer("VisibilityBuffer",
			sizeInBytes,
			D3D12_RESOURCE_STATE_COMMON,
			true,
			true);		// make sure it is initialized to zero
	}
}

void GBufferPass::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");

	if (m_numMeshesThisFrame == 0)
		return;

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();

	// build indirect draw args buffer -- no culling
	{
		ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

		computeCmdList.PIXBeginEvent("IndDrawArgs_NoCull");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "IndDrawArgs_NoCull");

		computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
		computeCmdList.SetPipelineState(m_computePsos[(int)COMPUTE_SHADERS::BUILD_IND_DRAW_ARGS_NO_CULL]);

		cbOcclussionCulling localCB;
		m_rootSig.SetRootSRV(2, m_meshInstances.GetGpuVA());
		m_rootSig.SetRootSRV(6, m_visibilityBuffer.GetGpuVA());
		m_rootSig.SetRootUAV(7, m_indirectDrawArgs.GetGpuVA());

		D3D12_RESOURCE_BARRIER barriers[2];
		barriers[0] = Direct3DHelper::TransitionBarrier(m_indirectDrawArgs.GetResource(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_COPY_DEST);

		barriers[1] = Direct3DHelper::TransitionBarrier(m_visibilityBuffer.GetResource(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

		// clear the counters
		computeCmdList.CopyBufferRegion(m_indirectDrawArgs.GetResource(),
			m_counterSingleSidedBufferOffsetFirst,
			m_zeroBuffer.GetResource(),
			0,
			sizeof(uint32_t) * 4);

		computeCmdList.ResourceBarrier(m_indirectDrawArgs.GetResource(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		// single-sided meshes
		if (m_numSingleSidedMeshes)
		{
			localCB.NumMeshes = m_numSingleSidedMeshes;
			localCB.CounterBufferOffset = m_counterSingleSidedBufferOffsetFirst;
			localCB.MeshBufferStartIndex = 0;
			localCB.ArgBufferStartOffsetInBytes = 0;

			m_rootSig.SetRootConstants(0, sizeof(localCB) / sizeof(DWORD), &localCB);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch((uint32_t)CeilUnsignedIntDiv(m_numSingleSidedMeshes, BUILD_NO_CULL_THREAD_GROUP_SIZE_X),
				1, 1);
		}

		// double-sided meshes
		if (m_numSingleSidedMeshes < m_numMeshesThisFrame)
		{
			const uint32_t numDoubleSided = m_numMeshesThisFrame - m_numSingleSidedMeshes;
			localCB.NumMeshes = numDoubleSided;
			localCB.CounterBufferOffset = m_counterDoubleSidedBufferOffsetFirst;
			localCB.ArgBufferStartOffsetInBytes = m_numSingleSidedMeshes * sizeof(CommandSig);
			localCB.MeshBufferStartIndex = m_numSingleSidedMeshes;

			m_rootSig.SetRootConstants(0, sizeof(localCB) / sizeof(DWORD), &localCB);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch((uint32_t)CeilUnsignedIntDiv(numDoubleSided, BUILD_NO_CULL_THREAD_GROUP_SIZE_X),
				1, 1);
		}

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	// GBuffer - draw the meshes that were visible last frame (potentially with false positives)
	{
		GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

		directCmdList.PIXBeginEvent("GBuffer_1st");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(directCmdList, "GBuffer_1st");

		directCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

		m_rootSig.SetRootSRV(2, m_meshInstances.GetGpuVA());
		m_rootSig.End(directCmdList);

		D3D12_VIEWPORT viewports[(int)SHADER_OUT::COUNT - 1] =
		{
			renderer.GetRenderViewport(),
			renderer.GetRenderViewport(),
			renderer.GetRenderViewport(),
			renderer.GetRenderViewport(),
			renderer.GetRenderViewport(),
			renderer.GetRenderViewport()
		};

		D3D12_RECT scissors[(int)SHADER_OUT::COUNT - 1] =
		{
			renderer.GetRenderScissor(),
			renderer.GetRenderScissor(),
			renderer.GetRenderScissor(),
			renderer.GetRenderScissor(),
			renderer.GetRenderScissor(),
			renderer.GetRenderScissor()
		};

		static_assert(ZetaArrayLen(viewports) == (int)SHADER_OUT::COUNT - 1, "bug");
		static_assert(ZetaArrayLen(scissors) == (int)SHADER_OUT::COUNT - 1, "bug");

		const Core::DefaultHeapBuffer& sceneIB = App::GetScene().GetMeshIB();
		Assert(sceneIB.IsInitialized(), "IB hasn't been built yet.");
		const auto ibGpuVa = sceneIB.GetGpuVA();

		D3D12_INDEX_BUFFER_VIEW ibv;
		ibv.BufferLocation = ibGpuVa;
		ibv.SizeInBytes = (UINT)sceneIB.GetDesc().Width;
		ibv.Format = DXGI_FORMAT_R32_UINT;

		directCmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		directCmdList.IASetIndexBuffer(ibv);
		directCmdList.RSSetViewportsScissorsRects((int)SHADER_OUT::COUNT - 1, viewports, scissors);

		Assert(m_inputDescriptors[(int)SHADER_IN_DESC::GBUFFERS_RTV].ptr > 0, "GBuffers RTV hasn't been set.");
		Assert(m_inputDescriptors[(int)SHADER_IN_DESC::CURR_DEPTH_BUFFER_DSV].ptr > 0, "Depth buffer DSV hasn't been set.");

		directCmdList.OMSetRenderTargets((int)SHADER_OUT::COUNT - 1, &m_inputDescriptors[SHADER_IN_DESC::GBUFFERS_RTV],
			true, &m_inputDescriptors[(int)SHADER_IN_DESC::CURR_DEPTH_BUFFER_DSV]);

		directCmdList.ResourceBarrier(m_indirectDrawArgs.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

		// single-sided meshes
		if (m_numSingleSidedMeshes)
		{
			directCmdList.SetPipelineState(m_graphicsPso[(int)PSO::ONE_SIDED]);

			directCmdList.ExecuteIndirect(m_cmdSig.Get(),
				m_numSingleSidedMeshes,
				m_indirectDrawArgs.GetResource(),
				0,
				m_indirectDrawArgs.GetResource(),
				m_counterSingleSidedBufferOffsetFirst);
		}

		// double-sided meshes
		if (m_numSingleSidedMeshes < m_numMeshesThisFrame)
		{
			const uint32_t numDoubleSided = m_numMeshesThisFrame - m_numSingleSidedMeshes;
			const uint32_t argBuffStartOffset = m_numSingleSidedMeshes * sizeof(CommandSig);

			directCmdList.SetPipelineState(m_graphicsPso[(int)PSO::DOUBLE_SIDED]);

			directCmdList.ExecuteIndirect(m_cmdSig.Get(),
				numDoubleSided,
				m_indirectDrawArgs.GetResource(),
				argBuffStartOffset,
				m_indirectDrawArgs.GetResource(),
				m_counterDoubleSidedBufferOffsetFirst);
		}

		// record the timestamp after execution
		gpuTimer.EndQuery(directCmdList, queryIdx);

		directCmdList.PIXEndEvent();
	}

	// depth pyramid
	{
		ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

		computeCmdList.PIXBeginEvent("DepthPyramid");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "DepthPyramid");

		computeCmdList.SetPipelineState(m_computePsos[(int)COMPUTE_SHADERS::DEPTH_PYRAMID]);
		m_rootSig.SetRootUAV(9, m_spdCounter.GetGpuVA());

		const int width = renderer.GetRenderWidth();
		const int height = renderer.GetRenderHeight();

		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(width, 64);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(height, 64);

		cbDepthPyramid localCB;
		uint32_t* mipUAVs = reinterpret_cast<uint32_t*>(localCB.Mips0_3);

		for (int i = 0; i < m_numMips; i++)
			*mipUAVs++ = m_descTable.GPUDesciptorHeapIndex(i);

		localCB.MipLevels = (uint16_t)m_numMips;
		localCB.NumThreadGroupsX = (uint16_t)dispatchDimX;
		localCB.NumThreadGroupsY = (uint16_t)dispatchDimY;
		localCB.Mip5DimX = (uint16_t)(width >> 6);
		localCB.Mip5DimY = (uint16_t)(height >> 6);
		m_rootSig.SetRootConstants(0, sizeof(localCB) / sizeof(DWORD), &localCB);
		m_rootSig.End(computeCmdList);

		D3D12_RESOURCE_BARRIER barriers[2];
		barriers[0] = Direct3DHelper::TransitionBarrier(m_currDepthBuffer,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		barriers[1] = Direct3DHelper::TransitionBarrier(m_depthPyramid.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	// build indirect draw args buffer -- with occlusion culling
	{
		ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

		computeCmdList.PIXBeginEvent("IndDrawArgs_OccCull");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "IndDrawArgs_OccCull");

		computeCmdList.SetPipelineState(m_computePsos[(int)COMPUTE_SHADERS::BUILD_IND_DRAW_ARGS_OCC_CULL]);

		D3D12_RESOURCE_BARRIER barriers[4];
		barriers[0] = Direct3DHelper::TransitionBarrier(m_visibilityBuffer.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		barriers[1] = Direct3DHelper::TransitionBarrier(m_indirectDrawArgs.GetResource(),
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		barriers[2] = Direct3DHelper::TransitionBarrier(m_currDepthBuffer,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_DEPTH_WRITE);
		barriers[3] = Direct3DHelper::TransitionBarrier(m_depthPyramid.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

		m_rootSig.SetRootUAV(8, m_visibilityBuffer.GetGpuVA());

		cbOcclussionCulling localCB;
		localCB.DepthPyramidSrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::SRV_ALL);
		localCB.DepthPyramidMip0DimX = (uint16_t)m_depthPyramidMip0DimX;
		localCB.DepthPyramidMip0DimY = (uint16_t)m_depthPyramidMip0DimY;
		localCB.NumDepthPyramidMips = (uint16_t)m_numMips;
		localCB.DepthThresh = m_occlusionTestDepthThresh;

		const Camera& cam = App::GetCamera();
		const float aspectRatio = float(m_depthPyramidMip0DimX) / m_depthPyramidMip0DimY;
		v_float4x4 vP = perspectiveReverseZ(aspectRatio, cam.GetFOV(), cam.GetNearZ());
		v_float4x4 vCurrV = load4x4(const_cast<float4x4a&>(cam.GetCurrView()));
		v_float4x4 vVP = mul(vCurrV, vP);

		localCB.ViewProj = store(vVP);

		// single-sided meshes
		if (m_numSingleSidedMeshes)
		{
			localCB.NumMeshes = m_numSingleSidedMeshes;
			localCB.CounterBufferOffset = m_counterSingleSidedBufferOffsetSecond;
			localCB.MeshBufferStartIndex = 0;
			localCB.ArgBufferStartOffsetInBytes = 0;

			m_rootSig.SetRootConstants(0, sizeof(localCB) / sizeof(DWORD), &localCB);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch((uint32_t)CeilUnsignedIntDiv(m_numSingleSidedMeshes, BUILD_OCC_CULL_THREAD_GROUP_SIZE_X),
				1, 1);
		}

		// double-sided meshes
		if (m_numSingleSidedMeshes < m_numMeshesThisFrame)
		{
			const uint32_t numDoubleSided = m_numMeshesThisFrame - m_numSingleSidedMeshes;
			localCB.NumMeshes = numDoubleSided;
			localCB.CounterBufferOffset = m_counterDoubleSidedBufferOffsetSecond;
			localCB.ArgBufferStartOffsetInBytes = m_numSingleSidedMeshes * sizeof(CommandSig);
			localCB.MeshBufferStartIndex = m_numSingleSidedMeshes;

			m_rootSig.SetRootConstants(0, sizeof(localCB) / sizeof(DWORD), &localCB);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch((uint32_t)CeilUnsignedIntDiv(numDoubleSided, BUILD_OCC_CULL_THREAD_GROUP_SIZE_X),
				1, 1);
		}

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	// GBuffer -- draw the instances that have become visible this frame
	{
		GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

		directCmdList.PIXBeginEvent("GBuffer_2nd");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(directCmdList, "GBuffer_2nd");

		directCmdList.ResourceBarrier(m_indirectDrawArgs.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

		// single-sided meshes
		if (m_numSingleSidedMeshes)
		{
			directCmdList.SetPipelineState(m_graphicsPso[(int)PSO::ONE_SIDED]);

			directCmdList.ExecuteIndirect(m_cmdSig.Get(),
				m_numSingleSidedMeshes,
				m_indirectDrawArgs.GetResource(),
				0,
				m_indirectDrawArgs.GetResource(),
				m_counterSingleSidedBufferOffsetSecond);
		}

		// double-sided meshes
		if (m_numSingleSidedMeshes < m_numMeshesThisFrame)
		{
			const uint32_t numDoubleSided = m_numMeshesThisFrame - m_numSingleSidedMeshes;
			const uint32_t argBuffStartOffset = m_numSingleSidedMeshes * sizeof(CommandSig);

			directCmdList.SetPipelineState(m_graphicsPso[(int)PSO::DOUBLE_SIDED]);

			directCmdList.ExecuteIndirect(m_cmdSig.Get(),
				numDoubleSided,
				m_indirectDrawArgs.GetResource(),
				argBuffStartOffset,
				m_indirectDrawArgs.GetResource(),
				m_counterDoubleSidedBufferOffsetSecond);
		}

		// record a copy that copies the counter to a cpu-readable buffer
		// PIX warns that following is not needed, yet the debug layer gives and error
		// that STATE_INDIRECT_ARGUMENT is invalid for copy!
		directCmdList.ResourceBarrier(m_indirectDrawArgs.GetResource(),
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
			D3D12_RESOURCE_STATE_COPY_SOURCE);

		directCmdList.CopyBufferRegion(m_readbackBuff.GetResource(),
			m_currFrameIdx * sizeof(uint32_t) * 4,
			m_indirectDrawArgs.GetResource(),
			m_counterSingleSidedBufferOffsetFirst,
			sizeof(uint32_t) * 4);

		// record the timestamp after execution
		gpuTimer.EndQuery(directCmdList, queryIdx);

		directCmdList.PIXEndEvent();

		// read the last completed counters (counters for some of the previous frames may be skipped)
		if (App::GetTimer().GetTotalFrameCount() > 1)
		{
			bool newData = false;
			const uint64_t completed = m_fence->GetCompletedValue();
			const int oldNextCompletedFrameIdx = m_nextCompletedFrameIdx;

			do
			{
				if (completed < m_fenceVals[m_nextCompletedFrameIdx])
					break;

				m_nextCompletedFrameIdx = m_nextCompletedFrameIdx < Constants::NUM_BACK_BUFFERS - 1 ? m_nextCompletedFrameIdx + 1 : 0;
				newData = true;
			} while (m_nextCompletedFrameIdx != oldNextCompletedFrameIdx);

			if (newData)
			{
				// undo the last add
				const int lastCompletedFrameIdx = m_nextCompletedFrameIdx > 0 ? m_nextCompletedFrameIdx - 1 : Constants::NUM_BACK_BUFFERS - 1;

				// safe to map the readback buffer -- the offset that is being read is guaranteed to not overlap
				// with any in-flight Copy commands 
				m_readbackBuff.Map();

				uint8_t* data = reinterpret_cast<uint8_t*>(m_readbackBuff.GetMappedMemory());
				uint32_t singleSidedCounterFirst;
				uint32_t doubleSidedCounterFirst;
				uint32_t singleSidedCounterSecond;
				uint32_t doubleSidedCounterSecond;

				const size_t startOffsetInBytes = lastCompletedFrameIdx * sizeof(uint32_t) * 4;
				memcpy(&singleSidedCounterFirst, data + startOffsetInBytes, sizeof(uint32_t));
				memcpy(&doubleSidedCounterFirst, data + startOffsetInBytes + sizeof(uint32_t), sizeof(uint32_t));
				memcpy(&singleSidedCounterSecond, data + startOffsetInBytes + sizeof(uint32_t) * 2, sizeof(uint32_t));
				memcpy(&doubleSidedCounterSecond, data + startOffsetInBytes + sizeof(uint32_t) * 3, sizeof(uint32_t));

				m_lastNumDrawCallsSubmitted = singleSidedCounterFirst + doubleSidedCounterFirst +
					singleSidedCounterSecond + doubleSidedCounterSecond;

				// avoid #occluded becoming negative
				m_lastNumDrawCallsSubmitted = Math::Min(m_lastNumDrawCallsSubmitted, m_numMeshesThisFrame);
				m_lastNumMeshes = m_numMeshesThisFrame;

				m_readbackBuff.Unmap();
			}
		}

		// report the last received values
		App::AddFrameStat("Scene", "OcclusionCulled",
			m_lastNumMeshes - m_lastNumDrawCallsSubmitted,
			m_lastNumMeshes);
	}
}

void GBufferPass::CreatePSOs(Span<DXGI_FORMAT> rtvs) noexcept
{
	for (int i = 0; i < (int)COMPUTE_SHADERS::COUNT; i++)
	{
		m_computePsos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
			s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[i]);
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DHelper::GetPSODesc(nullptr,
		(int)rtvs.size(),
		rtvs.data(),
		Constants::DEPTH_BUFFER_FORMAT);

	// reverse z
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;

	m_graphicsPso[(int)PSO::ONE_SIDED] = s_rpObjs.m_psoLib.GetGraphicsPSO((int)COMPUTE_SHADERS::COUNT + (int)PSO::ONE_SIDED,
		psoDesc,
		s_rpObjs.m_rootSig.Get(),
		COMPILED_VS[0],
		COMPILED_PS[0]);

	// disable backface culling for double-sided meshes
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	m_graphicsPso[(int)PSO::DOUBLE_SIDED] = s_rpObjs.m_psoLib.GetGraphicsPSO((int)COMPUTE_SHADERS::COUNT + (int)PSO::DOUBLE_SIDED,
		psoDesc,
		s_rpObjs.m_rootSig.Get(),
		COMPILED_VS[0],
		COMPILED_PS[0]);
}

void GBufferPass::CreateDepthPyramid() noexcept
{
	auto& renderer = App::GetRenderer();

	const int width = renderer.GetRenderWidth();
	const int height = renderer.GetRenderHeight();

	m_depthPyramidMip0DimX = width >> 1;
	m_depthPyramidMip0DimY = height >> 1;

	m_numMips = (uint32_t)log2f((float)Math::Max(width, height));
	Assert(m_numMips <= MAX_NUM_MIPS, "#mips can't exceed MAX_NUM_MIPS.");

	m_depthPyramid = renderer.GetGpuMemory().GetTexture2D("DepthPyramid",
		m_depthPyramidMip0DimX, m_depthPyramidMip0DimY,
		DXGI_FORMAT_R32_FLOAT,
		D3D12_RESOURCE_STATE_COMMON,
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS,
		(uint16_t)m_numMips);

	Direct3DHelper::CreateTexture2DSRV(m_depthPyramid, m_descTable.CPUHandle((uint32_t)DESC_TABLE::SRV_ALL));

	for (int i = 0; i < m_numMips; i++)
		Direct3DHelper::CreateTexture2DUAV(m_depthPyramid, m_descTable.CPUHandle(i), DXGI_FORMAT_R32_FLOAT, i);
}

void GBufferPass::ReloadShader() noexcept
{
	const int i = (int)COMPUTE_SHADERS::BUILD_IND_DRAW_ARGS_OCC_CULL;

	s_rpObjs.m_psoLib.Reload(i, "GBuffer\\BuildDrawIndArgs_OcclusionCull.hlsl", true);
	m_computePsos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
		s_rpObjs.m_rootSig.Get(), 
		COMPILED_CS[i]);
}

void GBufferPass::DepthThreshCallback(const Support::ParamVariant& p) noexcept
{
	m_occlusionTestDepthThresh = p.GetFloat().m_val;
}
