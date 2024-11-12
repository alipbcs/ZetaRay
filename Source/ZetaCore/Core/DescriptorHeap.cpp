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
    uint32_t internalVal)
    : m_descHeap(heap),
    m_baseCpuHandle(cpuHandle),
    m_baseGpuHandle(gpuHandle),
    m_numDescriptors(numDesc),
    m_descriptorSize(descSize),
    m_internal(internalVal)
{}

DescriptorTable::~DescriptorTable()
{
    Reset();
}

DescriptorTable::DescriptorTable(DescriptorTable&& other)
    : m_descHeap(other.m_descHeap),
    m_baseCpuHandle(other.m_baseCpuHandle),
    m_baseGpuHandle(other.m_baseGpuHandle),
    m_numDescriptors(other.m_numDescriptors),
    m_descriptorSize(other.m_descriptorSize),
    m_internal(other.m_internal)
{
    other.m_descHeap = nullptr;
    other.m_baseCpuHandle.ptr = 0;
    other.m_baseGpuHandle.ptr = 0;
    other.m_numDescriptors = 0;
    other.m_descriptorSize = 0;
    other.m_internal = UINT32_MAX;
}

DescriptorTable& DescriptorTable::operator=(DescriptorTable&& other)
{
    if (this == &other)
        return *this;

    std::swap(m_descHeap, other.m_descHeap);
    std::swap(m_baseCpuHandle, other.m_baseCpuHandle);
    std::swap(m_baseGpuHandle, other.m_baseGpuHandle);
    std::swap(m_numDescriptors, other.m_numDescriptors);
    std::swap(m_descriptorSize, other.m_descriptorSize);
    std::swap(m_internal, other.m_internal);

    return *this;
}

// TODO needs more testing
void DescriptorTable::Reset()
{
    if (m_baseCpuHandle.ptr)
        m_descHeap->Release(ZetaMove(*this));

    m_descHeap = nullptr;
    m_baseCpuHandle.ptr = 0;
    m_baseGpuHandle.ptr = 0;
    m_numDescriptors = 0;
    m_descriptorSize = 0;
    m_internal = UINT32_MAX;
}

//--------------------------------------------------------------------------------------
// DescriptorHeap
//--------------------------------------------------------------------------------------

DescriptorHeap::DescriptorHeap(uint32_t blockSize)
#ifndef NDEBUG
    : m_blockSize(blockSize),
    m_numLists((uint32_t)log2f((float)blockSize) + 1)
#else
    : m_blockSize(blockSize)
#endif
{
    Assert(Math::IsPow2(blockSize), "Block size must be a power of two.");

    // Initialize blocks
    for (uint32_t i = 0; i < MAX_NUM_LISTS; i++)
        m_heads[i].Head = UINT32_MAX;
}

void DescriptorHeap::Init(D3D12_DESCRIPTOR_HEAP_TYPE heapType, uint32_t numDescriptors, 
    bool isShaderVisible)
{
    Assert(!isShaderVisible || heapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        "Shader-visible heap type must be D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV.");
    Assert(!isShaderVisible || numDescriptors <= 1'000'000,
        "GPU resource heap can't contain more than 1'000'000 elements");
    Assert(numDescriptors >= m_blockSize, "#descriptors=%u is invalid for block size of %u.", 
        numDescriptors, m_blockSize);

    m_totalHeapSize = numDescriptors;
    m_isShaderVisible = isShaderVisible;

    D3D12_DESCRIPTOR_HEAP_DESC desc;
    desc.Type = heapType;
    desc.NumDescriptors = numDescriptors;
    desc.Flags = isShaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : 
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
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

bool DescriptorHeap::AllocateNewBlock(uint32_t listIdx)
{
    Assert(m_heads[listIdx].Entries.empty(), "This linked list must be empty.");

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
            .Next = (currEntry != numDescTablesInBlock - 1) ? 
            (uint32_t)(currEntry + 1) : UINT32_MAX };

        m_heads[listIdx].Entries[currEntry++] = e;
    }

    // Once this index reaches the end, it should stay there
    m_nextHeapIdx = Math::Min(m_nextHeapIdx + m_blockSize, m_totalHeapSize);
    m_heads[listIdx].Head = 0;

    return true;
}

