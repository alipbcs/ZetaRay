#include "BVH.h"
#include "../Math/CollisionFuncs.h"
#include "../Utility/Error.h"
#include <algorithm>

using namespace ZetaRay::Util;
using namespace ZetaRay::Math;

namespace
{
	struct alignas(16) Bin
	{
		ZetaInline void __vectorcall Extend(v_AABB box) noexcept
		{
			Box = NumModels > 0 ? compueUnionAABB(Box, box) : box;
			NumModels++;
		}

		ZetaInline void __vectorcall Extend(Bin bin) noexcept
		{
			Box = NumModels > 0 ? compueUnionAABB(Box, bin.Box) : bin.Box;
			NumModels += bin.NumModels;
		}

		v_AABB Box;
		uint32_t NumModels = 0;
	};
}

//--------------------------------------------------------------------------------------
// Node
//--------------------------------------------------------------------------------------

void BVH::Node::InitAsLeaf(int base, int count, int parent) noexcept
{
	Assert(count, "Invalid args");
	Base = base;
	Count = count;
//	AABB.Extents = float3(0.0f, 0.0f, 0.0f);
	RightChild = -1;
	Parent = parent;
}

void BVH::Node::InitAsInternal(Span<BVH::BVHInput> instances, int base, int count,
	int right, int parent) noexcept
{
	Assert(count, "Invalid args");
	Assert(base + count <= instances.size(), "Invalid args");

	v_AABB vBox(instances[base].AABB);

	for (int i = base + 1; i < base + count; i++)
		vBox = compueUnionAABB(vBox, v_AABB(instances[i].AABB));

	AABB = store(vBox);
	RightChild = right;
	Parent = parent;
}

//--------------------------------------------------------------------------------------
// BVH
//--------------------------------------------------------------------------------------

BVH::BVH() noexcept
	: m_arena(4 * 1096),
	m_instances(m_arena),
	m_nodes(m_arena)
{
}

void BVH::Clear() noexcept
{
	m_nodes.free_memory();
	m_instances.free_memory();
}

void BVH::Build(Span<BVHInput> instances) noexcept
{
	if (instances.size() == 0)
		return;

	//m_instances.swap(instances);
	m_instances.append_range(instances.begin(), instances.end(), true);
	Assert(m_instances.size() < UINT32_MAX, "not supported");
	const uint32_t numInstances = (uint32_t)m_instances.size();

	// special case when there's less than MAX_NUM_MODELS_PER_LEAF instances
	if (m_instances.size() <= MAX_NUM_INSTANCES_PER_LEAF)
	{
		m_nodes.resize(1);

		v_AABB vBox(m_instances[0].AABB);

		for (int i = 1; i < m_instances.size(); i++)
			vBox = compueUnionAABB(vBox, v_AABB(m_instances[i].AABB));

		m_nodes[0].AABB = store(vBox);
		m_nodes[0].Base = 0;
		m_nodes[0].Count = (int)m_instances.size();
		m_nodes[0].RightChild = -1;

		return;
	}

	// TODO check this computation
	const uint32_t MAX_NUM_NODES = (uint32_t)Math::CeilUnsignedIntDiv(4 * numInstances, MAX_NUM_INSTANCES_PER_LEAF) + 1;
	m_nodes.resize(MAX_NUM_NODES);

	BuildSubtree(0, numInstances, -1);
}

