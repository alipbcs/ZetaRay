#include "RendererCore.h"
#include "CommandList.h"
#include "../App/Timer.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Util;

void GpuTimer::Init()
{
	auto& renderer = App::GetRenderer();

	m_directQueueFreq = renderer.GetCommandQueueTimeStampFrequency(D3D12_COMMAND_LIST_TYPE_DIRECT);
	m_computeQueueFreq = renderer.GetCommandQueueTimeStampFrequency(D3D12_COMMAND_LIST_TYPE_COMPUTE);

	// ticks/s to ticks/ms
	m_directQueueFreq /= 1000;
	m_computeQueueFreq /= 1000;

	D3D12_QUERY_HEAP_DESC desc{};
	desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	desc.Count = MAX_NUM_QUERIES * 2 * Constants::NUM_BACK_BUFFERS;
	desc.NodeMask = 0;

	auto* device = renderer.GetDevice();
	CheckHR(device->CreateQueryHeap(&desc, IID_PPV_ARGS(m_queryHeap.GetAddressOf())));

	for (int i = 0; i < ZetaArrayLen(m_timings); i++)
		m_timings[i].resize(MAX_NUM_QUERIES);

	m_readbackBuff = GpuMemory::GetReadbackHeapBuffer(sizeof(uint64_t) * desc.Count);

#ifdef _DEBUG
	m_readbackBuff.Resource()->SetName(L"Timing_Buffer");
#endif // _DEBUG

	CheckHR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));
}

void GpuTimer::Shutdown()
{
	for (int i = 0; i < ZetaArrayLen(m_timings); i++)
		m_timings[i].free_memory();

	m_readbackBuff.Reset();
}

Span<GpuTimer::Timing> GpuTimer::GetFrameTimings()
{
	if (App::GetTimer().GetTotalFrameCount() < 2)
		return Span(reinterpret_cast<Timing*>(NULL), 0);

	return Span(m_timings[Constants::NUM_BACK_BUFFERS].data(), m_queryCounts[Constants::NUM_BACK_BUFFERS]);
}

void GpuTimer::BeginFrame()
{
	Assert(m_frameQueryCount.load(std::memory_order_relaxed) == 0,
		"Attempting to begin a new frame while GpuTimer::Resolve() hasn't been called for the previous frame.");

	if (App::GetTimer().GetTotalFrameCount() >= 1)
	{
		// At this point, previous frame's queries have been submitted
		m_fenceVals[m_currFrameIdx] = m_nextFenceVal;
		// Advance the frame index
		m_currFrameIdx = m_currFrameIdx < Constants::NUM_BACK_BUFFERS - 1 ? m_currFrameIdx + 1 : 0;

		App::GetRenderer().SignalDirectQueue(m_fence.Get(), m_nextFenceVal++);

		// Are there been newly resolved timings?
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
			// Undo the last add
			const int lastCompletedFrameIdx = m_nextCompletedFrameIdx > 0 ? m_nextCompletedFrameIdx - 1 : Constants::NUM_BACK_BUFFERS - 1;

			m_readbackBuff.Map();
			uint8_t* data = reinterpret_cast<uint8_t*>(m_readbackBuff.MappedMemory());

			for (int i = 0; i < m_queryCounts[lastCompletedFrameIdx]; i++)
			{
				uint8_t* currPtr = data + sizeof(uint64_t) * i * 2;
				uint64_t beg, end;

				memcpy(&beg, currPtr, sizeof(uint64_t));
				memcpy(&end, currPtr + sizeof(uint64_t), sizeof(uint64_t));

				uint64_t freq = m_timings[lastCompletedFrameIdx][i].ExecutionQueue == D3D12_COMMAND_LIST_TYPE_DIRECT ?
					m_directQueueFreq : m_computeQueueFreq;
				m_timings[lastCompletedFrameIdx][i].Delta = (end - beg) / (double)freq;
			}

			m_readbackBuff.Unmap();

			if (m_queryCounts[lastCompletedFrameIdx])
			{
				m_timings[Constants::NUM_BACK_BUFFERS].clear();
				m_timings[Constants::NUM_BACK_BUFFERS].append_range(m_timings[lastCompletedFrameIdx].begin(),
					m_timings[lastCompletedFrameIdx].end(), true);
			}

			m_queryCounts[Constants::NUM_BACK_BUFFERS] = m_queryCounts[lastCompletedFrameIdx];
		}
	}

	//for (int i = 0; i < MAX_NUM_QUERIES; i++)
	//	m_timings[m_currFrameIdx][i].Reset();
}

uint32_t GpuTimer::BeginQuery(ComputeCmdList& cmdList, const char* name)
{
	const uint32_t queryIdx = m_frameQueryCount.fetch_add(1, std::memory_order_relaxed);
	Assert(queryIdx < MAX_NUM_QUERIES, "number of queries exceeded maximum allowed.");

	const auto n = Math::Min(Timing::MAX_NAME_LENGTH - 1, (int)strlen(name));
	memcpy(&m_timings[m_currFrameIdx][queryIdx].Name, name, n);
	m_timings[m_currFrameIdx][queryIdx].Name[n] = '\0';
	m_timings[m_currFrameIdx][queryIdx].Delta = 0.0;
	m_timings[m_currFrameIdx][queryIdx].ExecutionQueue = cmdList.GetType();

	const uint32_t heapIdx = MAX_NUM_QUERIES * 2 * m_currFrameIdx + queryIdx * 2;
	Assert((heapIdx & 0x1) == 0, "invalid query index.");
	cmdList.EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, heapIdx);

	return heapIdx;
}

void GpuTimer::EndQuery(ComputeCmdList& cmdList, uint32_t begHeapIdx)
{
	Assert(((begHeapIdx & 0x1) == 0) && (begHeapIdx >= MAX_NUM_QUERIES * 2 * m_currFrameIdx), "invalid query index.");
	const uint32_t endHeapIdx = begHeapIdx + 1;
	Assert(endHeapIdx < MAX_NUM_QUERIES * 2 * Constants::NUM_BACK_BUFFERS, "invalid query index.");

	cmdList.EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, endHeapIdx);
}

void GpuTimer::EndFrame(ComputeCmdList& cmdList)
{
	Assert(!m_readbackBuff.IsMapped(), "Readback buffer shouldn't be mapped while in use by the GPU.");
	const int queryCount = m_frameQueryCount.load(std::memory_order_acquire);
	m_queryCounts[m_currFrameIdx] = queryCount;

	if (queryCount == 0)
		return;

	uint32_t heapStartIdx = m_currFrameIdx * MAX_NUM_QUERIES * 2;
	uint64_t bufferOffsetBeg = heapStartIdx * sizeof(uint64_t);

	cmdList.PIXBeginEvent("GpuTimer");

	cmdList.ResolveQueryData(m_queryHeap.Get(), 
		D3D12_QUERY_TYPE_TIMESTAMP, 
		heapStartIdx, 
		queryCount * 2,
		m_readbackBuff.Resource(), 
		bufferOffsetBeg);

	cmdList.PIXEndEvent();

	m_frameQueryCount.store(0, std::memory_order_relaxed);
}

