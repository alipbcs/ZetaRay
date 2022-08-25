#include "SceneCore.h"
#include "../Core/Renderer.h"
#include "../Win32/App.h"
#include "../Model/Mesh.h"
#include "../Math/CollisionFuncs.h"
#include "../Math/Quaternion.h"
#include "../RenderPass/Common/RtCommon.h"
#include "../Win32/Filesystem.h"
#include "../SupportSystem/Task.h"
#include <algorithm>

using namespace ZetaRay::Scene;
using namespace ZetaRay::Scene::Internal;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;
using namespace ZetaRay::Win32;

namespace
{
	inline uint64_t MeshID(uint64_t sceneID, int meshIdx, int meshPrimIdx) noexcept
	{
		StackStr(str, n, "mesh_%llu_%d_%d", sceneID, meshIdx, meshPrimIdx);
		uint64_t meshFromSceneID = XXH3_64bits(str, n);

		return meshFromSceneID;
	}

	inline uint64_t MaterialID(uint64_t sceneID, int materialIdx) noexcept
	{
		StackStr(str, n, "mat_%llu_%d", sceneID, materialIdx);
		uint64_t matFromSceneID = XXH3_64bits(str, n);

		return matFromSceneID;
	}
}

//--------------------------------------------------------------------------------------
// Scene
//--------------------------------------------------------------------------------------

SceneCore::SceneCore() noexcept
	: k_matBuffID(XXH3_64bits(SceneRenderer::MATERIAL_BUFFER, strlen(SceneRenderer::MATERIAL_BUFFER))),
	k_metallicRoughnessID(XXH3_64bits(SceneRenderer::METALNESS_ROUGHNESS_DESCRIPTOR_TABLE, 
		strlen(SceneRenderer::METALNESS_ROUGHNESS_DESCRIPTOR_TABLE))),
	k_baseColorID(XXH3_64bits(SceneRenderer::BASE_COLOR_DESCRIPTOR_TABLE, 
		strlen(SceneRenderer::BASE_COLOR_DESCRIPTOR_TABLE))),
	k_normalID(XXH3_64bits(SceneRenderer::NORMAL_DESCRIPTOR_TABLE, strlen(SceneRenderer::NORMAL_DESCRIPTOR_TABLE))),
	k_emissiveID(XXH3_64bits(SceneRenderer::EMISSIVE_DESCRIPTOR_TABLE, strlen(SceneRenderer::EMISSIVE_DESCRIPTOR_TABLE))),
	m_matBuffer(k_matBuffID),
	m_baseColorDescTable(k_baseColorID),
	m_normalsDescTable(k_normalID),
	m_metalnessRoughnessDescTable(k_metallicRoughnessID),
	m_emissiveDescTable(k_emissiveID)
{
}

void SceneCore::Init() noexcept
{
	//m_camera.Init(float3(-10.61f, 4.67f, -3.25f), App::GetRenderer().GetAspectRatio(), 
//	Math::DegreeToRadians(85.0f), 0.1f, 10000.0f, true);
	m_camera.Init(float3(-5.61f, 4.67f, -0.25f), App::GetRenderer().GetAspectRatio(),
		Math::DegreeToRadians(85.0f), 0.1f, 10000.0f, true);
	//m_camera.Init(float3(-1127.61f, 348.67f, 66.25f), App::GetRenderer().GetAspectRatio(), 
	//	Math::DegreeToRadians(85.0f), 10.0f, 10000.0f, true);
	//m_camera.Init(float3(0.61f, 3.67f, 0.25f), App::GetRenderer().GetAspectRatio(), Math::DegreeToRadians(85.0f), 0.1f);

	// level-0 is just a (dummy) root
	m_sceneGraph.resize(2);
	m_sceneGraph[0].m_toWorlds.resize(1);
	m_sceneGraph[0].m_subtreeRanges.resize(1);
	m_sceneGraph[0].m_subtreeRanges[0] = Range(0, 0);

	v_float4x4 I = identity();
	m_sceneGraph[0].m_toWorlds[0] = float4x3(store(I));

	m_matBuffer.Init();
	m_baseColorDescTable.Init();
	m_normalsDescTable.Init();
	m_metalnessRoughnessDescTable.Init();
	m_emissiveDescTable.Init();

	auto* device = App::GetRenderer().GetDevice();
	CheckHR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));

	m_sceneRenderer.Init();
}

