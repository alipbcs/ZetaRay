// Some of the ideas in this implementation were Inspired by following: 
// https://levelup.gitconnected.com/organizing-gpu-work-with-directed-acyclic-graphs-f3fd5f2c2af3

#include "RenderGraph.h"
#include "Renderer.h"
#include "Direct3DHelpers.h"
#include "../Math/Common.h"
#include "CommandList.h"
#include "../Support/Task.h"
#include "../Win32/Timer.h"
#include "../Win32/Log.h"
#include <algorithm>
#include <xxHash-0.8.1/xxhash.h>
#include <imgui/imnodes.h>

#ifdef _DEBUG
#include <string>
#endif // _DEBUG

using namespace ZetaRay;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DHelper;

namespace
{
	inline const char* GetResStateName(D3D12_RESOURCE_STATES s) noexcept
	{
		switch (s)
		{
		case D3D12_RESOURCE_STATE_COMMON:
			return "COMMON_OR_PRESENT";
		case D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER:
			return "VERTEX_AND_CONSTANT_BUFFER";
		case D3D12_RESOURCE_STATE_INDEX_BUFFER:
			return "INDEX_BUFFER";
		case D3D12_RESOURCE_STATE_RENDER_TARGET:
			return "RENDER_TARGET";
		case D3D12_RESOURCE_STATE_UNORDERED_ACCESS:
			return "UNORDERED_ACCESS";
		case D3D12_RESOURCE_STATE_DEPTH_WRITE:
			return "DEPTH_WRITE";
		case D3D12_RESOURCE_STATE_DEPTH_READ:
			return "DEPTH_READ";
		case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
			return "NON_PIXEL_SHADER_RESOURCE";
		case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
			return "PIXEL_SHADER_RESOURCE";
		case D3D12_RESOURCE_STATE_COPY_DEST:
			return "COPY_DEST";
		case D3D12_RESOURCE_STATE_COPY_SOURCE:
			return "COPY_SOURCE";
		case D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE:
			return "RAYTRACING_ACCELERATION_STRUCTURE";
		case D3D12_RESOURCE_STATE_GENERIC_READ:
			return "GENERIC_READ";
		case D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE:
			return "ALL_SHADER_RESOURCE";
		default:
			return "UNKNOWN";
		}
	}

	struct Edge
	{
		int E0;
		int E1;
	};
}

//--------------------------------------------------------------------------------------
// RenderGraph
//--------------------------------------------------------------------------------------

void RenderGraph::Shutdown() noexcept
{
	m_frameResources.free();

	for (int i = 0; i < MAX_NUM_RENDER_PASSES; i++)
	{
		m_renderNodes[i].Inputs.free();
		m_renderNodes[i].Outputs.free();
		m_renderNodes[i].Barriers.free();
	}
}

void RenderGraph::Reset() noexcept
{
	//m_frameResources.clear();
	m_frameResources.resize(MAX_NUM_RESOURCES);

	// sort the frame resources so that window-dependant ones come after window-independant ones
	auto it = std::partition(m_frameResources.begin(), m_frameResources.begin() + m_prevFramesNumResources,
		[](ResourceMetadata& res)
		{
			return res.IsWindowSizeDependant == false;
		});

	const size_t numRemaining = it - m_frameResources.begin();

	for (size_t i = numRemaining; i < m_prevFramesNumResources; i++)
		m_frameResources[i].Reset();

	std::sort(m_frameResources.begin(), m_frameResources.begin() + numRemaining,
		[](ResourceMetadata& lhs, ResourceMetadata& rhs)
		{
			return lhs.ID < rhs.ID;
		});

	m_prevFramesNumResources = (int)numRemaining;
	m_currResIdx = (int)numRemaining;

	// reset the render nodes
	const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);

	for (int currNode = 0; currNode < numNodes; currNode++)
		m_renderNodes[currNode].Reset();

	m_currRenderPassIdx.store(0, std::memory_order_relaxed);
}

void RenderGraph::RemoveResource(uint64_t path) noexcept
{
	const int pos = FindFrameResource(path, 0, m_prevFramesNumResources);

	if (pos != -1)
		m_frameResources[pos].Reset();
}

