#include "OffsetAllocator.h"
#include "../Utility/Error.h"
#include "../Math/Common.h"
#include <intrin.h>

using namespace ZetaRay::Support;

// Ref: https://github.com/sebbbi/OffsetAllocator/blob/main/offsetAllocator.cpp
namespace SmallFloat
{
    static constexpr uint32_t MANTISSA_BITS = 3;
    static constexpr uint32_t MANTISSA_VALUE = 1 << MANTISSA_BITS;
    static constexpr uint32_t MANTISSA_MASK = MANTISSA_VALUE - 1;

    // Bin sizes follow floating point (exponent + mantissa) distribution (piecewise linear log approx)
    // This ensures that for each size class, the average overhead percentage stays the same
    uint32_t uintToFloatRoundUp(uint32_t size)
    {
        Assert(size > 0, "invalid arg.");

        uint32_t exp = 0;
        uint32_t mantissa = 0;

        if (size < MANTISSA_VALUE)
        {
            // Denorm: 0..(MANTISSA_VALUE-1)
            mantissa = size;
        }
        else
        {
            // Normalized: Hidden high bit always 1. Not stored. Just like float.
            const uint32_t leadingZeros = _lzcnt_u32(size);
            const uint32_t highestSetBit = 31 - leadingZeros;

            const uint32_t mantissaStartBit = highestSetBit - MANTISSA_BITS;
            exp = mantissaStartBit + 1;
            mantissa = (size >> mantissaStartBit) & MANTISSA_MASK;

            const uint32_t lowBitsMask = (1 << mantissaStartBit) - 1;

            // Round up!
            if ((size & lowBitsMask) != 0)
                mantissa++;
        }

        return (exp << MANTISSA_BITS) + mantissa; // + allows mantissa->exp overflow for round up
    }

    uint32_t uintToFloatRoundDown(uint32_t size)
    {
        Assert(size > 0, "invalid arg.");

        uint32_t exp = 0;
        uint32_t mantissa = 0;

        if (size < MANTISSA_VALUE)
        {
            // Denorm: 0..(MANTISSA_VALUE-1)
            mantissa = size;
        }
        else
        {
            // Normalized: Hidden high bit always 1. Not stored. Just like float.
            const uint32_t leadingZeros = _lzcnt_u32(size);
            const uint32_t highestSetBit = 31 - leadingZeros;

            const uint32_t mantissaStartBit = highestSetBit - MANTISSA_BITS;
            exp = mantissaStartBit + 1;
            mantissa = (size >> mantissaStartBit) & MANTISSA_MASK;
        }

        return (exp << MANTISSA_BITS) | mantissa;
    }

    uint32_t floatToUint(uint32_t floatValue)
    {
        const uint32_t exponent = floatValue >> MANTISSA_BITS;
        const uint32_t mantissa = floatValue & MANTISSA_MASK;
        
        if (exponent == 0)
        {
            // Denorms
            return mantissa;
        }
        else
            return (mantissa | MANTISSA_VALUE) << (exponent - 1);
    }
}

namespace
{
    ZetaInline uint32_t LowestSetBitGeIndex(uint32_t mask, uint32_t idx)
    {
        const uint32_t geIdxMask = ~((1 << idx) - 1);
        mask &= geIdxMask;
        return mask > 0 ? _tzcnt_u32(mask & geIdxMask) : OffsetAllocator::INVALID_INDEX;
    }
}

//--------------------------------------------------------------------------------------
// OffsetAllocator
//--------------------------------------------------------------------------------------

OffsetAllocator::OffsetAllocator(uint32_t size, uint32_t maxNumAllocs)
{
    Init(size, maxNumAllocs);
}

OffsetAllocator::~OffsetAllocator()
{
    if(m_nodes)
        delete[] m_nodes;

    if(m_nodeStack)
        delete[] m_nodeStack;
}

