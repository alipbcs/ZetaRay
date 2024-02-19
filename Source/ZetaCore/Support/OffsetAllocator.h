#pragma once

#include "../App/ZetaRay.h"

namespace ZetaRay::Support
{
    // Ref: https://github.com/sebbbi/OffsetAllocator
    class OffsetAllocator
    {
    public:
        static constexpr uint32_t INVALID_INDEX = uint32_t(-1);
        static constexpr uint32_t INVALID_NODE = uint32_t(-1);

        struct Allocation
        {
            static Allocation Empty()
            {
                return Allocation{ .Size = 0,
                    .Offset = 0,
                    .Internal = INVALID_NODE };
            }

            bool IsEmpty() const { return Internal == INVALID_NODE; }

            uint32_t Size;
            uint32_t Offset;
            uint32_t Internal;
        };

        struct StorageReport
        {
            uint32_t TotalFreeSpace;
            uint32_t LargestFreeRegion;
        };

        OffsetAllocator() = default;
        OffsetAllocator(uint32_t size, uint32_t maxNumAllocs);
        ~OffsetAllocator();

        OffsetAllocator(OffsetAllocator&& rhs);
        OffsetAllocator& operator=(OffsetAllocator&& rhs);

        void Init(uint32_t size, uint32_t maxNumAllocs);
        Allocation Allocate(uint32_t size, uint32_t alignment = 1);
        void Free(const Allocation& alloc);
        void Reset();
        uint32_t FreeStorage() const { return m_freeStorage; }
        StorageReport GetStorageReport() const;

    private:
        static constexpr uint32_t NUM_FIRST_LEVEL_BINS = 32;
        static constexpr uint32_t NUM_SPLITS_PER_FIRST_LEVEL_BIN = 8;
        static constexpr uint32_t FIRST_LEVEL_INDEX_SHIFT = 3;
        static constexpr uint32_t SECOND_LEVEL_INDEX_MASK = NUM_SPLITS_PER_FIRST_LEVEL_BIN - 1;

        struct Node
        {
            uint32_t Offset = 0;
            uint32_t Size = 0;
            uint32_t Next = INVALID_NODE;
            uint32_t Prev = INVALID_NODE;
            uint32_t LeftNeighbor = INVALID_NODE;
            uint32_t RightNeighbor = INVALID_NODE;
            bool InUse = false;
        };

        uint32_t InsertNode(uint32_t offset, uint32_t size);
        void RemoveNode(uint32_t nodeIdx);

        uint32_t m_size;
        uint32_t m_maxNumAllocs;
        uint32_t m_freeStorage;

        uint32_t m_firstLevelMask = 0;
        uint8_t m_secondLevelMask[NUM_FIRST_LEVEL_BINS];

        // List i contains nodes N such that,
        //        i = SmallFloat(N.size)
        // 
        // e.g. for i = 35, SmallFloat(x) = 35 for x in [88, 96) 
        uint32_t m_freeListsHeads[NUM_FIRST_LEVEL_BINS * NUM_SPLITS_PER_FIRST_LEVEL_BIN];
        Node* m_nodes = nullptr;
        uint32_t* m_nodeStack = nullptr;
        uint32_t m_stackTop;
    };
}