void RenderGraph::BeginFrame() noexcept
{
	m_prevFramesNumResources = m_currResIdx.load(std::memory_order_relaxed);

	const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);
	m_numPassesPrevFrame = numNodes;
	m_currRenderPassIdx.store(0, std::memory_order_relaxed);

	//m_currFrameIdx = 1 - m_currFrameIdx;
	//m_currResIdx.store(0, std::memory_order_relaxed);

	// reset the producers
	for (auto& rm : m_frameResources)
	{
		rm.CurrProdIdx.store(0, std::memory_order_relaxed);
		
		for (int i = 0; i < MAX_NUM_PRODUCERS; i++)
			rm.Producers[i].Val = INVALID_NODE_HANDLE;
	}
}

int RenderGraph::FindFrameResource(uint64_t key, int beg, int end) noexcept
{
	if (end - beg == 0)
		return -1;

	end = (end == -1) ? m_currResIdx.load(std::memory_order_relaxed) : end;
	int mid = end >> 1;

	while (true)
	{
		if (end - beg <= 2)
			break;

		if (m_frameResources[mid].ID < key)
			beg = mid + 1;
		else
			end = mid + 1;

		mid = beg + ((end - beg) >> 1);
	}

	if (m_frameResources[beg].ID == key)
		return beg;
	else if (m_frameResources[mid].ID == key)
		return mid;

	return -1;
}

RenderNodeHandle RenderGraph::RegisterRenderPass(const char* name, RENDER_NODE_TYPE t, 
	fastdelegate::FastDelegate1<CommandList&> dlg) noexcept
{
	int h = m_currRenderPassIdx.fetch_add(1, std::memory_order_relaxed);
	Assert(h < MAX_NUM_RENDER_PASSES, "Number of render passes exceeded MAX_NUM_RENDER_PASSES");

	m_renderNodes[h].Reset(name, t, dlg);

	return RenderNodeHandle(h);
}

void RenderGraph::RegisterResource(ID3D12Resource* res, uint64_t path, D3D12_RESOURCE_STATES initState, bool isWindowSizeDependant) noexcept
{
	Assert(res == nullptr || path > DUMMY_RES::COUNT, "reource path ID can't take special value %llu", path);

	const int prevPos = FindFrameResource(path, 0, m_prevFramesNumResources);

	// existing resource
	if (prevPos != -1)
	{
		if(m_frameResources[prevPos].Res != res)
			m_frameResources[prevPos].Reset(path, res, initState, isWindowSizeDependant);

		return;
	}

	// new resource
	int pos = m_currResIdx.fetch_add(1, std::memory_order_relaxed);
	Assert(pos < MAX_NUM_RESOURCES, "Number of resources exceeded MAX_NUM_RESOURCES");

	m_frameResources[pos].Reset(path, res, initState, isWindowSizeDependant);
}

void RenderGraph::MoveToPostRegister() noexcept
{
	const int numResources = m_currResIdx.load(std::memory_order_relaxed);

	// sort the frame resources so that binary search can be performed
	std::sort(m_frameResources.begin(), m_frameResources.begin() + numResources,
		[](ResourceMetadata& lhs, ResourceMetadata& rhs)
		{
			return lhs.ID < rhs.ID;
		});

#ifdef _DEBUG
	for (int i = 0; i < numResources - 1; i++)
	{
		if (m_frameResources[i].ID == m_frameResources[i + 1].ID)
		{
			char name[64] = { '\0' };
			UINT n = sizeof(name);
			m_frameResources[i].Res->GetPrivateData(WKPDID_D3DDebugObjectName, &n, name);

			Assert(false, "Duplicate entries for resource %s.", name);
		}
	}
#endif // _DEBUG
}

void RenderGraph::AddInput(RenderNodeHandle h, uint64_t pathID, D3D12_RESOURCE_STATES expectedState) noexcept
{
	Assert(h.IsValid(), "Invalid handle");
	Assert(h.Val < m_currRenderPassIdx.load(std::memory_order_relaxed), "Invalid handle");
	Assert(expectedState & RendererConstants::READ_STATES, "Invalid read state.");

	// defer checking for invalid states until later on
	m_renderNodes[h.Val].Inputs.emplace_back(pathID, expectedState);
}

