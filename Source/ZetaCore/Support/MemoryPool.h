#pragma once

#include "Memory.h"

namespace ZetaRay::Support
{
    //    Pool-based memory allocator
    //     - A collection of pools where each pool is made up of chunks of the same size
    //     - Starting from a chunk size of 8 bytes, pool i has chuck size 2^(i + 3).
    //     - Total number of chunks in each pool can be configured in the constructor and is
    //       held in member "m_numChunks".
    //     - There are a total of "m_poolCount" pools.
    //
    //     - Visualization:
    //
    //        pool 0:
    //            8 bytes - 8 bytes - 8 bytes - .. - 8 bytes
    //            ------------- m_numChunks ----------------
    //        pool 1:
    //            16 bytes - 16 bytes - 16 bytes - .. - 16 bytes
    //            ------------- m_numChunks ----------------
    //        pool 2:
    //            32 bytes - 32 bytes - 32 bytes - .. - 32 bytes
    //            ------------- m_numChunks ----------------
    //
    //                            ....
    class MemoryPool
    {
    public:
        MemoryPool() = default;
        ~MemoryPool();

        MemoryPool(MemoryPool&&) = delete;
        MemoryPool& operator=(MemoryPool&&) = delete;

        // Initialize the memory pool. Has to be called before any allocation/deallocation can take place
        void Init();
        void Clear();

        void* AllocateAligned(size_t size, size_t alignment = alignof(std::max_align_t));
        void FreeAligned(void* pMem, size_t size, size_t alignment = alignof(std::max_align_t));

        size_t TotalSize() const;

        void MoveTo(MemoryPool& mp);

    private:
        void* Allocate(size_t size);
        void Free(void* pMem, size_t size);

        // Given x, returns:
        //        0    -> 8 bytes allocator    when 0 < x <= 8
        //        1    -> 16 bytes allocator    when 8 < x <= 16
        //        2    -> 32 bytes allocator    when 16 < x <= 32
        //        3    -> 64 bytes allocator    when 32 < x <= 64
        //            ...
        size_t GetPoolIndexFromSize(size_t x);

        // Chunk size for given pool index
        ZetaInline size_t GetChunkSizeFromPoolIndex(size_t x) const
        {
            return 1llu << (x + INDEX_SHIFT);
        }

        // Allocates a new memory block and turns it into a linked list
        void* AllocateNewBlock(size_t chunkSize);

        // Adds a new memory block
        void Grow(size_t poolIndex);

        static constexpr size_t BLOCK_SIZE = 4096;                    
        static constexpr size_t MAX_ALLOC_SIZE = BLOCK_SIZE;        // Allocation up to 4 kb supported    
        static constexpr size_t POOL_COUNT = 10;                    // Number of pools == log_2 (4096) - log_2 (8) + 1
        static constexpr size_t INDEX_SHIFT = 3;                    // First block starts at 8 bytes (log_2(sizeof(void *))
        static constexpr size_t MIN_ALLOC_SIZE = 1 << INDEX_SHIFT;        

        // Holds the pointer to head of memory blocks allocated for each pool size
        void** m_pools[POOL_COUNT] = { nullptr };

        // Keep track of how many memory blocks are there per block size
        size_t m_numMemoryBlocks[POOL_COUNT] = { 0 };

        // Pointer to head of the linked list for each memory block
        void* m_currHead[POOL_COUNT] = { nullptr };
    };

    struct PoolAllocator
    {
        PoolAllocator(MemoryPool& mp)
            : m_allocator(&mp)
        {}

        PoolAllocator(const PoolAllocator& other)
            : m_allocator(other.m_allocator)
        {
        }

        PoolAllocator& operator=(const PoolAllocator& other)
        {
            m_allocator = other.m_allocator;
            return *this;
        }

        ZetaInline void* AllocateAligned(size_t size, size_t alignment = alignof(std::max_align_t))
        {
            return m_allocator->AllocateAligned(size, alignment);
        }

        ZetaInline void FreeAligned(void* mem, size_t size, size_t alignment = alignof(std::max_align_t))
        {
            m_allocator->FreeAligned(mem, size, alignment);
        }

    private:
        MemoryPool* m_allocator;
    };
}