void SceneCore::OnWindowSizeChanged() noexcept
{
	//m_camera.OnWindowSizeChanged();
	m_sceneRenderer.OnWindowSizeChanged();
}

void SceneCore::Update(double dt, TaskSet& sceneTS, TaskSet& sceneRendererTS) noexcept
{
	if (m_isPaused)
		return;

	TaskSet::TaskHandle h0 = sceneTS.EmplaceTask("Scene::Update", [this, dt]()
		{
			SmallVector<AnimationUpdateOut> animUpdates;
			UpdateAnimations((float)dt, animUpdates);
			UpdateLocalTransforms(ZetaMove(animUpdates));

			SmallVector<BVH::BVHUpdateInput> toUpdateInstances;
			UpdateWorldTransformations(toUpdateInstances);

			if (m_rebuildBVHFlag)
			{
				RebuildBVH();
				m_rebuildBVHFlag = false;
			}
			else
				m_bvh.Update(ZetaMove(toUpdateInstances));

			m_frameInstances.clear();
			m_frameInstances.reserve(m_IDtoTreePos.size());

			m_camera.Update();

			m_bvh.DoFrustumCulling(m_camera.GetCameraFrustumViewSpace(), m_camera.GetViewInv(), m_frameInstances);

			//App::AddFrameStat("Scene", "#Instances", m_IDtoTreePos.size());
			//App::AddFrameStat("Scene", "#FrustumCulled", m_IDtoTreePos.size() - m_frameInstances.size());
			App::AddFrameStat("Scene", "FrustumCulled", (uint32_t)(m_IDtoTreePos.size() - m_frameInstances.size()), (uint32_t)m_IDtoTreePos.size());
		});

	m_sceneRenderer.Update(sceneRendererTS);
}

void SceneCore::Recycle() noexcept
{
	if (m_baseColorDescTable.Pending.empty() &&
		m_normalsDescTable.Pending.empty() &&
		m_metalnessRoughnessDescTable.Pending.empty() &&
		m_matBuffer.Pending.empty())
		return;

	App::GetRenderer().SignalDirectQueue(m_fence.Get(), m_nextFenceVal);

	ID3D12Fence* fence = m_fence.Get();
	m_baseColorDescTable.Recycle(fence);
	m_normalsDescTable.Recycle(fence);
	m_metalnessRoughnessDescTable.Recycle(fence);
	m_matBuffer.Recycle(fence);

	// guarding mesh destruction is handled by the DefaultHeapBuffer

	m_nextFenceVal++;
}

void SceneCore::Shutdown() noexcept
{
	ID3D12Fence* fence = m_fence.Get();
	App::GetRenderer().SignalDirectQueue(fence, m_nextFenceVal);

	if (fence->GetCompletedValue() < m_nextFenceVal)
	{
		HANDLE handle = CreateEvent(nullptr, false, false, "");
		AssertWin32(handle);

		CheckHR(fence->SetEventOnCompletion(1, handle));
		WaitForSingleObject(handle, INFINITE);

		CloseHandle(handle);
	}

	m_matBuffer.Clear();
	m_baseColorDescTable.Clear();
	m_normalsDescTable.Clear();
	m_metalnessRoughnessDescTable.Clear();
	m_emissiveDescTable.Clear();
	m_meshManager.Clear();
	m_bvh.Clear();

	m_baseColTableOffsetToID.free();
	m_normalTableOffsetToID.free();
	m_metalnessRougnessrTableOffsetToID.free();
	m_emissiveTableOffsetToID.free();

	m_frameInstances.free();
	m_prevToWorlds.free();
	m_sceneMetadata.free();
	m_sceneGraph.free();
	m_IDtoTreePos.free();

	m_sceneRenderer.Shutdown();
}