OffsetAllocator::OffsetAllocator(OffsetAllocator&& rhs)
    : m_size(rhs.m_size),
    m_maxNumAllocs(rhs.m_maxNumAllocs),
    m_freeStorage(rhs.m_freeStorage),
    m_firstLevelMask(rhs.m_firstLevelMask),
    m_nodes(rhs.m_nodes),
    m_nodeStack(rhs.m_nodeStack),
    m_stackTop(rhs.m_stackTop)
{
    rhs.m_nodes = nullptr;
    rhs.m_nodeStack = nullptr;
    rhs.m_freeStorage = 0;
    rhs.m_firstLevelMask = 0;

    memcpy(m_freeListsHeads, rhs.m_freeListsHeads, ZetaArrayLen(m_freeListsHeads) * sizeof(uint32_t));
    memcpy(m_secondLevelMask, rhs.m_secondLevelMask, ZetaArrayLen(m_secondLevelMask) * sizeof(uint8_t));
}

OffsetAllocator& OffsetAllocator::operator=(OffsetAllocator&& rhs)
{
    Assert(m_size == rhs.m_size, "invalid assignment - size can't change after construction.");
    Assert(m_maxNumAllocs == rhs.m_maxNumAllocs, "invalid assignment - max num allocs can't change after construction.");

    m_freeStorage = rhs.m_freeStorage;
    m_firstLevelMask = rhs.m_firstLevelMask;
    m_nodes = rhs.m_nodes;
    m_nodeStack = rhs.m_nodeStack;
    m_stackTop = rhs.m_stackTop;

    rhs.m_nodes = nullptr;
    rhs.m_nodeStack = nullptr;
    rhs.m_freeStorage = 0;
    rhs.m_firstLevelMask = 0;

    memcpy(m_freeListsHeads, rhs.m_freeListsHeads, ZetaArrayLen(m_freeListsHeads) * sizeof(uint32_t));
    memcpy(m_secondLevelMask, rhs.m_secondLevelMask, ZetaArrayLen(m_secondLevelMask) * sizeof(uint8_t));

    return *this;
}

void OffsetAllocator::Init(uint32_t size, uint32_t maxNumAllocs)
{
    m_size = size;
    // + 1 so that the first stack entry (whole memory region) doesn't count towards maximum
    m_maxNumAllocs = maxNumAllocs + 1;

    Assert(size >= 1 && maxNumAllocs >= 1 && maxNumAllocs <= size, "invalid args.");
    Reset();
}

void OffsetAllocator::Reset()
{
	Assert((m_nodes && m_nodeStack) || (!m_nodes && !m_nodeStack), "either both are allocated or both are null.");

	if (m_nodes)
	{
		delete[] m_nodes;
		delete[] m_nodeStack;
	}

	m_nodes = new Node[m_maxNumAllocs];
	m_nodeStack = new uint32_t[m_maxNumAllocs];

	for (uint32_t i = 0; i < m_maxNumAllocs; i++)
		m_nodeStack[i] = m_maxNumAllocs - 1 - i;

	m_firstLevelMask = 0;

	for (int i = 0; i < ZetaArrayLen(m_secondLevelMask); i++)
		m_secondLevelMask[i] = 0;

    for (int i = 0; i < ZetaArrayLen(m_freeListsHeads); i++)
        m_freeListsHeads[i] = INVALID_NODE;

    m_stackTop = m_maxNumAllocs - 1;
    m_freeStorage = 0;

    InsertNode(0, m_size);
}

uint32_t OffsetAllocator::InsertNode(uint32_t offset, uint32_t size)
{
    Assert(offset + size <= m_size, "requested node exceeded memory region bounds.");
    Assert(m_stackTop >= 0, "out of stack storage, InsertNode shouldn't have been called.");

    const uint32_t listIdx = SmallFloat::uintToFloatRoundDown(size);
    const uint32_t currHead = m_freeListsHeads[listIdx];
    const uint32_t nodeIdx = m_nodeStack[m_stackTop--];
    Assert(nodeIdx >= 0 && nodeIdx < m_maxNumAllocs, "out-of-bound access.");

    Node& node = m_nodes[nodeIdx];
    node = Node{ .Offset = offset, 
        .Size = size, 
        .Next = currHead,
        .LeftNeighbor = node.LeftNeighbor,
        .RightNeighbor = node.RightNeighbor };

    if (currHead != INVALID_NODE)
        m_nodes[currHead].Prev = nodeIdx;

    m_freeListsHeads[listIdx] = nodeIdx;

    const uint32_t firstLevelIdx = listIdx >> FIRST_LEVEL_INDEX_SHIFT;
    const uint32_t secondLevelIdx = listIdx & SECOND_LEVEL_INDEX_MASK;

    m_firstLevelMask |= 1 << firstLevelIdx;
    m_secondLevelMask[firstLevelIdx] |= 1 << secondLevelIdx;

    m_freeStorage += size;
    Assert(m_freeStorage <= m_size, "free storage exceeded maximum.");

    return nodeIdx;
}