int BVH::BuildSubtree(int base, int count, int parent) noexcept
{
	Assert(count > 0, "Number of nodes to build a subtree for must be > 0.");
	const uint32_t currNodeIdx = m_numNodes++;
	Assert(!m_nodes[currNodeIdx].IsInitialized(), "invalid index");
	
	// create a leaf node and return
	if (count <= MAX_NUM_INSTANCES_PER_LEAF)
	{
		m_nodes[currNodeIdx].InitAsLeaf(base, count, parent);
		return currNodeIdx;
	}

	// union AABB of all the centroids
	AABB centroidAABB;

	__m128 vMinPoint = _mm_set_ps1(FLT_MAX);
	__m128 vMaxPoint = _mm_set_ps1(-FLT_MAX);
	v_AABB vNodeBox(m_instances[base].AABB);

	for (int i = base; i < base + count; i++)
	{
		v_AABB vInstanceBox(m_instances[i].AABB);

		vMinPoint = _mm_min_ps(vMinPoint, vInstanceBox.vCenter);
		vMaxPoint = _mm_max_ps(vMaxPoint, vInstanceBox.vCenter);

		vNodeBox = compueUnionAABB(vNodeBox, v_AABB(m_instances[i].AABB));
	}

	v_AABB vCentroidAABB;
	vCentroidAABB.Reset(vMinPoint, vMaxPoint);
	centroidAABB = store(vCentroidAABB);

	// all the centroids are on the same point, no point in splitting further
	if (centroidAABB.Extents.x + centroidAABB.Extents.y + centroidAABB.Extents.z <= 1e-5f)
	{
		m_nodes[currNodeIdx].InitAsLeaf(base, count, parent);
		return currNodeIdx;
	}

	// axis along which paritioning should be performed
	const float* extArr = reinterpret_cast<float*>(&centroidAABB.Extents);
	int splitAxis = 0;
	float maxExtent = extArr[0];

	for (int i = 1; i < 3; i++)
	{
		if (extArr[i] > maxExtent)
		{
			maxExtent = extArr[i];
			splitAxis = i;
		}
	}

	uint32_t splitCount;

	// split using SAH
	if (count >= MIN_NUM_INSTANCES_SPLIT_SAH)
	{
		Bin bins[NUM_SAH_BINS];
		const float leftMostPlane = reinterpret_cast<float*>(&centroidAABB.Center)[splitAxis] - maxExtent;
		const float rcpStepSize = NUM_SAH_BINS / (2.0f * maxExtent);

		// assign each instace to one bin
		for (int i = base; i < base + count; i++)
		{
			const float* center = reinterpret_cast<float*>(&m_instances[i].AABB.Center);
			const int bin = std::min((int)((center[splitAxis] - leftMostPlane) * rcpStepSize), NUM_SAH_BINS - 1);
			Assert(bin < NUM_SAH_BINS, "invalid bin");

			v_AABB box(m_instances[i].AABB);
			bins[bin].Extend(box);
		}

		Assert(bins[0].NumModels > 0 && bins[NUM_SAH_BINS - 1].NumModels > 0, "first & last bins must contain at least 1 instance");

		//constexpr int NUM_PLANES = NUM_SAH_BINS - 1;
		float leftSurfaceArea[NUM_SAH_BINS - 1];
		float rightSurfaceArea[NUM_SAH_BINS - 1];
		uint32_t leftCount[NUM_SAH_BINS - 1];
		uint32_t rightCount[NUM_SAH_BINS - 1];

		{
			v_AABB currLeftBox = bins[0].Box;
			v_AABB currRightBox = bins[NUM_SAH_BINS - 1].Box;
			uint32_t currLeftSum = 0;
			uint32_t currRightSum = 0;

			// for each split plane
			for (int plane = 0; plane < NUM_SAH_BINS - 1; plane++)
			{
				currLeftSum += bins[plane].NumModels;
				leftCount[plane] = currLeftSum;

				currLeftBox = compueUnionAABB(bins[plane].Box, currLeftBox);
				leftSurfaceArea[plane] = computeAABBSurfaceArea(currLeftBox);
				currRightSum += bins[NUM_SAH_BINS - 1 - plane].NumModels;
				rightCount[NUM_SAH_BINS - 2 - plane] = currRightSum;

				currRightBox = compueUnionAABB(bins[NUM_SAH_BINS - 1 - plane].Box, currRightBox);
				rightSurfaceArea[NUM_SAH_BINS - 2 - plane] = computeAABBSurfaceArea(currRightBox);
			}
		}

		int lowestCostPlane = -1;
		float lowestCost = FLT_MAX;
		const float parentSurfaceArea = computeAABBSurfaceArea(vNodeBox);

		// cost of split along each each split plane
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
		const float splitPlane = leftMostPlane + (lowestCostPlane + 1) / rcpStepSize;	// == * StepSize

		auto it = std::partition(m_instances.begin() + base, m_instances.begin() + base + count,
			[splitPlane, splitAxis](BVHInput& box)
			{
				float c = reinterpret_cast<float*>(&box.AABB.Center)[splitAxis];
				return c <= splitPlane;
			});

		splitCount = (uint32_t)(it - m_instances.begin() - base);
		Assert(splitCount == leftCount[lowestCostPlane], "bug");
	}
	else
	{
		// split the nodes into two subtrees such that each subtree has an 
		// equal number of nodes (i.e. find the median)
		const uint32_t countDiv2 = (count >> 1);
		auto begIt = m_instances.begin() + base;
		auto midIt = m_instances.begin() + base + countDiv2;
		auto endIt = m_instances.begin() + base + count;
		std::nth_element(begIt, midIt, endIt,
			[splitAxis](BVHInput& b1, BVHInput& b2)
			{
				float* box1 = reinterpret_cast<float*>(&b1.AABB);
				float* box2 = reinterpret_cast<float*>(&b2.AABB);
				return box1[splitAxis] < box2[splitAxis];	// compare AABB centers along the split-axis
			});

		splitCount = countDiv2;
	}

	Assert(splitCount > 0, "bug");
	uint32_t left = BuildSubtree(base, splitCount, currNodeIdx);
	uint32_t right = BuildSubtree(base + splitCount, count - splitCount, currNodeIdx);
	Assert(left == currNodeIdx + 1, "Index of left-child should be equal to current parent's index plus one");

	m_nodes[currNodeIdx].InitAsInternal(m_instances, base, count, right, parent);

	return currNodeIdx;
}

