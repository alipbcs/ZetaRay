#pragma once

#include "../Utility/SmallVector.h"
#include "Device.h"

namespace ZetaRay::Core
{
    struct DescriptorTable;

    struct DescriptorHeap
    {
        DescriptorHeap(uint32_t BlockSize = MAX_BLOCK_SIZE);
        ~DescriptorHeap() = default;

        DescriptorHeap(const DescriptorHeap&) = delete;
        DescriptorHeap& operator=(const DescriptorHeap&) = delete;

        void Init(D3D12_DESCRIPTOR_HEAP_TYPE heapType, uint32_t numDescriptors, bool isShaderVisible);
        DescriptorTable Allocate(uint32_t count);
        void Release(DescriptorTable&& descTable);
        void Recycle();

        ZetaInline bool IsShaderVisible() const { return m_isShaderVisible; }
        ZetaInline uint32_t GetDescriptorSize() const { return m_descriptorSize; }
        ZetaInline uint32_t GetNumFreeDescriptors() const { return m_freeDescCount; }
        ZetaInline uint64_t GetBaseGpuHandle() const { return m_baseGPUHandle.ptr; }
        ZetaInline ID3D12DescriptorHeap* GetHeap() { return m_heap.Get(); }
        ZetaInline uint32_t GetHeapSize() { return m_totalHeapSize; }

    private:
        static const uint32_t MAX_BLOCK_SIZE = 1024;
        static const uint32_t MAX_NUM_LISTS = 11;
        static_assert(1 << (MAX_NUM_LISTS - 1) == MAX_BLOCK_SIZE, "these must match.");

        struct PendingDescTable
        {
            PendingDescTable() = default;
            PendingDescTable(uint64_t fence, uint32_t offset, uint32_t count, uint32_t internalVal)
                : ReleaseFence(fence),
                Offset(offset),
                Count(count),
                Internal(internalVal)
            {}

            uint64_t ReleaseFence;
            uint32_t Offset;
            uint32_t Count;
            uint32_t Internal;
        };

        struct Entry
        {
            uint32_t HeapOffset;
            uint32_t Next;
        };

        struct Block
        {
            Block() = default;
            Block(Block&&) = delete;
            Block& operator=(Block&&) = delete;

            uint32_t Head;
            Util::SmallVector<Entry> Entries;
        };

        struct ReleasedLargeBlock
        {
            uint32_t Offset;
            uint32_t Count;
        };

        ZetaInline uint32_t DescTableSizeFromListIndex(uint32_t x) const
        {
            return 1 << x;
        }

        ZetaInline uint32_t ListIndexFromDescTableSize(uint32_t x)
        {
            size_t s = Math::NextPow2(x);
            unsigned long idx;
            _BitScanForward64(&idx, s);

            return idx;
        }

        bool AllocateNewBlock(uint32_t listIdx);

        SRWLOCK m_lock = SRWLOCK_INIT;

        ComPtr<ID3D12DescriptorHeap> m_heap;
        D3D12_CPU_DESCRIPTOR_HANDLE m_baseCPUHandle;
        D3D12_GPU_DESCRIPTOR_HANDLE m_baseGPUHandle;
        bool m_isShaderVisible;
        ComPtr<ID3D12Fence> m_fence;
        uint64_t m_nextFenceVal = 1;
        uint32_t m_descriptorSize = 0;
        uint32_t m_totalHeapSize = 0;
        uint32_t m_freeDescCount = 0;

        Util::SmallVector<PendingDescTable> m_pending;

        // Segregated free lists
        const uint32_t m_blockSize;
        Block m_heads[MAX_NUM_LISTS];
#ifndef NDEBUG
        const uint32_t m_numLists;
#endif

        uint32_t m_nextHeapIdx = 0;
        Util::SmallVector<ReleasedLargeBlock> m_releasedBlocks;
    };

    // A contiguous range of descriptors that are allocated from one DescriptorHeap
    struct DescriptorTable
    {
        friend struct DescriptorHeap;

        DescriptorTable() = default;
        DescriptorTable(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle,
            uint32_t numDesc,
            uint32_t descSize,
            DescriptorHeap* heap,
            uint32_t internal);
        ~DescriptorTable();

        DescriptorTable(DescriptorTable&& other);
        DescriptorTable& operator=(DescriptorTable&& other);

        void Reset();
        ZetaInline bool IsEmpty() const { return m_numDescriptors == 0; }

        ZetaInline D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle(uint32_t offset) const
        {
            Assert(offset < m_numDescriptors, "Descriptor offset is out of bounds");
            return D3D12_CPU_DESCRIPTOR_HANDLE{ .ptr = m_baseCpuHandle.ptr + offset * m_descriptorSize };
        }

        ZetaInline D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle(uint32_t offset) const
        {
            Assert(offset < m_numDescriptors, "Descriptor offset is out of bounds");
            Assert(m_descHeap->IsShaderVisible(), "This descriptor doesn't belong to a shader-visible heap.");
            return D3D12_GPU_DESCRIPTOR_HANDLE{ .ptr = m_baseGpuHandle.ptr + offset * m_descriptorSize };
        }

        ZetaInline uint32_t GetNumDescriptors() const { return m_numDescriptors; };

        // Offset to the beginning of this desc. table in the GPU descriptor heap
        ZetaInline uint32_t GPUDescriptorHeapIndex(uint32_t offset = 0) const
        {
            Assert(m_descHeap->IsShaderVisible(), "Descriptor table is not shader-visible.");
            Assert(offset < m_numDescriptors, "Descriptor offset is out of bounds");
            uint32_t idx = (uint32_t)((m_baseGpuHandle.ptr - m_descHeap->GetBaseGpuHandle()) / m_descriptorSize);

            return idx + offset;
        }

    private:
        DescriptorHeap* m_descHeap = nullptr;    // DescriptorHeap from which this table was allocated

        D3D12_CPU_DESCRIPTOR_HANDLE m_baseCpuHandle = { 0 };
        D3D12_GPU_DESCRIPTOR_HANDLE m_baseGpuHandle = { 0 };

        uint32_t m_numDescriptors = 0;
        uint32_t m_descriptorSize = 0;
        uint32_t m_internal = UINT32_MAX;
    };
}
