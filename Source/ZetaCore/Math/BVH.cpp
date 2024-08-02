#include "BVH.h"
#include "../Math/CollisionFuncs.h"
#include "../Utility/Error.h"
#include "../App/Log.h"
#include "../Scene/SceneCommon.h"
#include <algorithm>

using namespace ZetaRay::Util;
using namespace ZetaRay::Math;

namespace
{
    struct alignas(16) Bin
    {
        ZetaInline void __vectorcall Extend(v_AABB box)
        {
            Box = NumEntries > 0 ? compueUnionAABB(Box, box) : box;
            NumEntries++;
        }

        ZetaInline void __vectorcall Extend(Bin bin)
        {
            Box = NumEntries > 0 ? compueUnionAABB(Box, bin.Box) : bin.Box;
            NumEntries += bin.NumEntries;
        }

        v_AABB Box = v_AABB(float3(0.0f), float3(-FLT_MAX));
        uint32_t NumEntries = 0;
    };
}

//--------------------------------------------------------------------------------------
// Node
//--------------------------------------------------------------------------------------

void BVH::Node::InitAsLeaf(int base, int count, int parent)
{
    Assert(count, "Invalid count");
    Base = base;
    Count = count;
//    AABB.Extents = float3(0.0f, 0.0f, 0.0f);
    RightChild = -1;
    Parent = parent;
}

void BVH::Node::InitAsInternal(Span<BVH::BVHInput> instances, int base, int count,
    int right, int parent)
{
    Assert(count, "Invalid count");
    Assert(base + count <= instances.size(), "Invalid base/count.");

    v_AABB vBox(instances[base].BoundingBox);

    for (int i = base + 1; i < base + count; i++)
        vBox = compueUnionAABB(vBox, v_AABB(instances[i].BoundingBox));

    BoundingBox = store(vBox);
    RightChild = right;
    Parent = parent;
}

//--------------------------------------------------------------------------------------
// BVH
//--------------------------------------------------------------------------------------

BVH::BVH()
    : m_arena(4 * 1096),
    m_instances(m_arena),
    m_nodes(m_arena)
{}

void BVH::Build(Span<BVHInput> instances)
{
    if (instances.size() == 0)
        return;

    //m_instances.swap(instances);
    m_instances.append_range(instances.begin(), instances.end(), true);
    Check(m_instances.size() < UINT32_MAX, "#Instances can't exceed UINT32_MAX.");
    const uint32_t numInstances = (uint32_t)m_instances.size();

    // Special case when there's less than MAX_NUM_MODELS_PER_LEAF instances
    if (m_instances.size() <= MAX_NUM_INSTANCES_PER_LEAF)
    {
        m_nodes.resize(1);

        v_AABB vBox(m_instances[0].BoundingBox);

        for (int i = 1; i < m_instances.size(); i++)
            vBox = compueUnionAABB(vBox, v_AABB(m_instances[i].BoundingBox));

        m_nodes[0].BoundingBox = store(vBox);
        m_nodes[0].Base = 0;
        m_nodes[0].Count = (int)m_instances.size();
        m_nodes[0].RightChild = -1;

        return;
    }

    // TODO check this computation
    const uint32_t MAX_NUM_NODES = Math::CeilUnsignedIntDiv(4 * numInstances, MAX_NUM_INSTANCES_PER_LEAF) + 1;
    m_nodes.resize(MAX_NUM_NODES);

    BuildSubtree(0, numInstances, -1);
}