void RenderGraph::AddOutput(RenderNodeHandle h, uint64_t pathID, D3D12_RESOURCE_STATES expectedState) noexcept
{
	Assert(h.IsValid(), "Invalid handle");
	Assert(h.Val < m_currRenderPassIdx.load(std::memory_order_relaxed), "Invalid handle");
	Assert(expectedState & RendererConstants::WRITE_STATES, "Invalid write state.");
	Assert(m_renderNodes[h.Val].Type != RENDER_NODE_TYPE::ASYNC_COMPUTE || !(expectedState & RendererConstants::INVALID_COMPUTE_STATES),
		"state transition to %u is not supported on an async-compute command list.", expectedState);

	m_renderNodes[h.Val].Outputs.emplace_back(pathID, expectedState);

	const size_t idx = FindFrameResource(pathID);
	Assert(idx != -1, "Invalid resource path %llu.", pathID);	

	const int prodIdx = m_frameResources[idx].CurrProdIdx.fetch_add(1, std::memory_order_relaxed);
	Assert(prodIdx < MAX_NUM_PRODUCERS, "Number of producers for each resource can't exceed MAX_NUM_PRODUCERS");
//	Assert(prodIdx == 0 || m_renderNodes[m_frameResources[idx].Producers[prodIdx - 1].Val].Type == m_renderNodes[h.Val].Type,
//		"All the producers need to have the same type.");

	m_frameResources[idx].Producers[prodIdx] = h;
}

void RenderGraph::Build(TaskSet& ts) noexcept
{
	const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);
	Assert(numNodes > 0, "no render nodes");

	for (int i = 0; i < numNodes; i++)
		m_renderNodes[i].Indegree = (int)m_renderNodes[i].Inputs.size();

	// adjacent tail nodes
	SmallVector<RenderNodeHandle, App::PoolAllocator> adjacentTailNodes[MAX_NUM_RENDER_PASSES];

	// add the graph edges. For each input of node N, add an edge from 
	// that input's producer node (previously populated by AddOutput) to N
	for (int currNode = 0; currNode < numNodes; currNode++)
	{
		RenderNode& node = m_renderNodes[currNode];

		for (Dependency& input : node.Inputs)
		{
			const size_t idx = FindFrameResource(input.ResID);
			Assert(idx != -1, "Resource ID %u was not found.", input.ResID);

			const int numProducers = m_frameResources[idx].CurrProdIdx.load(std::memory_order_relaxed);

			// null resources or resources that were produced in prior frames
			if (numProducers == 0)
			{
				node.Indegree--;
				Assert(node.Indegree >= 0, "Invalid indegree for node %s.", node.Name);
			}
			// each producer needs to decrement the dependency counter
			else
				node.Indegree += numProducers - 1;	// -1 to avoid double counting

			for (int prod = 0; prod < numProducers; prod++)
			{
				const int prodHandle = m_frameResources[idx].Producers[prod].Val;

				// workaround for when resource is set as both input and output for some node, otherwise there'd be cycle
				if (currNode == prodHandle)
				{
					node.Indegree--;

					const int numOutputs = (int)node.Outputs.size();
					Assert(numOutputs > 0, "invalid graph.");

					// for pass P, resource R is ping ponged between input & output and may appear as both 
					// an input and output of P, with possibly different states. Since barriers are executed "prior" 
					// to recording, this scenario can't be handled. As a workaround, the render graph takes cares of 
					// transitioning R into its input state, while further transitions (ping-ponging) for R inside P 
					// must be handled manually. R's state must be restored to its input state, otherwise actual state 
					// and render graph's state go out of sync.
					for (int i = 0; i < numOutputs; i++)
					{
						if (node.Outputs[i].ResID == input.ResID)
						{
							node.OutputMask |= (1 << i);
							break;
						}
					}
				}
				else
					adjacentTailNodes[prodHandle].push_back(RenderNodeHandle(currNode));
			}
		}
	}

	RenderNodeHandle mapping[MAX_NUM_RENDER_PASSES];

	Sort(adjacentTailNodes, mapping);

	// at this point "m_frameResources[_].Producers" is invalid since "m_renderNodes" was sorted. "mapping" must be used instead
	InsertResourceBarriers(mapping);
	BuildTaskGraph(ts);

#ifdef _DEBUG
	//Log();
#endif // _DEBUG
}