AABB SceneCore::GetWorldAABB() noexcept
{
	// BVH hasn't been built yet
	// TODO replace with something less hacky
	if (!m_bvh.IsBuilt())
		return Math::AABB(Math::float3(0.0f, 0.0f, 0.0f), Math::float3(2000.0f, 2000.0f, 2000.0f));

	return m_bvh.GetWorldAABB();
}

void SceneCore::ReserveScene(uint64_t sceneID, size_t numMeshes, size_t numMats, size_t numNodes) noexcept
{
	auto& it = m_sceneMetadata[sceneID];
	it.MaterialIDs.reserve(numMats);
	it.Meshes.reserve(numMeshes);
	it.Instances.reserve(numNodes);
}

void SceneCore::AddMesh(uint64_t sceneID, Asset::MeshSubset&& mesh) noexcept
{
	const uint64_t meshFromSceneID = MeshID(sceneID, mesh.MeshIdx, mesh.MeshPrimIdx);
	const uint64_t matFromSceneID = MaterialID(sceneID, mesh.MaterialIdx);

	//std::unique_lock<std::shared_mutex> lock(m_meshMtx);

	AcquireSRWLockExclusive(&m_meshLock);
	// remember from which gltf scene this mesh came from
	m_sceneMetadata[sceneID].Meshes.push_back(meshFromSceneID);
	m_meshManager.Add(meshFromSceneID, ZetaMove(mesh.Vertices), ZetaMove(mesh.Indices), matFromSceneID);
	ReleaseSRWLockExclusive(&m_meshLock);
}

void SceneCore::AddMaterial(uint64_t sceneID, Asset::MaterialDesc&& matDesc) noexcept
{
	Assert(matDesc.Index >= 0, "invalid material index.");
	const uint64_t matFromSceneID = MaterialID(sceneID, matDesc.Index);

	Material mat;
	mat.BaseColorFactor = matDesc.BaseColorFactor;
	mat.EmissiveFactor = matDesc.EmissiveFactor;
	mat.AlphaCuttoff = matDesc.AlphaCuttoff;
	mat.MetallicFactor = matDesc.MetallicFactor;
	mat.NormalScale = matDesc.NormalScale;
	mat.RoughnessFactor = matDesc.RoughnessFactor;
	mat.SetAlphaMode(matDesc.AlphaMode);
	mat.SetTwoSided(matDesc.TwoSided);

	auto addTex = [](const Filesystem::Path& p, TexSRVDescriptorTable& table, uint32_t& tableOffset) noexcept -> uint64_t
	{
		if (!p.IsEmpty())
		{
			//auto* str = const_cast<std::filesystem::path::value_type*>(p.c_str());
			const uint64_t id = XXH3_64bits(p.Get(), strlen(p.Get()));

			uint32_t descTableIdx = table.Add(p, id);
			tableOffset = descTableIdx;

			return id;
		}

		return -1;
	};

	//std::unique_lock<std::shared_mutex> lock(m_matMtx);
	AcquireSRWLockExclusive(&m_matLock);

	// load the textures (if needed), create corresponding descriptors and add them
	// to their descriptor tables
	{
		uint32_t tableOffset = -1;	// i.e. index in GPU descriptor table
		uint64_t texID = addTex(matDesc.BaseColorTexPath, m_baseColorDescTable, tableOffset);
		if (texID != -1)
		{
			m_baseColTableOffsetToID[tableOffset] = texID;
			mat.BaseColorTexture = tableOffset;
		}
	}

	{
		uint32_t tableOffset = -1;
		uint64_t texID = addTex(matDesc.NormalTexPath, m_normalsDescTable, tableOffset);
		if (texID != -1)
		{
			m_normalTableOffsetToID[tableOffset] = texID;
			mat.NormalTexture = tableOffset;
		}
	}

	{
		uint32_t tableOffset = -1;
		uint64_t texID = addTex(matDesc.MetalnessRoughnessTexPath, m_metalnessRoughnessDescTable, tableOffset);
		if (texID != -1)
		{
			m_metalnessRougnessrTableOffsetToID[tableOffset] = texID;
			mat.MetallicRoughnessTexture = tableOffset;
		}
	}

	{
		uint32_t tableOffset = -1;
		uint64_t texID = addTex(matDesc.EmissiveTexPath, m_emissiveDescTable, tableOffset);
		if (texID != -1)
		{
			m_emissiveTableOffsetToID[tableOffset] = texID;
			mat.EmissiveTexture = tableOffset;
		}
	}

	// add it to the GPU buffer
	m_matBuffer.Add(matFromSceneID, mat);

	// remember from which gltf scene this material came from
	m_sceneMetadata[sceneID].MaterialIDs.push_back(matFromSceneID);

	ReleaseSRWLockExclusive(&m_matLock);
}