int BVH::Find(uint64_t ID, const Math::AABB& AABB, int& nodeIdx) noexcept
{
	nodeIdx = -1;

	// use a manual stack as it'd be possible to return early when a match is found, whereas
	// in a recursive call, travel back through the call-chain is required
	constexpr int STACK_SIZE = 64;
	int stack[STACK_SIZE];
	int currStackIdx = 0;
	stack[currStackIdx] = 0;	// insert root
	
	v_AABB vBox(AABB);

	// can return early if root doesn't intersect or contain the given AABB
	if(Math::intersectAABBvsAABB(vBox, v_AABB(m_nodes[0].AABB)) == COLLISION_TYPE::DISJOINT)
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
				if (m_instances[i].ID == ID)
				{
					nodeIdx = currNodeIdx;
					return i;
				}
			}

			continue;
		}

		v_AABB vNodeBox(node.AABB);

		if(Math::intersectAABBvsAABB(vNodeBox, vBox) != COLLISION_TYPE::DISJOINT)
		{
			// decide which tree to descent on first
			v_AABB vLeft(m_nodes[currNodeIdx + 1].AABB);
			v_AABB vRight(m_nodes[node.RightChild].AABB);

			v_AABB vOverlapLeft = Math::computeOverlapAABB(vBox, vLeft);
			v_AABB vOverlapRight = Math::computeOverlapAABB(vBox, vRight);

			Math::AABB Left = Math::store(vOverlapLeft);
			Math::AABB Right = Math::store(vOverlapRight);

			float leftOverlapVolume = m_nodes[currNodeIdx + 1].IsLeaf() ? FLT_MAX : 
				Left.Extents.x * Left.Extents.y * Left.Extents.z;
			float rightOverlapVolume = m_nodes[node.RightChild].IsLeaf() ? FLT_MAX : 
				Right.Extents.x * Right.Extents.y * Right.Extents.z;

			// Bigger overlap with the right subtree, descent throught that first
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

void BVH::Update(Span<BVHUpdateInput> instances) noexcept
{
	for (auto& [oldBox, newBox, id] : instances)
	{
		// find the leaf node that contains it
		int nodeIdx;
		int instanceIdx = Find(id, oldBox, nodeIdx);
		Assert(instanceIdx != -1, "Model with ID %u was not found.", id);

		// update the bounding box
		Node& node = m_nodes[nodeIdx];
		m_instances[instanceIdx].AABB = newBox;

		const v_AABB vOldBox(oldBox);
		const v_AABB vNewBox(newBox);

		Math::COLLISION_TYPE res = Math::intersectAABBvsAABB(vOldBox, vNewBox);

		// if the old AABB contains the new one, keep using the old one
		if (res != COLLISION_TYPE::CONTAINS)
		{
			int currParent = node.Parent;

			// following the parent indices, keep going up the tree and merge the AABBs. Break once a parent node's
			// AABB contains the new one
			while (currParent != -1)
			{
				Node& parentNode = m_nodes[currParent];

				v_AABB vParentBox(parentNode.AABB);
				if (Math::intersectAABBvsAABB(vParentBox, vNewBox) == COLLISION_TYPE::CONTAINS)
					break;

				vParentBox = Math::compueUnionAABB(vParentBox, vNewBox);
				parentNode.AABB = Math::store(vParentBox);

				currParent = m_nodes[currParent].Parent;
			}
		}

		// Note: when the new AABB and old AABB are disjoint, it would've been better to
		// remove and then reinsert the update Node. That requires modifying the range of
		// all the leaves, which is expensive
	}
}

void BVH::Remove(uint64_t ID, const Math::AABB& AABB) noexcept
{
	// find the leaf node that contains it
	int nodeIdx;
	const int instanceIdx = Find(ID, AABB, nodeIdx);
	Assert(instanceIdx != -1, "Model with ID %u was not found.", ID);

	m_instances[instanceIdx].ID = uint64_t(-1);
	m_instances[instanceIdx].AABB.Extents = float3(-1.0f, -1.0f, -1.0f);
	m_instances[instanceIdx].AABB.Center = float3(0.0f, 0.0f, 0.0f);

	// swap with the last Model in this leaf
	const uint32_t swapIdx = m_nodes[nodeIdx].Base + m_nodes[nodeIdx].Count - 1;
	std::swap(m_instances[instanceIdx], m_instances[swapIdx]);
	m_nodes[nodeIdx].Count--;
}

void BVH::DoFrustumCulling(const Math::ViewFrustum& viewFrustum, 
	const Math::float4x4a& viewToWorld, 
	Vector<uint64_t, App::FrameAllocator>& visibleInstanceIDs)
{
	// transform the view frustum from view space into world space
	v_float4x4 vM(const_cast<float4x4a&>(viewToWorld));
	v_ViewFrustum vFrustum(const_cast<ViewFrustum&>(viewFrustum));
	vFrustum = Math::transform(vM, vFrustum);

	v_AABB vBox(m_nodes[0].AABB);

	// root doesn't intersect the camera
	if (Math::instersectFrustumVsAABB(vFrustum, vBox) == COLLISION_TYPE::DISJOINT)
		return;

	// manual stack
	constexpr int STACK_SIZE = 64;
	int stack[STACK_SIZE];
	int currStackIdx = 0;
	stack[currStackIdx] = 0;		// insert root
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
				vBox.Reset(m_instances[i].AABB);

				if (Math::instersectFrustumVsAABB(vFrustum, vBox) != COLLISION_TYPE::DISJOINT)
					visibleInstanceIDs.push_back(m_instances[i].ID);
			}
		}
		else
		{
			vBox.Reset(node.AABB);

			if (Math::instersectFrustumVsAABB(vFrustum, vBox) != COLLISION_TYPE::DISJOINT)
			{
				stack[++currStackIdx] = node.RightChild;
				stack[++currStackIdx] = currNode + 1;
			}
		}
	}
	
	/*
	for (size_t i = 0; i < m_instances.size(); i++)
	{
		vBox.Reset(m_instances[i].AABB);

		if (m_instances[i].ID == 3326297391030289423)
		{
			auto res2 = instersectFrustumVsAABB(vFrustum, vBox);
			printf("dfgdfg");
		}

		if (Math::instersectFrustumVsAABB(vFrustum, vBox) != COLLISION_TYPE::DISJOINT)
			visibleModelIDs.push_back(m_instances[i].ID);
	}
	*/
}