void RenderGraph::BuildTaskGraph(TaskSet& ts) noexcept
{
	// Task-level dependency cases:
	// 1. From nodes with batchIdx i to nodes with batchIdx i + 1
	// 2. From gpuDep(node) to node

	// GPU dependency & unsupported barriers:
	//  - If C has an unsupported barrier, add a barrier Task immediately before
	// the tasks from batch index B where B = C.batchIdx
	//  - Remove C's GPU dependency (if any), then add a GPU dependency from T to C

	const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);

	int prevBatchOffset = 0;
	int prevBatchSize = 0;
	int currBatchSize = 0;
	int currBatchIdx = 0;

	for (int currNode = 0; currNode < numNodes; currNode++)
	{
		if (m_renderNodes[currNode].BatchIdx != currBatchIdx)
		{
			prevBatchOffset = currNode - currBatchSize;
			prevBatchSize = currBatchSize;
			currBatchSize = 0;
			currBatchIdx = m_renderNodes[currNode].BatchIdx;
		}

		m_renderNodes[currNode].TaskH = ts.EmplaceTask(m_renderNodes[currNode].Name, [this, currNode, numNodes]() noexcept
			{
				auto& renderer = App::GetRenderer();
				auto& gpuTimer = renderer.GetGpuTimer();
				RenderNode& node = m_renderNodes[currNode];

				// get a command list
				ComputeCmdList* cmdList = nullptr;

				if (node.Type != RENDER_NODE_TYPE::ASYNC_COMPUTE)
					cmdList = static_cast<ComputeCmdList*>(renderer.GetGraphicsCmdList());
				else
					cmdList = renderer.GetComputeCmdList();

				cmdList->SetName(node.Name);

				if (node.HasUnsupportedBarrier)
				{
					CommandList* cmdList = renderer.GetGraphicsCmdList();
					GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(*cmdList);
					directCmdList.SetName("Barrier");

					directCmdList.TransitionResource(node.Barriers.begin(), (UINT)node.Barriers.size());
					uint64_t f = renderer.ExecuteCmdList(cmdList);

					renderer.WaitForDirectQueueOnComputeQueue(f);
				}
				else if (node.Barriers.size() > 0)
				{
					cmdList->TransitionResource(node.Barriers.begin(), (UINT)node.Barriers.size());
				}

				const uint32_t queryIdx = gpuTimer.BeginQuery(cmdList, node.Name);	// record the timestamp prior to execution
				node.Dlg(*cmdList);													// record
				gpuTimer.EndQuery(cmdList, queryIdx);								// record the timestamp after execution

				// wait for possible GPU fence
				if (!node.HasUnsupportedBarrier && node.GpuDepSourceIdx.Val != -1)
				{
					uint64_t f = m_renderNodes[node.GpuDepSourceIdx.Val].CompletionFence;
					Assert(f != -1, "Gpu hasn't finished executing");

					if (node.Type == RENDER_NODE_TYPE::ASYNC_COMPUTE)
						renderer.WaitForDirectQueueOnComputeQueue(f);
					else
						renderer.WaitForComputeQueueOnDirectQueue(f);
				}

				if (currNode == numNodes - 1)
					gpuTimer.EndFrame(cmdList);

				// submit
				node.CompletionFence = renderer.ExecuteCmdList(cmdList);
			});

		if (currBatchIdx > 0)
		{
			// there must be a dependency regardless of type
			for (int i = prevBatchOffset; i < prevBatchOffset + prevBatchSize; i++)
				ts.AddOutgoingEdge(m_renderNodes[i].TaskH, m_renderNodes[currNode].TaskH);
		}

		currBatchSize++;
	}
}