SceneCore::TreePos* SceneCore::FindTreePosFromID(uint64_t id) noexcept
{
	TreePos* treePos = m_IDtoTreePos.find(id);
	return treePos;
}

void SceneCore::AddInstance(uint64_t sceneID, Asset::InstanceDesc&& instance) noexcept
{
	const uint64_t meshID = MeshID(sceneID, instance.MeshIdx, instance.MeshPrimIdx);
	const uint64_t instanceID = InstanceID(sceneID, instance.Name, instance.MeshIdx, instance.MeshPrimIdx);

	//std::unique_lock<std::shared_mutex> lock(m_instanceMtx);
	AcquireSRWLockExclusive(&m_instanceLock);

	m_sceneMetadata[sceneID].Instances.push_back(instanceID);

	if (instance.RtMeshMode == RT_MESH_MODE::STATIC)
	{
		m_numStaticInstances++;
		m_staleStaticInstances = true;
	}
	else
	{
		m_numDynamicInstances++;
	}
	
	int treeLevel = 1;
	int parentIdx = 0;
	
	// get parent's index from the hashmap
	if (instance.ParentID != ROOT_ID)
	{
		TreePos* p = FindTreePosFromID(instance.ParentID);
		Assert(p, "instance with ID %llu was not found in the scene-graph.", instance.ParentID);

		treeLevel = p->Level + 1;
		parentIdx = p->Offset;
	}
	
	const int insertIdx = InsertAtLevel(instanceID, treeLevel, parentIdx, instance.LocalTransform, meshID,
		instance.RtMeshMode, instance.RtInstanceMask);

	// update the instance "dictionary"
	{
		Assert(m_IDtoTreePos.find(instanceID) == nullptr, "instance with id %llu already existed.", instanceID);
		m_IDtoTreePos.emplace_or_assign(instanceID, TreePos{ .Level = treeLevel, .Offset = insertIdx });

		// adjust the tree-positions of shifted instances
		for(int i = insertIdx + 1; i < m_sceneGraph[treeLevel].m_IDs.size(); i++)
		{
			uint64_t insID = m_sceneGraph[treeLevel].m_IDs[i];
			TreePos* p = m_IDtoTreePos.find(insID);
			Assert(p, "instance with ID %llu was not found in the scene-graph.", insID);

			// shift the tree poistion to right
			p->Offset++;
		}
	}

	/*
	// cache the meshe IDs that are emissive
	if (instance.RtInstanceMask & RT_AS_SUBGROUP::EMISSIVE)
	{
		Assert(!instance.Lumen.empty(), "Emissive instances require precomputed per-triangle power");
		m_sceneRenderer.AddEmissiveInstance(instanceID, ZetaMove(instance.Lumen));
	}
	*/

	m_rebuildBVHFlag = true;

	ReleaseSRWLockExclusive(&m_instanceLock);
}

