// References:
// 1. M. Pharr, W. Jakob, and G. Humphreys, Physically Based Rendering: From theory to implementation, Morgan Kaufmann, 2016.
// 2. C. Ericson, Real-time Collision Detection, Morgan Kaufmann, 2005.

#pragma once

#include "../Utility/Span.h"
#include "../Math/CollisionTypes.h"
#include "../Support/MemoryArena.h"
#include "../App/App.h"

namespace ZetaRay::Math
{
    struct alignas(16) float4x4a;

    class BVH
    {
    public:
        struct alignas(16) BVHInput
        {
            Math::AABB AABB;
            uint64_t ID;
        };

        struct alignas(16) BVHUpdateInput
        {
            Math::AABB OldBox;
            Math::AABB NewBox;
            uint64_t ID;
        };

        BVH();
        ~BVH() = default;

        BVH(BVH&&) = delete;
        BVH& operator=(BVH&&) = delete;

        bool IsBuilt() { return m_nodes.size() != 0; }
        void Clear();

        void Build(Util::Span<BVHInput> instances);
        void Update(Util::Span<BVHUpdateInput> instances);
        void Remove(uint64_t ID, const Math::AABB& AABB);

        // Returns ID of instances that at least partially overlap the view frustum. Assumes 
        // the view frustum is in the view space
        void DoFrustumCulling(const Math::ViewFrustum& viewFrustum, 
            const Math::float4x4a& viewToWorld,
            Util::Vector<uint64_t, App::FrameAllocator>& visibleInstanceIDs);

        // Returns IDs & AABBs of instances that at least partially overlap the view frustum. Assumes 
        // the view frustum is in the view space
        void DoFrustumCulling(const Math::ViewFrustum& viewFrustum,
            const Math::float4x4a& viewToWorld,
            Util::Vector<BVHInput, App::FrameAllocator>& visibleInstanceIDs);

        // Casts a ray into the BVH and returns the closest-hit intersection. Ray is assumed to 
        // be in world space
        uint64_t CastRay(Math::Ray& r);

        // Returns AABB that contains the scene
        Math::AABB GetWorldAABB() 
        {
            Assert(m_nodes.size() > 0, "BVH hasn't been built yet.");
            return m_nodes[0].AABB; 
        }

    private:
        // maximum number of instances that can be included in a leaf node
        static constexpr uint32_t MAX_NUM_INSTANCES_PER_LEAF = 8;
        static constexpr uint32_t MIN_NUM_INSTANCES_SPLIT_SAH = 10;
        static constexpr uint32_t NUM_SAH_BINS = 6;

        struct alignas(64) Node
        {
            bool IsInitialized() { return Parent != -1; }
            void InitAsLeaf(int base, int count, int parent);
            void InitAsInternal(Util::Span<BVH::BVHInput> instances, int base, int count,
                int right, int parent);
            bool IsLeaf() const { return RightChild == -1; }

            // Union AABB of all the child nodes for internal nodes
            // also used to distinguish between leaf & internal nodes
            Math::AABB AABB;

            /*
            union
            {
                struct
                {
                    int Base;
                    int Count;
                } Leaf;

                int RightChild;
            };
            */

            // for leafs
            int Base;
            int Count;

            // for internal nodes
            int RightChild;

            int Parent = -1;
        };

        // Recursively builds a BVH (subtree) for the given range
        int BuildSubtree(int base, int count, int parent);

        // Finds the leaf node that contains the given instance. Returns -1 otherwise.
        int Find(uint64_t ID, const Math::AABB& AABB, int& modelIdx);

        Support::MemoryArena m_arena;

        // tree hierarchy is stored as an array
        Util::SmallVector<Node, Support::ArenaAllocator> m_nodes;

        // array of inputs to build a BVH for. During BVH build, elements are moved around
        Util::SmallVector<BVHInput, Support::ArenaAllocator> m_instances;

        uint32_t m_numNodes = 0;
    };
}