int BVH::BuildSubtree(int base, int count, int parent)
{
    Assert(count > 0, "Number of nodes to build a subtree for must be greater than 0.");
    const uint32_t currNodeIdx = m_numNodes++;
    Assert(!m_nodes[currNodeIdx].IsInitialized(), "invalid index");

    // Create a leaf node and return
    if (count <= MAX_NUM_INSTANCES_PER_LEAF)
    {
        m_nodes[currNodeIdx].InitAsLeaf(base, count, parent);
        return currNodeIdx;
    }

    // Compute union AABB of all centroids
    __m128 vMinPoint = _mm_set_ps1(FLT_MAX);
    __m128 vMaxPoint = _mm_set_ps1(-FLT_MAX);
    // Union AABB of all nodes in this subtree
    v_AABB vNodeBox(m_instances[base].BoundingBox);

    for (int i = base; i < base + count; i++)
    {
        v_AABB vInstanceBox(m_instances[i].BoundingBox);

        vMinPoint = _mm_min_ps(vMinPoint, vInstanceBox.vCenter);
        vMaxPoint = _mm_max_ps(vMaxPoint, vInstanceBox.vCenter);

        vNodeBox = compueUnionAABB(vNodeBox, v_AABB(m_instances[i].BoundingBox));
    }

    v_AABB vCentroidAABB;
    vCentroidAABB.Reset(vMinPoint, vMaxPoint);
    AABB centroidAABB;
    centroidAABB = store(vCentroidAABB);

    // All centroids are (almost) the same point, no point in splitting further
    if (centroidAABB.Extents.x + centroidAABB.Extents.y + centroidAABB.Extents.z <= 1e-5f)
    {
        m_nodes[currNodeIdx].InitAsLeaf(base, count, parent);
        return currNodeIdx;
    }

    // Axis along which partitioning should be performed
    const float* extArr = reinterpret_cast<float*>(&centroidAABB.Extents);
    int splitAxis = 0;
    float maxExtent = extArr[0];

    // Find the longest axis
    for (int i = 1; i < 3; i++)
    {
        if (extArr[i] > maxExtent)
        {
            maxExtent = extArr[i];
            splitAxis = i;
        }
    }

    uint32_t splitCount;

    // Split using SAH
    if (count >= MIN_NUM_INSTANCES_SPLIT_SAH)
    {
        Bin bins[NUM_SAH_BINS];
        const float leftMostPlane = reinterpret_cast<float*>(&centroidAABB.Center)[splitAxis] - maxExtent;
        const float rcpStepSize = NUM_SAH_BINS / (2.0f * maxExtent);

        // Assign each instance to one bin
        for (int i = base; i < base + count; i++)
        {
            const float* center = reinterpret_cast<float*>(&m_instances[i].BoundingBox.Center);
            float numBinWidthsFromLeftMostPlane = (center[splitAxis] - leftMostPlane) * rcpStepSize;
            int bin = Math::Min((int)numBinWidthsFromLeftMostPlane, (int)NUM_SAH_BINS - 1);

            v_AABB box(m_instances[i].BoundingBox);
            bins[bin].Extend(box);
        }

        Assert(bins[0].NumEntries > 0 && bins[NUM_SAH_BINS - 1].NumEntries > 0, "first & last bin must contain at least 1 instance.");

        // N bins correspond to N - 1 split planes, e.g. for N = 4
        //        bin 0 | bin 1 | bin 2 | bin 3 
        float leftSurfaceArea[NUM_SAH_BINS - 1];
        float rightSurfaceArea[NUM_SAH_BINS - 1];
        uint32_t leftCount[NUM_SAH_BINS - 1];
        uint32_t rightCount[NUM_SAH_BINS - 1];

        {
            // For each split plane corresponding to each bin, compute surface area of nodes
            // to its left and right
            v_AABB currLeftBox = bins[0].Box;
            v_AABB currRightBox = bins[NUM_SAH_BINS - 1].Box;
            uint32_t currLeftSum = 0;
            uint32_t currRightSum = 0;

            for (int plane = 0; plane < NUM_SAH_BINS - 1; plane++)
            {
                currLeftSum += bins[plane].NumEntries;
                leftCount[plane] = currLeftSum;

                currLeftBox = compueUnionAABB(bins[plane].Box, currLeftBox);
                leftSurfaceArea[plane] = computeAABBSurfaceArea(currLeftBox);
                currRightSum += bins[NUM_SAH_BINS - 1 - plane].NumEntries;
                rightCount[NUM_SAH_BINS - 2 - plane] = currRightSum;

                currRightBox = compueUnionAABB(bins[NUM_SAH_BINS - 1 - plane].Box, currRightBox);
                rightSurfaceArea[NUM_SAH_BINS - 2 - plane] = computeAABBSurfaceArea(currRightBox);
            }
        }

        int lowestCostPlane = -1;
        float lowestCost = FLT_MAX;
        const float parentSurfaceArea = computeAABBSurfaceArea(vNodeBox);

        // Cost of split along each split plane
        for (int i = 0; i < NUM_SAH_BINS - 1; i++)
        {
            const float splitCost = leftCount[i] * leftSurfaceArea[i] / parentSurfaceArea +
                rightCount[i] * rightSurfaceArea[i] / parentSurfaceArea;

            if (splitCost < lowestCost)
            {
                lowestCost = splitCost;
                lowestCostPlane = i;
            }
        }

        const float noSplitCost = (float)count;
        if (noSplitCost <= lowestCost)
        {
            m_nodes[currNodeIdx].InitAsLeaf(base, count, parent);
            return currNodeIdx;
        }

        Assert(lowestCostPlane != -1, "bug");
        const float splitPlane = leftMostPlane + (lowestCostPlane + 1) / rcpStepSize;    // == * StepSize

        auto it = std::partition(m_instances.begin() + base, m_instances.begin() + base + count,
            [splitPlane, splitAxis](BVHInput& box)
            {
                float c = reinterpret_cast<float*>(&box.BoundingBox.Center)[splitAxis];
                return c <= splitPlane;
            });

        splitCount = (uint32_t)(it - m_instances.begin() - base);
        if (splitCount != leftCount[lowestCostPlane])
            LOG_UI_WARNING("BVH::Build(): floating-point imprecision detected.");
    }
    else
    {
        // Split into two subtrees such that each subtree has an equal number of nodes (i.e. find the median)
        const uint32_t countDiv2 = (count >> 1);
        auto begIt = m_instances.begin() + base;
        auto midIt = m_instances.begin() + base + countDiv2;
        auto endIt = m_instances.begin() + base + count;
        std::nth_element(begIt, midIt, endIt,
            [splitAxis](BVHInput& b1, BVHInput& b2)
            {
                float* box1 = reinterpret_cast<float*>(&b1.BoundingBox);
                float* box2 = reinterpret_cast<float*>(&b2.BoundingBox);
                // compare AABB centers along the split axis
                return box1[splitAxis] < box2[splitAxis];
            });

        splitCount = countDiv2;
    }

    Assert(splitCount > 0, "bug");
    uint32_t left = BuildSubtree(base, splitCount, currNodeIdx);
    uint32_t right = BuildSubtree(base + splitCount, count - splitCount, currNodeIdx);
    Assert(left == currNodeIdx + 1, "Index of left child should be equal to current parent's index plus one");

    m_nodes[currNodeIdx].InitAsInternal(m_instances, base, count, right, parent);

    return currNodeIdx;
}