int SceneCore::InsertAtLevel(uint64_t id, int treeLevel, int parentIdx, float4x3& localTransform,
	uint64_t meshID, RT_MESH_MODE rtMeshMode, uint8_t rtInstanceMask) noexcept
{
	auto& parentLevel = m_sceneGraph[treeLevel - 1];
	auto& currLevel = m_sceneGraph[treeLevel];
	auto& parentRange = parentLevel.m_subtreeRanges[parentIdx];

	// position to insert at is right next to the parent's rightmost child
	const int insertIdx = parentRange.Base + parentRange.Count;

	// increment parent's children count
	parentRange.Count++;

	// append it to the end, then keep swapping it back until it's at insertIdx
	auto rearrange = []<typename T, typename... Args> requires std::is_swappable<T>::value
	(Vector<T>& vec, int insertIdx, Args&&... args) noexcept
	{
		vec.emplace_back(T(ZetaForward(args)...));
		for (int i = (int)vec.size() - 1; i != insertIdx; --i)
			std::swap(vec[i], vec[i - 1]);
	};

	//float4a s;
	//float4a q;
	//float4a t;
	//decomposeAffine(v_float4x4(localTransform), s, q, t);
	//AffineTransformation affine{ 
	//	.Scale = float3(s.x, s.y, s.z), 
	//	.RotQuat = float4(q.x, q.y, q.z, q.w), 
	//	.Translation = float3(t.x, t.y, t.z)};

	// TODO check whether m_toWorlds needs to be included
	float4x3 I = float4x3(store(identity()));

	rearrange(currLevel.m_IDs, insertIdx, id);
	rearrange(currLevel.m_localTransforms, insertIdx, localTransform);
	rearrange(currLevel.m_toWorlds, insertIdx, I);
	rearrange(currLevel.m_meshIDs, insertIdx, meshID);
	rearrange(currLevel.m_subtreeRanges, insertIdx, 0, 0);
	rearrange(currLevel.m_parendIndices, insertIdx, parentIdx);
	// set rebuild flag to true for any instance that is added for the first time
	rearrange(currLevel.m_rtFlags, insertIdx, SetRtFlags(rtMeshMode, rtInstanceMask, 1, 0));

	// shift base offset of all the parent's right-siblings to right by one
	for (int siblingIdx = parentIdx + 1; siblingIdx != parentLevel.m_subtreeRanges.size(); siblingIdx++)
		parentLevel.m_subtreeRanges[siblingIdx].Base++;

	return insertIdx;
}

void SceneCore::AddAnimation(uint64_t id, Vector<Keyframe>&& keyframes, float tOffset, bool isSorted) noexcept
{
#ifdef _DEBUG
	TreePos *p = FindTreePosFromID(id);
	Assert(p, "instance with ID %llu was not found in the scene-graph.", id);
	Assert(GetRtFlags(m_sceneGraph[p->Level].m_rtFlags[p->Offset]).MeshMode != RT_MESH_MODE::STATIC, "Static instance can't be animated.");
#endif // _DEBUG

	Check(keyframes.size() > 1, "Invalid animation");

	if (!isSorted)
	{
		std::sort(m_keyframes.begin(), m_keyframes.end(),
			[](const Keyframe& k1, const Keyframe& k2)
			{
				return k1.Time < k2.Time;
			});
	}

	const uint32_t currOffset = (uint32_t)m_keyframes.size();
	m_animationOffsets.emplace_back(currOffset, currOffset + (int)keyframes.size(), tOffset);

	// insertion sort
	m_animOffsetToInstanceMap.emplace_back(id, currOffset);
	int currIdx = std::max(0, (int)m_animOffsetToInstanceMap.size() - 2);
	while (currIdx >= 0 && id < m_animOffsetToInstanceMap[currIdx].Offset)
	{
		std::swap(m_animOffsetToInstanceMap[currIdx], m_animOffsetToInstanceMap[currIdx + 1]);
		currIdx--;
	}

	// append
	m_keyframes.append_range(keyframes.begin(), keyframes.end());
}