void BVH::DoFrustumCulling(const Math::ViewFrustum& viewFrustum,
	const Math::float4x4a& viewToWorld,
	Vector<BVHInput, App::FrameAllocator>& visibleInstanceIDs)
{
	// transform view frustum from view space into world space
	v_float4x4 vM(const_cast<float4x4a&>(viewToWorld));
	v_ViewFrustum vFrustum(const_cast<ViewFrustum&>(viewFrustum));
	vFrustum = Math::transform(vM, vFrustum);

	v_AABB vBox(m_nodes[0].AABB);

	// root doesn't intersect the camera
	if (Math::instersectFrustumVsAABB(vFrustum, vBox) == COLLISION_TYPE::DISJOINT)
		return;

	// manual stack
	constexpr int STACK_SIZE = 64;
	int stack[STACK_SIZE];
	int currStackIdx = 0;
	stack[currStackIdx] = 0;		// insert root
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
				vBox.Reset(m_instances[i].AABB);

				if (Math::instersectFrustumVsAABB(vFrustum, vBox) != COLLISION_TYPE::DISJOINT)
				{
					visibleInstanceIDs.emplace_back(BVH::BVHInput{
						.AABB = m_instances[i].AABB,
						.ID = m_instances[i].ID });
				}
			}
		}
		else
		{
			vBox.Reset(node.AABB);

			if (Math::instersectFrustumVsAABB(vFrustum, vBox) != COLLISION_TYPE::DISJOINT)
			{
				stack[++currStackIdx] = node.RightChild;
				stack[++currStackIdx] = currNode + 1;
			}
		}
	}
}