void RenderGraph::Sort(Span<SmallVector<RenderNodeHandle, App::PoolAllocator>> adjacentTailNodes, Span<RenderNodeHandle> mapping) noexcept
{
	const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);
	RenderNodeHandle sorted[MAX_NUM_RENDER_PASSES];
	int currIdx = 0;
	
	// move all the nodes with zero indegree to sorted
	for (int currNode = 0; currNode < numNodes; currNode++)
	{
		RenderNode& node = m_renderNodes[currNode];

		if (node.Indegree == 0)
		{
			// batchIdx is zero there are no dependencies
			sorted[currIdx++] = RenderNodeHandle(currNode);
			node.BatchIdx = 0;
		}
	}

	Assert(currIdx > 0, "Graph is not a DAG- no node with 0 dependencies.");

	// topological sort
	for (int currNode = 0; currNode < numNodes; currNode++)
	{
		Assert(sorted[currNode].IsValid(), "invalid handle");
		const int currHandle = sorted[currNode].Val;

		for (RenderNodeHandle adjacent : adjacentTailNodes[currHandle])
		{
			if (--m_renderNodes[adjacent.Val].Indegree == 0)
				sorted[currIdx++] = adjacent;
		}
	}

	Assert(numNodes == currIdx, "Graph is not a DAG");

	// length of the longest path for every node in DAG
	for (int i = 0; i < numNodes; i++)
	{
		RenderNodeHandle currHandle = sorted[i];

		for (RenderNodeHandle adjacent : adjacentTailNodes[currHandle.Val])
		{
			m_renderNodes[adjacent.Val].BatchIdx = std::max(
				m_renderNodes[currHandle.Val].BatchIdx + 1,
				m_renderNodes[adjacent.Val].BatchIdx);
		}
	}

	std::sort(sorted, sorted + numNodes, [this](const RenderNodeHandle& lhs, const RenderNodeHandle& rhs)
		{
			return m_renderNodes[lhs.Val].BatchIdx < m_renderNodes[rhs.Val].BatchIdx;
		});

	// Producer Handle to sorted array index mapping.
	// Producer handles were specified using the unsorted index. This maps those
	// to sorted order as subsequent processing uses the sorted one:
	//		original: [0, 1, 2, 3, 4, 5]
	//		sorted:   [3, 2, 1, 4, 0, 5]
	//		mapping:  [4, 2, 1, 0, 3, 5]
	//
	// e.g. Producer handle 0 is now located at mapping[0] = 4
	for (int currNode = 0; currNode < numNodes; currNode++)
		mapping[sorted[currNode].Val] = RenderNodeHandle(currNode);

	// shuffle
	RenderNode tempRenderNodes[MAX_NUM_RENDER_PASSES];

	for (int currNode = 0; currNode < numNodes; currNode++)
		tempRenderNodes[currNode] = ZetaMove(m_renderNodes[sorted[currNode].Val]);

	for (int currNode = 0; currNode < numNodes; currNode++)
		m_renderNodes[currNode] = ZetaMove(tempRenderNodes[currNode]);

	// TODO can be avoided
//	std::sort(m_renderNodes, m_renderNodes + numNodes, [this](const RenderNode& lhs, const RenderNode& rhs)
//		{
//			return lhs.BatchIdx < rhs.BatchIdx;
//		});
}