DescriptorTable DescriptorHeap::Allocate(uint32_t count)
{
    Assert(count && count <= m_totalHeapSize, "Invalid allocation count.");
    uint32_t heapOffset;
    uint32_t arrayOffset;

    AcquireSRWLockExclusive(&m_lock);

    if (count > m_blockSize)
    {
        Assert(m_nextHeapIdx + count < m_totalHeapSize, "Out of free space in descriptor heap.");

        heapOffset = m_nextHeapIdx;
        arrayOffset = UINT32_MAX;
        m_nextHeapIdx += count;

        m_freeDescCount -= count;
    }
    else
    {
        uint32_t listIdx = ListIndexFromDescTableSize(count);
        Assert(listIdx < m_numLists, "Unvalid list index.");

        bool success = true;

        // build a new linked list
        if (m_heads[listIdx].Head == UINT32_MAX)
        {
            m_heads[listIdx].Entries.clear();
            success = AllocateNewBlock(listIdx);
        }

        // Try to allocate from existing linked lists with a larger block size
        // TODO alternatively, chunks from smaller block sizes can be coalesced together
        if (!success)
        {
            // TODO instead of returning a larger block directly, break it into chunks (with size
            // of each equal to the best fit for this request), insert those chunks into the current
            // (empty) list and then return the head
            while (m_heads[listIdx].Head == UINT32_MAX)
            {
                listIdx++;
                Assert(listIdx < m_numLists, "Out of free space in the descriptor heap.");
            }
        }

        // Pop from the linked list
        const uint32_t currHeadIdx = m_heads[listIdx].Head;
        Entry e = m_heads[listIdx].Entries[currHeadIdx];

        // Set the new head
        const uint32_t nextHeadIdx = e.Next;
        m_heads[listIdx].Entries[currHeadIdx].Next = UINT32_MAX;
        m_heads[listIdx].Head = nextHeadIdx;

        heapOffset = e.HeapOffset;
        arrayOffset = currHeadIdx;

        m_freeDescCount -= DescTableSizeFromListIndex(listIdx);
    }

    ReleaseSRWLockExclusive(&m_lock);

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle{ .ptr = 
        m_baseCPUHandle.ptr + heapOffset * m_descriptorSize };

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_isShaderVisible ?
        D3D12_GPU_DESCRIPTOR_HANDLE{ .ptr = m_baseGPUHandle.ptr + heapOffset * m_descriptorSize } :
        D3D12_GPU_DESCRIPTOR_HANDLE{ .ptr = 0 };

    return DescriptorTable(cpuHandle,
        gpuHandle,
        count,
        m_descriptorSize,
        this,
        arrayOffset);
}

void DescriptorHeap::Release(DescriptorTable&& table)
{
    const uint32_t offset = 
        (uint32_t)((table.m_baseCpuHandle.ptr - m_baseCPUHandle.ptr) / m_descriptorSize);

    AcquireSRWLockExclusive(&m_lock);
    m_pending.emplace_back(m_nextFenceVal, offset, table.m_numDescriptors, table.m_internal);
    ReleaseSRWLockExclusive(&m_lock);
}

void DescriptorHeap::Recycle()
{
    if (m_pending.empty())
        return;

    // TODO Is it necessary to signal the compute queue?
    if(m_isShaderVisible)
        App::GetRenderer().SignalDirectQueue(m_fence.Get(), m_nextFenceVal++);

    const uint64_t completedFenceVal = m_fence->GetCompletedValue();

    for (PendingDescTable* currPending = m_pending.begin(); currPending != m_pending.end(); )
    {
        auto [releaseFence, offset, numDescs, internalVal] = *currPending;

        Assert(offset < m_totalHeapSize, "invalid offset");
        Assert(numDescs < m_totalHeapSize, "invalid #descs");

        // Not safe to release just yet
        if (m_isShaderVisible && completedFenceVal < releaseFence)
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
            if (internalVal != UINT32_MAX && internalVal < m_heads[listIdx].Entries.size() &&
                m_heads[listIdx].Entries[internalVal].HeapOffset == offset)
            {
                Assert(m_heads[listIdx].Entries[internalVal].Next == UINT32_MAX, 
                    "These must match.");

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
