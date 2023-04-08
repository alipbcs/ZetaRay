#include "DescriptorHeap.h"
#include "RendererCore.h"
#include "../Utility/Error.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;

//--------------------------------------------------------------------------------------
// DescriptorTable
//--------------------------------------------------------------------------------------

DescriptorTable::DescriptorTable(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle,
	uint32_t numDesc, 
	uint32_t descSize,
	DescriptorHeap* heap,
	uint32_t internalVal) noexcept
	: m_baseCpuHandle(cpuHandle),
	m_baseGpuHandle(gpuHandle),
	m_numDescriptors(numDesc),
	m_descHeap(heap),
	m_descriptorSize(descSize),
	m_internal(internalVal)
{
}

DescriptorTable::~DescriptorTable() noexcept
{
	Reset();
}

DescriptorTable::DescriptorTable(DescriptorTable&& other) noexcept
	: m_baseCpuHandle(other.m_baseCpuHandle),
	m_baseGpuHandle(other.m_baseGpuHandle),
	m_numDescriptors(other.m_numDescriptors),
	m_descHeap(other.m_descHeap),
	m_descriptorSize(other.m_descriptorSize),
	m_internal(other.m_internal)
{
	other.m_baseCpuHandle.ptr = 0;
	other.m_baseGpuHandle.ptr = 0;
	other.m_numDescriptors = 0;
	other.m_descHeap = nullptr;
	other.m_descriptorSize = 0;
	other.m_internal = uint32_t(-1);
}

DescriptorTable& DescriptorTable::operator=(DescriptorTable&& other) noexcept
{
	if (this == &other)
		return *this;

	std::swap(m_baseCpuHandle, other.m_baseCpuHandle);
	std::swap(m_baseGpuHandle, other.m_baseGpuHandle);
	std::swap(m_numDescriptors, other.m_numDescriptors);
	std::swap(m_descHeap, other.m_descHeap);
	std::swap(m_descriptorSize, other.m_descriptorSize);
	std::swap(m_internal, other.m_internal);

	return *this;
}

// TODO needs more testing
void DescriptorTable::Reset() noexcept
{
	if (m_baseCpuHandle.ptr)
		m_descHeap->Release(ZetaMove(*this));

	m_baseCpuHandle.ptr = 0;
	m_baseGpuHandle.ptr = 0;
	m_numDescriptors = 0;
	m_descHeap = nullptr;
	m_descriptorSize = 0;
	m_internal = uint32_t(-1);
}

//--------------------------------------------------------------------------------------
// DescriptorHeap
//--------------------------------------------------------------------------------------

DescriptorHeap::DescriptorHeap(uint32_t blockSize) noexcept
	: m_pending(m_memoryPool),
	m_blockSize(blockSize),
	m_numLists((uint32_t)log2f((float)blockSize) + 1),
	m_releasedBlocks(m_memoryPool)
{
	Assert(Math::IsPow2(blockSize), "block size must be a power of two.");

	m_memoryPool.Init();
	uintptr_t currBuffPointer = reinterpret_cast<uintptr_t>(m_headsBuffer);

	for (uint32_t i = 0; i < m_numLists; i++)
	{
		void* ptr = reinterpret_cast<void*>(currBuffPointer);
		new (ptr) Block(m_memoryPool);

		currBuffPointer += sizeof(Block);
	}

	m_heads = reinterpret_cast<Block*>(m_headsBuffer);

	// initialize blocks
	for (uint32_t i = 0; i < MAX_NUM_LISTS; i++)
		m_heads[i].Head = uint32_t(-1);
}