uint64_t BVH::CastRay(Math::Ray& r) noexcept
{
	v_AABB vBox(m_nodes[0].AABB);
	v_Ray vRay(r);
	float t;

	const __m128 vIsParallel = _mm_cmpge_ps(_mm_set1_ps(FLT_EPSILON), abs(vRay.vDir));
 	const __m128 vDirRcp = _mm_div_ps(_mm_set1_ps(1.0f), vRay.vDir);
	const __m128 vDirIsPos = _mm_cmpge_ps(vRay.vDir, _mm_setzero_ps());

	// can return early if root doesn't intersect the worl AABB
	if (!Math::intersectRayVsAABB(vRay, vDirRcp, vDirIsPos, vIsParallel, vBox, t))
		return uint64_t(-1);

	// manual stack
	constexpr int STACK_SIZE = 64;
	int stack[STACK_SIZE];
	int currStackIdx = 0;
	stack[currStackIdx] = 0;	// insert root
	int currNode = -1;
	float minT = FLT_MAX;
	uint64_t closestID = uint64_t(-1);

	while (currStackIdx >= 0)
	{
		Assert(currStackIdx < STACK_SIZE, "Stack size exceeded 64.");

		currNode = stack[currStackIdx--];
		const Node& node = m_nodes[currNode];

		if (node.IsLeaf())
		{
			for (int i = node.Base; i < node.Base + node.Count; i++)
			{
				vBox.Reset(m_instances[i].AABB);

				if (Math::intersectRayVsAABB(vRay, vDirRcp, vDirIsPos, vIsParallel, vBox, t))
				{
					const bool tLtTmin = t < minT;
					minT = tLtTmin ? t : minT;
					closestID = tLtTmin ? m_instances[i].ID : closestID;
				}
			}
		}
		else
		{
			const Node& leftChild = m_nodes[currNode + 1];
			const Node& rightChild = m_nodes[node.RightChild];
			const v_AABB vLeftBox(leftChild.AABB);
			const v_AABB vRightBox(rightChild.AABB);
			float leftT;
			float rightT;

			const bool hitLeftChild = Math::intersectRayVsAABB(vRay, vDirRcp, vDirIsPos, vIsParallel, vLeftBox, leftT);
			const bool hitRightChild = Math::intersectRayVsAABB(vRay, vDirRcp, vDirIsPos, vIsParallel, vRightBox, rightT);

			int sortedByT[2] = { currNode + 1, node.RightChild };
			const int numChilds = hitLeftChild + hitRightChild;

			// make sure subtree closer to camera is searched first
			if (numChilds == 2 && leftT < rightT)
				std::swap(sortedByT[0], sortedByT[1]);

			for (int c = 0; c < numChilds; c++)
			{
				// no need to search this subtree as earlier hits are necessarily closer to camera
				if(sortedByT[c] < minT)
					stack[++currStackIdx] = sortedByT[c];
			}
		}
	}

	return closestID;
}