void SceneCore::AddEnvLightSource(const Filesystem::Path& p) noexcept
{
	// HDRi
	Filesystem::Path pathToEnvMap(App::GetAssetDir());
	pathToEnvMap.Append(p.Get());

	// patches
	char name[64];
	p.Stem(name);
	const size_t nStem = strlen(name);
	const char* envSuffix = "_patches.bin";
	const size_t nSuffix = strlen(envSuffix);
	Check(nStem + nSuffix + 1 < sizeof(name), "buffer is too small");

	memcpy(name + strlen(name), envSuffix, nSuffix);
	name[nStem + nSuffix] = '\0';

	Filesystem::Path pathToPatches = pathToEnvMap;
	pathToPatches.ToParent();
	pathToPatches.Append(name);

	m_sceneRenderer.SetEnvLightSource(pathToEnvMap, pathToPatches);
}

float4x3 SceneCore::GetToWorld(uint64_t id) noexcept
{
	TreePos* p = FindTreePosFromID(id);
	Assert(p, "instance with ID %llu was not found in the scene-graph.", id);

	return m_sceneGraph[p->Level].m_toWorlds[p->Offset];
}

float4x3 SceneCore::GetPrevToWorld(uint64_t key) noexcept
{
	int beg = 0;
	int end = (int)m_prevToWorlds.size();
	int mid = (int)m_prevToWorlds.size() >> 1;

	while (true)
	{
		if (end - beg <= 2)
			break;

		if (m_prevToWorlds[mid].ID < key)
			beg = mid + 1;
		else
			end = mid + 1;

		mid = beg + ((end - beg) >> 1);
	}

	if (m_prevToWorlds[beg].ID == key)
		return m_prevToWorlds[beg].W;
	else if (m_prevToWorlds[mid].ID == key)
		return m_prevToWorlds[mid].W;

	return float4x3(store(identity()));
}

uint64_t SceneCore::GetMeshIDForInstance(uint64_t id) noexcept
{
	TreePos* p = FindTreePosFromID(id);
	Assert(p, "instance with ID %llu was not found in the scene-graph.", id);

	return m_sceneGraph[p->Level].m_meshIDs[p->Offset];
}

void SceneCore::RebuildBVH() noexcept
{
	SmallVector<BVH::BVHInput> allInstances;
	allInstances.reserve(m_IDtoTreePos.size());

	const int numLevels = (int)m_sceneGraph.size();

	for (int level = 1; level < numLevels; ++level)
	{
		for (int i = 0; i < m_sceneGraph[level].m_toWorlds.size(); ++i)
		{
			// find the Mesh of this instance
			const uint64_t meshID = m_sceneGraph[level].m_meshIDs[i];
			v_AABB vBox(m_meshManager.GetMeshAABB(meshID));
			v_float4x4 vM(m_sceneGraph[level].m_toWorlds[i]);

			vBox = transform(vM, vBox);

			allInstances.emplace_back(BVH::BVHInput{ .AABB = store(vBox), .ID = m_sceneGraph[level].m_IDs[i] });
		}
	}

	m_bvh.Build(ZetaMove(allInstances));
}

