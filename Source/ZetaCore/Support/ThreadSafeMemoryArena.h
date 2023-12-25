#pragma once

#include "../Utility/SmallVector.h"
#include <atomic>
#include "../Win32/Win32.h"

namespace ZetaRay::Support
{
    struct ThreadSafeMemoryArena
    {
        ThreadSafeMemoryArena(size_t blockSize = 64 * 1024, int initNumBlocks = ZETA_MAX_NUM_THREADS);
        ~ThreadSafeMemoryArena() = default;

        ThreadSafeMemoryArena(ThreadSafeMemoryArena&&) = delete;
        ThreadSafeMemoryArena& operator=(ThreadSafeMemoryArena&&) = delete;

        void* AllocateAligned(size_t size, size_t alignment);
        void FreeAligned(void* mem, size_t size, size_t alignment) {}

    private:
        struct MemoryBlock
        {
            MemoryBlock()
                : Start(nullptr),
                Offset(0),
                Size(0)
            {}

            ~MemoryBlock()
            {
                Offset = 0;
                if (Start)
                {
                    free(Start);
                    Start = nullptr;
                }
            }

            MemoryBlock(MemoryBlock&& rhs)
                : Start(rhs.Start),
                Offset(rhs.Offset),
                Size(rhs.Size)
            {
                rhs.Start = nullptr;
                rhs.Offset = 0;
            }

            MemoryBlock& operator=(MemoryBlock&& rhs)
            {
                Start = rhs.Start;
                Offset = rhs.Offset;
                Size = rhs.Size;
                rhs.Start = nullptr;
                rhs.Offset = 0;
                rhs.Size = 0;

                return *this;
            }

            void* Start;
            uintptr_t Offset;
            size_t Size;
        };

        ZetaInline size_t NumMemoryBlocks()
        {
            return m_blocks.size();
        }

        size_t TotalSizeInBytes();

        const size_t k_defaultBlockSize;
        Util::SmallVector<MemoryBlock, Support::SystemAllocator, ZETA_MAX_NUM_THREADS> m_blocks;
        SRWLOCK m_blocksLock;
        
        uint32_t m_threadIDs[ZETA_MAX_NUM_THREADS] = { 0 };
        int m_threadCurrBlockIdx[ZETA_MAX_NUM_THREADS];
        int m_numThreads;
        std::atomic_int32_t m_currBlockIdx;
    };


    struct ThreadSafeArenaAllocator
    {
        ThreadSafeArenaAllocator(ThreadSafeMemoryArena& ma)
            : m_allocator(&ma)
        {}

        ThreadSafeArenaAllocator(const ThreadSafeArenaAllocator& other)
            : m_allocator(other.m_allocator)
        {}

        ThreadSafeArenaAllocator& operator=(const ThreadSafeArenaAllocator& other)
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
        ThreadSafeMemoryArena* m_allocator;
    };
}