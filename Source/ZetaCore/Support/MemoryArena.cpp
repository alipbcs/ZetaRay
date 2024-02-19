#include "MemoryArena.h"

using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// MemoryArena
//--------------------------------------------------------------------------------------

MemoryArena::MemoryArena(size_t blockSize)
    : m_blockSize(blockSize)
{
}

MemoryArena::MemoryArena(MemoryArena&& other)
    : m_blockSize(other.m_blockSize)
{
    m_blocks.swap(other.m_blocks);

#ifdef _DEBUG
    m_numAllocs = other.m_numAllocs;
#endif // _DEBUG}
}

MemoryArena& MemoryArena::operator=(MemoryArena&& other)
{
    Check(m_blockSize == other.m_blockSize, "these MemoryArenas are incompatible.");

    m_blocks.swap(other.m_blocks);
    other.m_blocks.free_memory();

#ifdef _DEBUG
    m_numAllocs = other.m_numAllocs;
    other.m_numAllocs = 0;
#endif // _DEBUG}

    return *this;
}

void* MemoryArena::AllocateAligned(size_t size, size_t alignment)
{
    for (auto& block : m_blocks)
    {
        const uintptr_t start = reinterpret_cast<uintptr_t>(block.Start);
        const uintptr_t ret = Math::AlignUp(start + block.Offset, alignment);
        const uintptr_t startOffset = ret - start;

        if (startOffset + size < block.Size)
        {
            block.Offset = startOffset + size;

#ifdef _DEBUG
            m_numAllocs++;
#endif

            return reinterpret_cast<void*>(ret);
        }
    }

    size_t blockSize = Math::Max(m_blockSize, size);

    // Memory allocationss are 16-byte aligned by default -- for larger alignments, at
    // most alignment - 1 extra bytes are required
    if (alignment > 16)
        blockSize = Math::AlignUp(blockSize + alignment - 1, alignment);

    MemoryBlock memBlock(blockSize);

    const uintptr_t ret = Math::AlignUp(reinterpret_cast<uintptr_t>(memBlock.Start), alignment);
    memBlock.Offset = ret - reinterpret_cast<uintptr_t>(memBlock.Start);
    memBlock.Offset += size;
    Assert(memBlock.Offset <= memBlock.Size, "offset must be <= size");

    // Push the newly added block to the front, so it's searched before others for future allocations
    m_blocks.push_front(ZetaMove(memBlock));

    return reinterpret_cast<void*>(ret);
}

size_t MemoryArena::TotalSize() const
{
    size_t sum = 0;

    for (auto& block : m_blocks)
        sum += block.Size;

    return sum;
}

void MemoryArena::Reset()
{
    while (m_blocks.size() > 1)
        m_blocks.pop_back();

    if (!m_blocks.empty())
        m_blocks[0].Offset = 0;
}