void SceneCore::UpdateWorldTransformations(Vector<BVH::BVHUpdateInput>& toUpdateInstances) noexcept
{
	m_prevToWorlds.clear();
	const int numLevels = (int)m_sceneGraph.size();

	for (int level = 0; level < numLevels - 1; ++level)
	{
		for (int i = 0; i < m_sceneGraph[level].m_subtreeRanges.size(); i++)
		{
			v_float4x4 vParentTransform(m_sceneGraph[level].m_toWorlds[i]);
			const auto& range = m_sceneGraph[level].m_subtreeRanges[i];

			for (int j = range.Base; j < range.Base + range.Count; j++)
			{
				//AffineTransformation& aff = m_sceneGraph[level + 1].m_localTransforms[j];

				//v_float4x4 vLocal = affineTransformation(float4a(aff.Scale), float4a(aff.RotQuat), float4a(aff.Translation));
				//v_float4x4 newW = mul(vParentTransform, vLocal);
				
				v_float4x4 vLocal(m_sceneGraph[level + 1].m_localTransforms[j]);
				v_float4x4 newW = mul(vParentTransform, vLocal);
				v_float4x4 prevW(m_sceneGraph[level + 1].m_toWorlds[j]);

				if (!m_rebuildBVHFlag && !equal(newW, prevW))
				{
					const uint64_t meshID = m_sceneGraph[level + 1].m_meshIDs[j];

					v_AABB vOldBox(m_meshManager.GetMeshAABB(meshID));
					vOldBox = transform(prevW, vOldBox);
					v_AABB vNewBox = transform(newW, vOldBox);

					toUpdateInstances.emplace_back(BVH::BVHUpdateInput{ 
						.OldBox = store(vOldBox),
						.NewBox = store(vNewBox),
						.ID = m_sceneGraph[level + 1].m_IDs[j] });

					RT_Flags f = GetRtFlags(m_sceneGraph[level + 1].m_rtFlags[j]);
					Assert(f.MeshMode != RT_MESH_MODE::STATIC, "Transformation of static meshes can't change");
					Assert(!f.RebuildFlag, "Rebuild & update flags can't be set at the same time.");

					m_sceneGraph[level + 1].m_rtFlags[j] = SetRtFlags(f.MeshMode, f.InstanceMask, 0, 1);
				}

				m_prevToWorlds.emplace_back(PrevToWorld{ 
					.W = m_sceneGraph[level + 1].m_toWorlds[j], 
					.ID = m_sceneGraph[level + 1].m_IDs[j] });

				m_sceneGraph[level + 1].m_toWorlds[j] = float4x3(store(newW));
			}
		}
	}

	std::sort(m_prevToWorlds.begin(), m_prevToWorlds.end(), 
		[](const PrevToWorld& lhs, const PrevToWorld& rhs)
		{
			return lhs.ID < rhs.ID;
		});
}

void SceneCore::UpdateAnimations(float t, Vector<AnimationUpdateOut>& animVec) noexcept
{
	for (int i = 0; i < m_animationOffsets.size(); i++)
	{
		v_float4x4 vRes;

		// interpolate
		const Keyframe& kStart = m_keyframes[m_animationOffsets[i].BegOffset];
		const Keyframe& kEnd = m_keyframes[m_animationOffsets[i].EndOffset - 1];
		const float startOffset = m_animationOffsets[i].BegTimeOffset;

		// fast path
		if (t <= kStart.Time + startOffset)
		{
			vRes = affineTransformation(float4a(kStart.Transform.Scale),
				float4a(kStart.Transform.RotQuat),
				float4a(kStart.Transform.Translation));
		}
		else if (t >= kEnd.Time + startOffset)
		{
			vRes = affineTransformation(float4a(kEnd.Transform.Scale),
				float4a(kEnd.Transform.RotQuat),
				float4a(kEnd.Transform.Translation));
		}
		else
		{
			// binary search
			int beg = m_animationOffsets[i].BegOffset;
			int end = (int)m_animationOffsets[i].EndOffset;
			int mid = (end - beg) >> 1;

			while (true)
			{
				if (end - beg <= 2)
					break;

				if (m_keyframes[mid].Time + startOffset < t)
					beg = mid + 1;
				else
					end = mid + 1;

				mid = beg + ((end - beg) >> 1);
			}

			auto& k1 = m_keyframes[beg];
			auto& k2 = m_keyframes[mid];

			Assert(t >= k1.Time + startOffset && t <= k2.Time + startOffset, "bug");
			Assert(k1.Time < k2.Time, "divide-by-zero");

			float interpolatedT = (t - (k1.Time + startOffset)) / (k2.Time - k1.Time);

			// scale
			float4a temp1 = float4a(k1.Transform.Scale);
			float4a temp2 = float4a(k2.Transform.Scale);
			const __m128 vScale1 = _mm_load_ps(reinterpret_cast<float*>(&temp1));
			const __m128 vScale2 = _mm_load_ps(reinterpret_cast<float*>(&temp2));
			const __m128 vScaleInt = lerp(vScale1, vScale2, interpolatedT);

			// translation
			float4a temp3 = float4a(k1.Transform.Translation);
			float4a temp4 = float4a(k2.Transform.Translation);
			const __m128 vTranlate1 = _mm_load_ps(reinterpret_cast<float*>(&temp3));
			const __m128 vTranlate2 = _mm_load_ps(reinterpret_cast<float*>(&temp4));
			const __m128 vTranslateInt = lerp(vTranlate1, vTranlate2, interpolatedT);

			// rotation
			float4a temp5 = float4a(k1.Transform.RotQuat);
			float4a temp6 = float4a(k2.Transform.RotQuat);
			const __m128 vRot1 = _mm_load_ps(reinterpret_cast<float*>(&temp5));
			const __m128 vRot2 = _mm_load_ps(reinterpret_cast<float*>(&temp6));
			const __m128 vRotInt = slerp(vRot1, vRot2, interpolatedT);

			vRes = affineTransformation(vScaleInt, vRotInt, vTranslateInt);
		}

		animVec.emplace_back(AnimationUpdateOut{ .M = (float4x3)store(vRes), .Offset = m_animationOffsets[i].BegOffset });
	}
}