void RenderGraph::InsertResourceBarriers(Span<RenderNodeHandle> mapping) noexcept
{
	const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);

	// Using ordering imposed by sorted, largest index of the node on the Direct/Compute queue with which 
	// a compute/Direct node has already synced (see case b below). Note that this is an index into "sorted"
	// not a Handle
	int lastDirQueueHandle = 0;
	int lastComputeQueueHandle = 0;

	// helper to return last*Idx based on the node type
	auto getLastSyncedIdx = [&lastDirQueueHandle, &lastComputeQueueHandle](RENDER_NODE_TYPE t)
	{
		return t == RENDER_NODE_TYPE::ASYNC_COMPUTE ? &lastDirQueueHandle : &lastComputeQueueHandle;
	};

	// Workflow:
	// 1. For each input resource R:
	//		 - if R.state != expected -> add a barrier (e.g. RTV to SRV)
	//		 - if stateBefore(== R.state) is unsupported -> set hasUnsupportedBarriers
	//       - if producer is on a different queue, add a gpu sync, but only if an earlier
	//		   task hasn't synced already (see cases below)
	//
	// 2. For each output resource R:
	//		 - if R.state != expected -> add a barrier (e.g. SRV to UAV)
	//		 - if stateBefore(== R.state) is unsupported -> set hasUnsupportedBarriers

	// iterate by execution order (i.e. sorted by batch-index)
	for (int currNode = 0; currNode < numNodes; currNode++)
	{
		RenderNode& node = m_renderNodes[currNode];
		const bool isAsyncCompute = node.Type == RENDER_NODE_TYPE::ASYNC_COMPUTE;
		RenderNodeHandle largestProducerSortedHandle;	// i.e. index in sorted (execution) order

		//
		// inputs
		//
		for (Dependency& currInputRes : node.Inputs)
		{
			if (currInputRes.ResID < DUMMY_RES::COUNT)
				continue;

			const size_t inputFrameResIdx = FindFrameResource(currInputRes.ResID);
			Assert(inputFrameResIdx != -1, "Resource %llu was not found.", currInputRes.ResID);
			const D3D12_RESOURCE_STATES inputResState = m_frameResources[inputFrameResIdx].State;

			if (!(inputResState & currInputRes.ExpectedState))
			{
				// unsupported stateAfter should've been caught earlier
				node.HasUnsupportedBarrier = node.HasUnsupportedBarrier || 
					(isAsyncCompute && (inputResState & RendererConstants::INVALID_COMPUTE_STATES));
				node.Barriers.push_back(TransitionBarrier(m_frameResources[inputFrameResIdx].Res,
					inputResState,
					currInputRes.ExpectedState));

				// update the resource state
				m_frameResources[inputFrameResIdx].State = currInputRes.ExpectedState;
			}

			// If the input producer is on a different command queue, a GPU cross-queue sync is required.
			// (numbers correspond to index in the execution order)
			// 
			// Cases:
			//
			// a. 5 only needs to sync with 4.
			//
			//		Queue2:		1------> 3 ------> 5 ------> 7
			//									   |         |
			//					|--------|-------------------
			//		Queue2: 	2 -----> 4 ------> 6
			//
			//		   
			// b. since 4 has synced with 1, 6 no longer needs to sync with 1
			//
			//		Queue1:		1------> 2 ------> 3
			//					|-----------------
			//					|		 		  |
			//		Queue2: 	4 -----> 5 -----> 6

			// find the biggest producer batch-index (case a)
			const int numProducers = m_frameResources[inputFrameResIdx].CurrProdIdx.load(std::memory_order_relaxed);
			//Assert(m_frameResources[inputFrameResIdx].Res == nullptr || 
			//	numProducers > 0, "No producer for resource %llu", m_frameResources[inputFrameResIdx].ID);

			for (int i = 0; i < numProducers; i++)
			{
				RenderNodeHandle unsortedHandle = m_frameResources[inputFrameResIdx].Producers[i];
				RenderNodeHandle sortedHandle = mapping[unsortedHandle.Val];
				const bool producerOnDifferentQueue = 
					(isAsyncCompute && m_renderNodes[sortedHandle.Val].Type != RENDER_NODE_TYPE::ASYNC_COMPUTE) ||
					(!isAsyncCompute && m_renderNodes[sortedHandle.Val].Type == RENDER_NODE_TYPE::ASYNC_COMPUTE);
				
				if (producerOnDifferentQueue)
				{
					Assert(m_renderNodes[sortedHandle.Val].BatchIdx < node.BatchIdx, "Invalid graph");
					// case a
					largestProducerSortedHandle.Val = std::max(largestProducerSortedHandle.Val, sortedHandle.Val);
				}
			}
		}

		// case b
		if (largestProducerSortedHandle.Val != -1 && *getLastSyncedIdx(node.Type) < largestProducerSortedHandle.Val)
		{
			*getLastSyncedIdx(node.Type) = largestProducerSortedHandle.Val;
			node.GpuDepSourceIdx = largestProducerSortedHandle;
		}

		//
		// outputs
		//
		int i = 0;

		for (Dependency& currOutputRes : node.Outputs)
		{
			if (currOutputRes.ResID < DUMMY_RES::COUNT)
				continue;

			const bool skipBarrier = ((1 << i++) & node.OutputMask);

			const size_t ouputFrameResIdx = FindFrameResource(currOutputRes.ResID);
			Assert(ouputFrameResIdx != -1, "Resource %ull was not found.", currOutputRes.ResID);
			const D3D12_RESOURCE_STATES outputResState = m_frameResources[ouputFrameResIdx].State;

			if (!skipBarrier && !(m_frameResources[ouputFrameResIdx].State & currOutputRes.ExpectedState))
			{
				// unsupported resourceAfter should've been caught earlier
				node.HasUnsupportedBarrier = node.HasUnsupportedBarrier || 
					(isAsyncCompute && (outputResState & RendererConstants::INVALID_COMPUTE_STATES));
				node.Barriers.push_back(TransitionBarrier(m_frameResources[ouputFrameResIdx].Res,
					outputResState,
					currOutputRes.ExpectedState));
			}

			// update the resource state
			m_frameResources[ouputFrameResIdx].State = currOutputRes.ExpectedState;
		}
	}

	// temporary solution; assumes that "someone" will transition backbuffer to Present state
	int idx = FindFrameResource(App::GetRenderer().GetCurrBackBuffer().GetPathID());
	//Assert(idx != -1, "Current backbuffer was not found in frame resources");
	if(idx != -1)
		m_frameResources[idx].State = D3D12_RESOURCE_STATE_PRESENT;
}