int BVH::Find(uint64_t instanceID, const Math::AABB& queryBox, int& nodeIdx)
{
    nodeIdx = -1;

    // Using a manual stack, we can return early when a match is found, whereas
    // with a recursive call, travelling back through the call-chain is required
    constexpr int STACK_SIZE = 64;
    int stack[STACK_SIZE];
    int currStackIdx = 0;
    stack[currStackIdx] = 0;    // insert root

    v_AABB vBox(queryBox);

    // Can return early if root doesn't intersect or contain the given AABB
    if(Math::intersectAABBvsAABB(vBox, v_AABB(m_nodes[0].BoundingBox)) == COLLISION_TYPE::DISJOINT)
        return -1;

    int currNodeIdx = -1;

    while (currStackIdx >= 0)
    {
        Assert(currStackIdx < 64, "Stack size exceeded 64.");

        currNodeIdx = stack[currStackIdx--];
        const Node& node = m_nodes[currNodeIdx];

        if (node.IsLeaf())
        {
            for (int i = node.Base; i < node.Base + node.Count; i++)
            {
                if (m_instances[i].InstanceID == instanceID)
                {
                    nodeIdx = currNodeIdx;
                    return i;
                }
            }

            continue;
        }

        v_AABB vNodeBox(node.BoundingBox);

        if(Math::intersectAABBvsAABB(vNodeBox, vBox) != COLLISION_TYPE::DISJOINT)
        {
            // Decide which tree to descend on first
            v_AABB vLeft(m_nodes[currNodeIdx + 1].BoundingBox);
            v_AABB vRight(m_nodes[node.RightChild].BoundingBox);

            v_AABB vOverlapLeft = Math::computeOverlapAABB(vBox, vLeft);
            v_AABB vOverlapRight = Math::computeOverlapAABB(vBox, vRight);

            Math::AABB Left = Math::store(vOverlapLeft);
            Math::AABB Right = Math::store(vOverlapRight);

            float leftOverlapVolume = m_nodes[currNodeIdx + 1].IsLeaf() ? FLT_MAX : 
                Left.Extents.x * Left.Extents.y * Left.Extents.z;
            float rightOverlapVolume = m_nodes[node.RightChild].IsLeaf() ? FLT_MAX : 
                Right.Extents.x * Right.Extents.y * Right.Extents.z;

            // Larger overlap with the right subtree, descend throught that first
            if (leftOverlapVolume <= rightOverlapVolume)
            {
                stack[++currStackIdx] = currNodeIdx + 1;
                stack[++currStackIdx] = node.RightChild;
            }
            else
            {
                stack[++currStackIdx] = node.RightChild;
                stack[++currStackIdx] = currNodeIdx + 1;
            }
        }
    }

    return currNodeIdx;
}