void DescriptorHeap::Init(D3D12_DESCRIPTOR_HEAP_TYPE heapType, uint32_t numDescriptors, bool isShaderVisible) noexcept
{
	Assert(!isShaderVisible || heapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		"Shader-visible heap type must be D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV");

	Assert(!isShaderVisible || numDescriptors <= 1'000'000,
		"GPU resource heap can't contain more than 1'000'000 elements");

	Assert(numDescriptors >= m_blockSize, "invalid #descriptors of %u for block size of %u", numDescriptors, m_blockSize);

	m_totalHeapSize = numDescriptors;
	m_isShaderVisisble = isShaderVisible;

	D3D12_DESCRIPTOR_HEAP_DESC desc;
	desc.Type = heapType;
	desc.NumDescriptors = numDescriptors;
	desc.Flags = isShaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	desc.NodeMask = 0;

	auto* device = App::GetRenderer().GetDevice();
	CheckHR(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_heap.GetAddressOf())));

	m_descriptorSize = device->GetDescriptorHandleIncrementSize(heapType);
	m_baseCPUHandle = m_heap->GetCPUDescriptorHandleForHeapStart();
	m_freeDescCount = numDescriptors;

	if (isShaderVisible)
		m_baseGPUHandle = m_heap->GetGPUDescriptorHandleForHeapStart();
	
	CheckHR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));
}

void DescriptorHeap::Shutdown() noexcept
{
	m_pending.free_memory();
}

bool DescriptorHeap::AllocateNewBlock(uint32_t listIdx) noexcept
{
	Assert(m_heads[listIdx].Entries.empty(), "this linked list must be empty.");

	uint32_t nextHeapIdx = m_nextHeapIdx;
	uint32_t blockSize = m_blockSize;

	if (m_nextHeapIdx >= m_totalHeapSize)
	{
		if(m_releasedBlocks.empty())
			return false;
	
		auto b = m_releasedBlocks.back();
		m_releasedBlocks.pop_back();

		nextHeapIdx = b.Offset;
		blockSize = b.Count;
	}

	const int descTableSize = DescTableSizeFromListIndex(listIdx);
	const int numDescTablesInBlock = blockSize / descTableSize;
	m_heads[listIdx].Entries.resize(numDescTablesInBlock);
	int currEntry = 0;

	for (int i = 0; i < (int)blockSize; i += descTableSize)
	{
		Entry e{ .HeapOffset = nextHeapIdx + i,
			.Next = (currEntry != numDescTablesInBlock - 1) ? (uint32_t)(currEntry + 1) : uint32_t(-1) };

		m_heads[listIdx].Entries[currEntry++] = e;
	}

	// once this index reaches the end, it should stay there
	m_nextHeapIdx = Math::Min(m_nextHeapIdx + m_blockSize, m_totalHeapSize);
	m_heads[listIdx].Head = 0;

	return true;
}

DescriptorTable DescriptorHeap::Allocate(uint32_t count) noexcept
{
	Assert(count && count <= m_totalHeapSize, "invalid allocation count.");
	uint32_t heapOffset;
	uint32_t arrayOffset;

	AcquireSRWLockExclusive(&m_lock);

	if (count > m_blockSize)
	{
		Assert(m_nextHeapIdx + count < m_totalHeapSize, "out of free space in descriptor heap.");

		heapOffset = m_nextHeapIdx;
		arrayOffset = uint32_t(-1);
		m_nextHeapIdx += count;

		m_freeDescCount -= count;
	}
	else
	{
		uint32_t listIdx = ListIndexFromDescTableSize(count);
		Assert(listIdx < m_numLists, "invalid list index.");

		bool success = true;

		// build a new linked list
		if (m_heads[listIdx].Head == uint32_t(-1))
		{
			m_heads[listIdx].Entries.clear();
			success = AllocateNewBlock(listIdx);
		}

		// try to allocate from existing linked lists with a larger block size
		// TODO alternatively, chunks from smaller block sizes can be coalesced together
		if (!success)
		{
			// TODO instead of returning a larger block directly, break it into chunks (with size
			// of each equal to the best fit for this request), insert those chunks into the current
			// (empty) list and then return the head
			while (m_heads[listIdx].Head == uint32_t(-1))
			{
				listIdx++;
				Assert(listIdx < m_numLists, "out of free space in the descriptor heap.");
			}
		}

		// pop from the linked list
		const uint32_t currHeadIdx = m_heads[listIdx].Head;
		Entry e = m_heads[listIdx].Entries[currHeadIdx];

		// set the new head
		const uint32_t nextHeadIdx = e.Next;
		m_heads[listIdx].Entries[currHeadIdx].Next = uint32_t(-1);
		m_heads[listIdx].Head = nextHeadIdx;

		heapOffset = e.HeapOffset;
		arrayOffset = currHeadIdx;
		
		m_freeDescCount -= DescTableSizeFromListIndex(listIdx);
	}

	ReleaseSRWLockExclusive(&m_lock);

	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle{ .ptr = m_baseCPUHandle.ptr + heapOffset * m_descriptorSize };

	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_isShaderVisisble ?
		D3D12_GPU_DESCRIPTOR_HANDLE{ .ptr = m_baseGPUHandle.ptr + heapOffset * m_descriptorSize } :
		D3D12_GPU_DESCRIPTOR_HANDLE{ .ptr = 0 };

	return DescriptorTable(cpuHandle,
		gpuHandle,
		count,
		m_descriptorSize,
		this,
		arrayOffset);
}