void RenderGraph::DebugDrawGraph() noexcept
{
	const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);
	const bool needsReorder = m_numPassesPrevFrame != numNodes;

	ImNodes::BeginNodeEditor();

	int batchSize[MAX_NUM_RENDER_PASSES];
	memset(batchSize, 0, sizeof(int) * MAX_NUM_RENDER_PASSES);

	int currBatchIdx = 0;

	// compute batch sizes
	{
		int currBatchSize = 0;

		for (int currNode = 0; currNode < numNodes; currNode++)
		{
			if (m_renderNodes[currNode].BatchIdx != currBatchIdx)
			{
				batchSize[currBatchIdx] = currBatchSize;

				currBatchSize = 0;
				currBatchIdx = m_renderNodes[currNode].BatchIdx;
			}

			currBatchSize++;
		}

		Assert(currBatchIdx < MAX_NUM_RENDER_PASSES, "out-of-bound write");
		batchSize[currBatchIdx] = currBatchSize;
	}

	const int numBatches = currBatchIdx + 1;
	int currBatchStartPin = 0;
	int currBatchInputPin = 0;
	int currBatchOutputPin = 0;
	currBatchIdx = 0;
	int idxInBatch = 0;

	for (int currNode = 0; currNode < numNodes; currNode++)
	{
		if (m_renderNodes[currNode].BatchIdx != currBatchIdx)
		{
			const int prevBatchSize = currBatchIdx > 0 ? batchSize[currBatchIdx - 1] : 0;
			const int currBatchSize = batchSize[currBatchIdx];
			const int nextBatchSize = currBatchIdx + 1 < numBatches ? batchSize[currBatchIdx + 1] : 0;

			currBatchIdx = m_renderNodes[currNode].BatchIdx;
			currBatchStartPin += currBatchSize * prevBatchSize + nextBatchSize * currBatchSize;

			currBatchInputPin = 0;
			currBatchOutputPin = 0;
			idxInBatch = 0;
		}

		Assert(currBatchIdx >= 0 && currBatchIdx < numBatches, "out-of-bound access");

		ImNodes::BeginNode(currNode);

		ImNodes::BeginNodeTitleBar();
		ImGui::Text("\t%d. %s, Batch: %d, (GPU dep %d)\n", currNode, m_renderNodes[currNode].Name,
			m_renderNodes[currNode].BatchIdx, m_renderNodes[currNode].GpuDepSourceIdx.Val);
		ImNodes::EndNodeTitleBar();

		for (auto b : m_renderNodes[currNode].Barriers)
		{
			char buff[64] = { '\0' };
			UINT n = sizeof(buff);
			CheckHR(b.Transition.pResource->GetPrivateData(WKPDID_D3DDebugObjectName, &n, buff));

			ImGui::Text("\t\tRes: %s\n\tBefore: %s\nAfter: %s",
				buff,
				GetResStateName(b.Transition.StateBefore),
				GetResStateName(b.Transition.StateAfter));
		}

		const int prevBatchSize = currBatchIdx > 0 ? batchSize[currBatchIdx - 1] : 0;
		const int currBatchSize = batchSize[currBatchIdx];
		const int nextBatchSize = currBatchIdx + 1 < numBatches ? batchSize[currBatchIdx + 1] : 0;

		for (int i = 0; i < prevBatchSize; i++)
		{
			int p = currBatchStartPin + currBatchInputPin;
			ImNodes::BeginInputAttribute(p);
			ImNodes::EndInputAttribute();

			currBatchInputPin++;
		}
		
		for (int i = 0; i < nextBatchSize; i++)
		{
			int p = currBatchStartPin + currBatchSize * prevBatchSize + currBatchOutputPin;
			ImNodes::BeginOutputAttribute(p);
			ImNodes::EndOutputAttribute();

			currBatchOutputPin++;
		}
				
		ImNodes::EndNode();

		if (needsReorder)
		{
			const float x = currBatchIdx * 280.0f;
			const float y = 50.0f + idxInBatch++ * (50.0f + m_renderNodes[currNode].Barriers.size() * 50.0f);
			ImNodes::SetNodeEditorSpacePos(currNode, ImVec2(x, y));
		}
			//ImNodes::SetNodeScreenSpacePos(currNode, ImVec2(currBatchIdx * 400.0f, 50.0f + idxInBatch++ * 150.0f));
			//ImNodes::SetNodeGridSpacePos(currNode, ImVec2(currBatchIdx * 400.0f, 50.0f + idxInBatch++ * 150.0f));
	}

	currBatchIdx = 0;
	int currEdge = 0;
	currBatchStartPin = 0;
	int batchOutpinStart = 0;
	int nextBatchInpinStart = batchSize[0] * batchSize[1];

	for (int currNode = 0; currNode < numNodes; currNode++)
	{
		if (m_renderNodes[currNode].BatchIdx != currBatchIdx)
		{
			currBatchIdx = m_renderNodes[currNode].BatchIdx;

			const int prevPrevBatchSize = currBatchIdx > 1 ? batchSize[currBatchIdx - 2] : 0;
			const int prevBatchSize = currBatchIdx > 0 ? batchSize[currBatchIdx - 1] : 0;
			const int currBatchSize = batchSize[currBatchIdx];
			const int nextBatchSize = currBatchIdx + 1 < numBatches ? batchSize[currBatchIdx + 1] : 0;

			currBatchStartPin += prevPrevBatchSize * prevBatchSize + prevBatchSize * currBatchSize;
			batchOutpinStart = currBatchStartPin + currBatchSize * prevBatchSize;
			nextBatchInpinStart = batchOutpinStart + nextBatchSize * currBatchSize;

			idxInBatch = 0;
		}

		const int prevBatchSize = currBatchIdx > 0 ? batchSize[currBatchIdx - 1] : 0;
		const int currBatchSize = batchSize[currBatchIdx];
		const int nextBatchSize = currBatchIdx + 1 < numBatches ? batchSize[currBatchIdx + 1] : 0;

		//const int targetPinStart = batchOutpinStart + nextBatchSize * currBatchSize + idxInBatch;

		for (int i = 0; i < nextBatchSize; i++)
		{
			// in between Begin|EndAttribute calls, you can call ImGui
			// UI functions
			//ImGui::Text("out_%d", i);
			const int t = nextBatchInpinStart + i * currBatchSize + idxInBatch;
			ImNodes::Link(currEdge++, batchOutpinStart++, t);
		}

		idxInBatch++;
	}

	ImNodes::MiniMap(0.3f, ImNodesMiniMapLocation_BottomLeft);
	ImNodes::EndNodeEditor();
}