void BVH::Update(Span<BVHUpdateInput> instances)
{
    for (auto& [oldBox, newBox, id] : instances)
    {
        // Find the leaf node that contains it
        int nodeIdx;
        int instanceIdx = Find(id, oldBox, nodeIdx);
        Assert(instanceIdx != -1, "Instance with ID %u was not found.", id);

        // Update the bounding box
        Node& node = m_nodes[nodeIdx];
        m_instances[instanceIdx].BoundingBox = newBox;

        const v_AABB vOldBox(oldBox);
        const v_AABB vNewBox(newBox);

        Math::COLLISION_TYPE res = Math::intersectAABBvsAABB(vOldBox, vNewBox);

        // If the old AABB contains the new one, keep using the old one
        if (res != COLLISION_TYPE::CONTAINS)
        {
            int currParent = node.Parent;

            // Following the parent indices, keep going up the tree and merge the AABBs. Break once a parent node's
            // AABB contains the new one
            while (currParent != -1)
            {
                Node& parentNode = m_nodes[currParent];

                v_AABB vParentBox(parentNode.BoundingBox);
                if (Math::intersectAABBvsAABB(vParentBox, vNewBox) == COLLISION_TYPE::CONTAINS)
                    break;

                vParentBox = Math::compueUnionAABB(vParentBox, vNewBox);
                parentNode.BoundingBox = Math::store(vParentBox);

                currParent = m_nodes[currParent].Parent;
            }
        }

        // Note: when the new AABB and the old AABB are disjoint, it'd be better to
        // remove and then reinsert the update Node. That requires modifying the range of
        // all the leaves, which is expensive
    }
}