void OffsetAllocator::RemoveNode(uint32_t nodeIdx)
{
    Node& node = m_nodes[nodeIdx];

    if (node.Prev != INVALID_NODE)
    {
        m_nodes[node.Prev].Next = node.Next;

        if (node.Next != INVALID_NODE)
            m_nodes[node.Next].Prev = node.Prev;
    }
    else
    {
        const uint32_t listIdx = SmallFloat::uintToFloatRoundDown(node.Size);
        m_freeListsHeads[listIdx] = node.Next;

        if (node.Next != INVALID_NODE)
            m_nodes[node.Next].Prev = INVALID_NODE;
        else
        {
            const uint32_t firstLevelIdx = listIdx >> FIRST_LEVEL_INDEX_SHIFT;
            const uint32_t secondLevelIdx = listIdx & SECOND_LEVEL_INDEX_MASK;

            m_secondLevelMask[firstLevelIdx] ^= (1 << secondLevelIdx);

            if (m_secondLevelMask[firstLevelIdx] == 0)
                m_firstLevelMask ^= (1 << firstLevelIdx);
        }
    }

    m_nodeStack[++m_stackTop] = nodeIdx;
    m_freeStorage -= node.Size;
}

OffsetAllocator::Allocation OffsetAllocator::Allocate(uint32_t size, uint32_t alignment)
{
    Assert(alignment >= 1, "invalid alignment.");
    Assert(size >= 1, "redundant call.");

    if (size == 0)
        return Allocation::Empty();

    // out of node storage
    if (m_stackTop == INVALID_NODE)
        return Allocation::Empty();

    // assuming start offset is aligned, at most alignment - 1 extra bytes are required
    const uint32_t alignedSize = size + alignment - 1;

    uint32_t listIdx = SmallFloat::uintToFloatRoundUp(alignedSize);
    uint32_t firstLevelIdx = listIdx >> FIRST_LEVEL_INDEX_SHIFT;
    uint32_t secondLevelIdx = listIdx & SECOND_LEVEL_INDEX_MASK;

    secondLevelIdx = LowestSetBitGeIndex(m_secondLevelMask[firstLevelIdx], secondLevelIdx);
    firstLevelIdx = secondLevelIdx != INVALID_INDEX ? 
        firstLevelIdx : 
        LowestSetBitGeIndex(m_firstLevelMask, firstLevelIdx + 1);

    // out of space
    if (firstLevelIdx == INVALID_INDEX)
        return Allocation::Empty();

    Assert(m_firstLevelMask & (1 << firstLevelIdx), "first/second level mask mismatch.");

    secondLevelIdx = secondLevelIdx != INVALID_INDEX ?
        secondLevelIdx :
        _tzcnt_u32(m_secondLevelMask[firstLevelIdx]);

    listIdx = (firstLevelIdx << FIRST_LEVEL_INDEX_SHIFT) + secondLevelIdx;
    Assert(m_freeListsHeads[listIdx] != INVALID_NODE, "list/mask mismatch.");
    const uint32_t nodeIdx = m_freeListsHeads[listIdx];

    Node& head = m_nodes[nodeIdx];
    Assert(!head.InUse, "a freelist node shoudn't be in use.");

    const uint32_t oldSize = head.Size;
    const uint32_t oldRightNeighbor = head.RightNeighbor;
    // due to rounding up, oldSize > alignedSize
    const uint32_t leftoverSize = oldSize - alignedSize;

    // pop head node from list
    m_freeListsHeads[listIdx] = head.Next;

    if (head.Next != INVALID_NODE)
        m_nodes[head.Next].Prev = INVALID_NODE;

    head = Node{ .Offset = head.Offset,
        .Size = alignedSize, 
        .LeftNeighbor = head.LeftNeighbor,
        .RightNeighbor = head.RightNeighbor,
        .InUse = true};

    // no more nodes in this freelist
    if (m_freeListsHeads[listIdx] == INVALID_NODE)
    {
        m_secondLevelMask[firstLevelIdx] ^= (1 << secondLevelIdx);

        if (m_secondLevelMask[firstLevelIdx] == 0)
            m_firstLevelMask ^= (1 << firstLevelIdx);
    }

    m_freeStorage -= oldSize;

    if (leftoverSize)
    {
        const uint32_t newRightNeighbor = InsertNode(head.Offset + head.Size, leftoverSize);

        if (newRightNeighbor == INVALID_NODE)
            return Allocation::Empty();

        if (oldRightNeighbor != INVALID_NODE)
            m_nodes[oldRightNeighbor].LeftNeighbor = newRightNeighbor;

        m_nodes[newRightNeighbor].LeftNeighbor = nodeIdx;
        m_nodes[newRightNeighbor].RightNeighbor = oldRightNeighbor;
        head.RightNeighbor = newRightNeighbor;
    }

    const uint32_t alignedOffset = (uint32_t)Math::AlignUp(head.Offset, alignment);
    Assert(alignedOffset + size <= head.Offset + alignedSize, "invalid bin idx.");

    return Allocation{ .Size = size,
        .Offset = alignedOffset,
        .Internal = nodeIdx };
}