void SceneCore::UpdateLocalTransforms(Vector<AnimationUpdateOut>&& animVec) noexcept
{
	for(auto& update : animVec)
	{
		uint64_t insID;

		{
			int key = update.Offset;
			int beg = 0;
			int end = (int)m_animOffsetToInstanceMap.size();
			int mid = end >> 1;

			while (true)
			{
				if (end - beg <= 2)
					break;

				if (m_animOffsetToInstanceMap[mid].Offset < key)
					beg = mid + 1;
				else
					end = mid + 1;

				mid = beg + ((end - beg) >> 1);
			}

			if (m_animOffsetToInstanceMap[beg].Offset == key)
				insID = m_animOffsetToInstanceMap[beg].InstanceID;
			else if (m_animOffsetToInstanceMap[mid].Offset == key)
				insID = m_animOffsetToInstanceMap[mid].InstanceID;
			else
				Assert(false, "Instance ID for current animation was not found.");
		}

		TreePos* t = FindTreePosFromID(insID);
		Assert(t, "instance with ID %llu was not found in the scene-graph.", insID);

		m_sceneGraph[t->Level].m_localTransforms[t->Offset] = update.M;
	}
}

void SceneCore::RemoveFromLevel(int index, int level) noexcept
{
	// 1. Subtract one from parent's number of children
	// 2. Shift base index of all the parent's right siblings left
	// 3. Shift parent index of all the swapped models left

	const int parentIdx = m_sceneGraph[level].m_parendIndices[index];
	m_sceneGraph[level - 1].m_subtreeRanges[parentIdx].Count--;

	m_sceneGraph[level].m_localTransforms.erase(index);
	m_sceneGraph[level].m_toWorlds.erase(index);
	m_sceneGraph[level].m_meshIDs.erase(index);
	m_sceneGraph[level].m_subtreeRanges.erase(index);
	m_sceneGraph[level].m_parendIndices.erase(index);

	// shift base offset of all the parent's siblings to left
	for (int siblingIdx = parentIdx + 1; siblingIdx != m_sceneGraph[level - 1].m_subtreeRanges.size(); ++siblingIdx)
	{
		auto& range = m_sceneGraph[level - 1].m_subtreeRanges[siblingIdx];
		range.Base = std::max(0, range.Base - 1);
	}
}