#ifdef _DEBUG
void RenderGraph::Log() noexcept
{
	const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);

	std::string formattedRenderGraph;
	formattedRenderGraph.reserve(2048);

	char temp[256];
	stbsp_snprintf(temp, sizeof(temp), "\nRenderGraph for frame %llu, #batches = %d\n", App::GetTimer().GetTotalFrameCount(),
		m_renderNodes[numNodes - 1].BatchIdx);
	formattedRenderGraph += temp;

	int currBatch = -1;
	temp[0] = '\0';

	for (int currNode = 0; currNode < numNodes; currNode++)
	{
		if (m_renderNodes[currNode].BatchIdx != currBatch)
		{
			currBatch = m_renderNodes[currNode].BatchIdx;
			stbsp_snprintf(temp, sizeof(temp), "Batch %d\n", currBatch);
			formattedRenderGraph += temp;
		}

		stbsp_snprintf(temp, sizeof(temp), "\t%d. %s (GPU dep %d)\n", currNode, m_renderNodes[currNode].Name, 
			m_renderNodes[currNode].GpuDepSourceIdx.Val);
		formattedRenderGraph += temp;

		for (auto b : m_renderNodes[currNode].Barriers)
		{
			char buff[64] = { '\0' };
			UINT n = sizeof(buff);
			CheckHR(b.Transition.pResource->GetPrivateData(WKPDID_D3DDebugObjectName, &n, buff));

			stbsp_snprintf(temp, sizeof(temp), "\t\tRes: %s, Before: %s, After: %s\n",
				buff,
				GetResStateName(b.Transition.StateBefore),
				GetResStateName(b.Transition.StateAfter));

			formattedRenderGraph += temp;
		}
	}

	formattedRenderGraph += '\n';

	LOG(formattedRenderGraph.c_str());
}
#endif