#pragma once

#include "../Utility/SmallVector.h"

namespace ZetaRay::Support
{
    class MemoryArena
    {
    public:
        explicit MemoryArena(size_t blockSize = 64 * 1024);
        ~MemoryArena() = default;

        MemoryArena(MemoryArena&&);
        MemoryArena& operator=(MemoryArena&&);

        void* AllocateAligned(size_t size, size_t alignment = alignof(std::max_align_t));
        void FreeAligned(void* pMem, size_t size, size_t alignment = alignof(std::max_align_t)) {};
        size_t TotalSize() const;
        void Reset();

    private:
        struct MemoryBlock
        {
            MemoryBlock() = default;
            explicit MemoryBlock(size_t size)
            {
                Start = malloc(size);
                Offset = 0;
                Size = size;
            }
            ~MemoryBlock()
            {
                if (Start)
                    free(Start);

                Start = nullptr;
                Offset = 0;
                Size = 0;
            }

            MemoryBlock(MemoryBlock&& rhs)
                : Start(rhs.Start),
                Offset(rhs.Offset),
                Size(rhs.Size)
            {
                rhs.Start = nullptr;
                rhs.Offset = 0;
                rhs.Size = 0;
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

        const size_t m_blockSize;
        Util::SmallVector<MemoryBlock, SystemAllocator, 8> m_blocks;

#ifndef NDEBUG
        uint32_t m_numAllocs = 0;
#endif
    };

    struct ArenaAllocator
    {
        ArenaAllocator(MemoryArena& ma)
            : m_allocator(&ma)
        {}

        ArenaAllocator(const ArenaAllocator& other)
            : m_allocator(other.m_allocator)
        {
        }

        ArenaAllocator& operator=(const ArenaAllocator& other)
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
        MemoryArena* m_allocator;
    };
}