void BVH::Remove(uint64_t ID, const Math::AABB& box)
{
    // Find the leaf node that contains it
    int nodeIdx;
    const int instanceIdx = Find(ID, box, nodeIdx);
    Assert(instanceIdx != -1, "Instance with ID %u was not found.", ID);

    m_instances[instanceIdx].InstanceID = Scene::INVALID_INSTANCE;
    m_instances[instanceIdx].BoundingBox.Extents = float3(-1.0f, -1.0f, -1.0f);
    m_instances[instanceIdx].BoundingBox.Center = float3(0.0f, 0.0f, 0.0f);

    // Swap with the last Instance in this leaf
    const uint32_t swapIdx = m_nodes[nodeIdx].Base + m_nodes[nodeIdx].Count - 1;
    std::swap(m_instances[instanceIdx], m_instances[swapIdx]);
    m_nodes[nodeIdx].Count--;
}

void BVH::DoFrustumCulling(const Math::ViewFrustum& viewFrustum, 
    const Math::float4x4a& viewToWorld, 
    Vector<uint64_t, App::FrameAllocator>& visibleInstanceIDs)
{
    // Transform view frustum from view space into world space
    v_float4x4 vM = load4x4(const_cast<float4x4a&>(viewToWorld));
    v_ViewFrustum vFrustum(const_cast<ViewFrustum&>(viewFrustum));
    vFrustum = Math::transform(vM, vFrustum);

    v_AABB vBox(m_nodes[0].BoundingBox);

    // root doesn't intersect camera
    if (Math::instersectFrustumVsAABB(vFrustum, vBox) == COLLISION_TYPE::DISJOINT)
        return;

    // Manual stack
    constexpr int STACK_SIZE = 64;
    int stack[STACK_SIZE];
    int currStackIdx = 0;

    // Insert root
    stack[currStackIdx] = 0;
    int currNode = -1;

    while (currStackIdx >= 0)
    {
        Assert(currStackIdx < 64, "Stack size exceeded maximum allowed.");

        currNode = stack[currStackIdx--];
        const Node& node = m_nodes[currNode];

        if (node.IsLeaf())
        {
            for (int i = node.Base; i < node.Base + node.Count; i++)
            {
                vBox.Reset(m_instances[i].BoundingBox);

                if (Math::instersectFrustumVsAABB(vFrustum, vBox) != COLLISION_TYPE::DISJOINT)
                    visibleInstanceIDs.push_back(m_instances[i].InstanceID);
            }
        }
        else
        {
            vBox.Reset(node.BoundingBox);

            if (Math::instersectFrustumVsAABB(vFrustum, vBox) != COLLISION_TYPE::DISJOINT)
            {
                stack[++currStackIdx] = node.RightChild;
                stack[++currStackIdx] = currNode + 1;
            }
        }
    }
}

void BVH::DoFrustumCulling(const Math::ViewFrustum& viewFrustum,
    const Math::float4x4a& viewToWorld,
    Vector<BVHInput, App::FrameAllocator>& visibleInstanceIDs)
{
    // Transform view frustum from view space into world space
    v_float4x4 vM = load4x4(const_cast<float4x4a&>(viewToWorld));
    v_ViewFrustum vFrustum(const_cast<ViewFrustum&>(viewFrustum));
    vFrustum = Math::transform(vM, vFrustum);

    v_AABB vBox(m_nodes[0].BoundingBox);

    // Root doesn't intersect camera
    if (Math::instersectFrustumVsAABB(vFrustum, vBox) == COLLISION_TYPE::DISJOINT)
        return;

    // Manual stack
    constexpr int STACK_SIZE = 64;
    int stack[STACK_SIZE];
    int currStackIdx = 0;

    // Insert root
    stack[currStackIdx] = 0;
    int currNode = -1;

    while (currStackIdx >= 0)
    {
        Assert(currStackIdx < 64, "Stack size exceeded maximum allowed.");

        currNode = stack[currStackIdx--];
        const Node& node = m_nodes[currNode];

        if (node.IsLeaf())
        {
            for (int i = node.Base; i < node.Base + node.Count; i++)
            {
                vBox.Reset(m_instances[i].BoundingBox);

                if (Math::instersectFrustumVsAABB(vFrustum, vBox) != COLLISION_TYPE::DISJOINT)
                {
                    visibleInstanceIDs.emplace_back(BVH::BVHInput{
                        .BoundingBox = m_instances[i].BoundingBox,
                        .InstanceID = m_instances[i].InstanceID });
                }
            }
        }
        else
        {
            vBox.Reset(node.BoundingBox);

            if (Math::instersectFrustumVsAABB(vFrustum, vBox) != COLLISION_TYPE::DISJOINT)
            {
                stack[++currStackIdx] = node.RightChild;
                stack[++currStackIdx] = currNode + 1;
            }
        }
    }
}

