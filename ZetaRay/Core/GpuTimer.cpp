#include "GpuTimer.h"
#include "Renderer.h"
#include "../Utility/Error.h"
#include "CommandList.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;

void GpuTimer::Init() noexcept
{
	auto& renderer = App::GetRenderer();

	m_directQueueFreq = renderer.GetCommandQueueTimeStampFrequency(D3D12_COMMAND_LIST_TYPE_DIRECT);
	m_computeQueueFreq = renderer.GetCommandQueueTimeStampFrequency(D3D12_COMMAND_LIST_TYPE_DIRECT);

	// ticks/s to ticks/ms
	m_directQueueFreq /= 1000;
	m_computeQueueFreq /= 1000;

	D3D12_QUERY_HEAP_DESC desc{};
	desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	desc.Count = MAX_NUM_QUERIES * 2 * RendererConstants::NUM_BACK_BUFFERS;
	desc.NodeMask = 0;

	auto* device = renderer.GetDevice();
	CheckHR(device->CreateQueryHeap(&desc, IID_PPV_ARGS(m_queryHeap.GetAddressOf())));

	for (int i = 0; i < RendererConstants::NUM_BACK_BUFFERS; i++)
	{
		m_timings[i].resize(MAX_NUM_QUERIES);
	}

	m_readbackBuff = renderer.GetGpuMemory().GetReadbackHeapBuffer(sizeof(uint64_t) * desc.Count);
	m_readbackBuff.GetResource()->SetName(L"Timing_Buffer");
}

void GpuTimer::Shutdown() noexcept
{
	for (int i = 0; i < RendererConstants::NUM_BACK_BUFFERS; i++)
	{
		m_timings[i].free_memory();
	}
}

void GpuTimer::BeginFrame() noexcept
{
	Assert(m_queryCount[m_currFrameIdx].load(std::memory_order_relaxed) == 0,
		"Attempting to begin a new frame while GpuTimer::Resolve hasn't been called yet for previous frame.");

	m_currFrameIdx = m_currFrameIdx + 1 < RendererConstants::NUM_BACK_BUFFERS ? m_currFrameIdx + 1 : 0;

	for (int i = 0; i < MAX_NUM_QUERIES; i++)
		m_timings[m_currFrameIdx][i].Reset();
}

uint32_t GpuTimer::BeginQuery(ComputeCmdList* cmdList, const char* name) noexcept
{
	uint32_t queryIdx = m_queryCount[m_currFrameIdx].fetch_add(1, std::memory_order_relaxed);
	Assert(queryIdx < MAX_NUM_QUERIES, "number of queries exceeded maximum allowed.");

	auto n = std::min(Timing::MAX_NAME_LENGTH - 1, (int)strlen(name));
	memcpy(&m_timings[m_currFrameIdx][queryIdx].Name, name, n);
	m_timings[m_currFrameIdx][queryIdx].Name[n] = '\0';
	m_timings[m_currFrameIdx][queryIdx].Delta = 0.0;
	m_timings[m_currFrameIdx][queryIdx].ExecutionQueue = cmdList->GetType();

	uint32_t heapIdx = MAX_NUM_QUERIES * 2 * m_currFrameIdx + queryIdx * 2;
	cmdList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, heapIdx);

	return heapIdx;
}

void GpuTimer::EndQuery(ComputeCmdList* cmdList, uint32_t begHeapIdx) noexcept
{
	Assert((int)begHeapIdx >= MAX_NUM_QUERIES * 2 * m_currFrameIdx, "invalid query index.");
	uint32_t endHeapIdx = begHeapIdx + 1;
	Assert(endHeapIdx < MAX_NUM_QUERIES * 2 * RendererConstants::NUM_BACK_BUFFERS, "invalid query index.");

	cmdList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, endHeapIdx);
}

bool GpuTimer::EndFrame(ComputeCmdList* cmdList) noexcept
{
	int queryCount = m_queryCount[m_currFrameIdx].load(std::memory_order_acquire) - 1;
	m_numQueryHist[m_currFrameIdx] = queryCount;

	if (queryCount < 1)
		return false;

	uint32_t heapStartIdx = m_currFrameIdx * MAX_NUM_QUERIES * 2;
	uint64_t bufferOffsetBeg = heapStartIdx * sizeof(uint64_t);

	cmdList->PIXBeginEvent("GpuTimer");

	// ResolveQueryData performs a batched operation that writes query data into a destination 
	// buffer. Query data is written contiguously to the destination buffer
	cmdList->ResolveQueryData(m_queryHeap.Get(), 
		D3D12_QUERY_TYPE_TIMESTAMP, 
		heapStartIdx, 
		queryCount * 2,
		m_readbackBuff.GetResource(), 
		bufferOffsetBeg);

	cmdList->PIXEndEvent();

	m_readbackBuff.Map();
	uint8_t* data = reinterpret_cast<uint8_t*>(m_readbackBuff.GetMappedMemory());

	for (int i = 0; i < queryCount; i++)
	{
		uint8_t* currPtr = data + sizeof(uint64_t) * i * 2;
		uint64_t beg, end;

		memcpy(&beg, currPtr, sizeof(uint64_t));
		memcpy(&end, currPtr + sizeof(uint64_t), sizeof(uint64_t));

		uint64_t freq = m_timings[m_currFrameIdx][i].ExecutionQueue == D3D12_COMMAND_LIST_TYPE_DIRECT ? m_directQueueFreq : m_computeQueueFreq;
		m_timings[m_currFrameIdx][i].Delta = (end - beg) / (double)freq;
	}

	m_readbackBuff.Unmap();
	m_queryCount[m_currFrameIdx].store(0, std::memory_order_relaxed);

	return true;
}