void DescriptorHeap::Release(DescriptorTable&& table) noexcept
{
	const uint32_t offset = (uint32_t)((table.m_baseCpuHandle.ptr - m_baseCPUHandle.ptr) / m_descriptorSize);

	AcquireSRWLockExclusive(&m_lock);
	m_pending.emplace_back(m_nextFenceVal, offset, table.m_numDescriptors, table.m_internal);
	ReleaseSRWLockExclusive(&m_lock);
}

void DescriptorHeap::Recycle() noexcept
{
	if (m_pending.empty())
		return;

	// TODO Is it necessary to signal the compute queue?
	if(m_isShaderVisisble)
		App::GetRenderer().SignalDirectQueue(m_fence.Get(), m_nextFenceVal++);

	const uint64_t completedFenceVal = m_fence->GetCompletedValue();

	for (PendingDescTable* currPending = m_pending.begin(); currPending != m_pending.end(); )
	{
		auto [releaseFence, offset, numDescs, internalVal] = *currPending;

		Assert(offset < m_totalHeapSize, "invalid offset");
		Assert(numDescs < m_totalHeapSize, "invalid #descs");

		// not safe to release just yet
		if (m_isShaderVisisble && completedFenceVal < releaseFence)
		{
			currPending++;
			continue;
		}

		if (numDescs <= m_blockSize)
		{
			const int listIdx = ListIndexFromDescTableSize(numDescs);
			const uint32_t currHead = m_heads[listIdx].Head;

			Entry e{ .HeapOffset = offset, .Next = currHead };

			// When a single Entry ping pongs between allocation and release and there hasn't been
			// any other allocations/releases in the meantime, the corresponding Entries array continues 
			// to grow indefinitely. To avoid that, attempt to reuse the previous array position instead
			// of appending to the end. Note that when a new block is added, SmallVector is cleared first 
			// and unbounded growth is avoided.
			if (internalVal != uint32_t(-1) && internalVal < m_heads[listIdx].Entries.size() &&
				m_heads[listIdx].Entries[internalVal].HeapOffset == offset)
			{
				Assert(m_heads[listIdx].Entries[internalVal].Next == uint32_t(-1), "these must match.");
			
				m_heads[listIdx].Entries[internalVal] = e;
				m_heads[listIdx].Head = internalVal;
			}
			else
			{
				m_heads[listIdx].Head = (uint32_t)m_heads[listIdx].Entries.size();
				m_heads[listIdx].Entries.push_back(e);
			}

			m_freeDescCount += DescTableSizeFromListIndex(listIdx);
		}
		else
		{
			ReleasedLargeBlock b{ .Offset = offset, .Count = numDescs };
			m_releasedBlocks.push_back(b);
		}

		currPending = m_pending.erase(*currPending);
	}
}