void OffsetAllocator::Free(const Allocation& alloc)
{
    const uint32_t listIdx = SmallFloat::uintToFloatRoundUp(alloc.Size);
    const uint32_t firstLevelIdx = listIdx >> FIRST_LEVEL_INDEX_SHIFT;
    const uint32_t secondLevelIdx = listIdx & SECOND_LEVEL_INDEX_MASK;

    const uint32_t nodeIdx = alloc.Internal;
    Assert(nodeIdx != INVALID_NODE, "invalid node index.");
    Node& node = m_nodes[nodeIdx];
    Assert(node.InUse == true, "can't free node that isn't in use.");

    uint32_t newOffset = node.Offset;
    uint32_t newSize = node.Size;
    uint32_t newLeftNeighbor = node.LeftNeighbor;
    uint32_t newRightNeighbor = node.RightNeighbor;

    if (node.LeftNeighbor != INVALID_NODE)
    {
        Node& leftNeighbor = m_nodes[node.LeftNeighbor];
        Assert(leftNeighbor.RightNeighbor == nodeIdx, "these must match.");

        if (!leftNeighbor.InUse)
        {
            newOffset = leftNeighbor.Offset;
            newSize += leftNeighbor.Size;
            newLeftNeighbor = leftNeighbor.LeftNeighbor;

            RemoveNode(node.LeftNeighbor);
        }
    }

    if (node.RightNeighbor != INVALID_NODE)
    {
        Node& rightNeighbor = m_nodes[node.RightNeighbor];
        Assert(rightNeighbor.LeftNeighbor == nodeIdx, "these must match.");

        if (!rightNeighbor.InUse)
        {
            newSize += rightNeighbor.Size;
            newRightNeighbor = rightNeighbor.RightNeighbor;

            RemoveNode(node.RightNeighbor);
        }
    }

    m_nodeStack[++m_stackTop] = nodeIdx;

    const uint32_t newNodeIdx = InsertNode(newOffset, newSize);

    m_nodes[newNodeIdx].RightNeighbor = newRightNeighbor;
    m_nodes[newNodeIdx].LeftNeighbor = newLeftNeighbor;

    if (newLeftNeighbor != INVALID_NODE)
        m_nodes[newLeftNeighbor].RightNeighbor = newNodeIdx;

    if (newRightNeighbor != INVALID_NODE)
        m_nodes[newRightNeighbor].LeftNeighbor = newNodeIdx;
}

OffsetAllocator::StorageReport OffsetAllocator::GetStorageReport() const
{
    uint32_t largestFreeRegion = 0;
    uint32_t freeStorage = 0;

    if (m_stackTop != INVALID_NODE)
    {
        freeStorage = m_freeStorage;

        if (m_firstLevelMask)
        {
            const uint32_t firstLevel = 31 - _lzcnt_u32(m_firstLevelMask);
            const uint32_t secondLevel = 31 - _lzcnt_u32(m_secondLevelMask[firstLevel]);

            largestFreeRegion = SmallFloat::floatToUint((firstLevel << FIRST_LEVEL_INDEX_SHIFT) + secondLevel);
            Assert(freeStorage >= largestFreeRegion, "");
        }
    }

    return { .TotalFreeSpace = freeStorage, .LargestFreeRegion = largestFreeRegion };
}