uint64_t BVH::CastRay(v_Ray& vRay)
{
    v_AABB vBox(m_nodes[0].BoundingBox);
    float t;

    const __m128 vIsParallel = _mm_cmpge_ps(_mm_set1_ps(FLT_EPSILON), abs(vRay.vDir));
     const __m128 vDirRcp = _mm_div_ps(_mm_set1_ps(1.0f), vRay.vDir);
    const __m128 vDirIsPos = _mm_cmpge_ps(vRay.vDir, _mm_setzero_ps());

    // Can return early if ray doesn't intersect root AABB
    if (!Math::intersectRayVsAABB(vRay, vDirRcp, vDirIsPos, vIsParallel, vBox, t))
        return Scene::INVALID_INSTANCE;

    // Manual stack
    constexpr int STACK_SIZE = 64;
    int stack[STACK_SIZE];
    int currStackIdx = 0;

    // Insert root
    stack[currStackIdx] = 0;
    int currNode = -1;
    float minT = FLT_MAX;
    uint64_t closestID = Scene::INVALID_INSTANCE;

    while (currStackIdx >= 0)
    {
        Assert(currStackIdx < STACK_SIZE, "Stack size exceeded 64.");

        currNode = stack[currStackIdx--];
        const Node& node = m_nodes[currNode];

        if (node.IsLeaf())
        {
            for (int i = node.Base; i < node.Base + node.Count; i++)
            {
                vBox.Reset(m_instances[i].BoundingBox);

                if (Math::intersectRayVsAABB(vRay, vDirRcp, vDirIsPos, vIsParallel, vBox, t))
                {
                    const bool tLtTmin = t < minT;
                    minT = tLtTmin ? t : minT;
                    closestID = tLtTmin ? m_instances[i].InstanceID : closestID;
                }
            }
        }
        else
        {
            const Node& leftChild = m_nodes[currNode + 1];
            const Node& rightChild = m_nodes[node.RightChild];
            const v_AABB vLeftBox(leftChild.BoundingBox);
            const v_AABB vRightBox(rightChild.BoundingBox);
            float leftT;
            float rightT;

            const bool hitLeftChild = Math::intersectRayVsAABB(vRay, vDirRcp, vDirIsPos, vIsParallel, vLeftBox, leftT);
            const bool hitRightChild = Math::intersectRayVsAABB(vRay, vDirRcp, vDirIsPos, vIsParallel, vRightBox, rightT);

            int sortedByT[2] = { currNode + 1, node.RightChild };
            const int numChilds = hitLeftChild + hitRightChild;

            // Make sure subtree closer to camera is searched first
            if (numChilds == 2 && leftT < rightT)
                std::swap(sortedByT[0], sortedByT[1]);

            for (int c = 0; c < numChilds; c++)
            {
                // No need to search this subtree as earlier hits are necessarily closer to camera
                if(sortedByT[c] < minT)
                    stack[++currStackIdx] = sortedByT[c];
            }
        }
    }

    return closestID;
}

uint64_t BVH::CastRay(Math::Ray& r)
{
    v_Ray vRay(r);
    return CastRay(vRay);
}
