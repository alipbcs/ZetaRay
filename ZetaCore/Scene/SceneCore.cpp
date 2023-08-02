#include "SceneCore.h"
#include "../Math/CollisionFuncs.h"
#include "../Math/Quaternion.h"
#include "../Math/Color.h"
#include "../RayTracing/RtCommon.h"
#include "../Support/Task.h"
#include "../Core/RendererCore.h"
#include "Camera.h"
#include "../Utility/Utility.h"
#include <algorithm>

using namespace ZetaRay::Scene;
using namespace ZetaRay::Scene::Internal;
using namespace ZetaRay::Model;
using namespace ZetaRay::Model::glTF::Asset;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;
using namespace ZetaRay::App;

namespace
{
	struct uint3
	{
		uint3() = default;
		uint3(uint32_t e0, uint32_t e1, uint32_t e2)
			: x(e0), y(e1), z(e2)
		{}

		uint32_t x;
		uint32_t y;
		uint32_t z;
	};

	uint3 operator+(uint3 v, uint32_t m)
	{
		return uint3(v.x + m, v.y + m, v.z + m);
	}

	uint3 operator*(uint3 v, uint32_t m)
	{
		return uint3(v.x * m, v.y * m, v.z * m);
	}

	uint3 operator>>(uint3 v, uint32_t m)
	{
		return uint3(v.x >> m, v.y >> m, v.z >> m);
	}

	uint3 operator^(uint3 v, uint3 m)
	{
		return uint3(v.x ^ m.x, v.y ^ m.y, v.z ^ m.z);
	}

	static uint3 Pcg3d(uint3 v)
	{
		v = v * 1664525u + 1013904223u;
		v.x += v.y * v.z;
		v.y += v.z * v.x;
		v.z += v.x * v.y;
		v = v ^ (v >> 16u);
		v.x += v.y * v.z;
		v.y += v.z * v.x;
		v.z += v.x * v.y;
		return v;
	}
}

//--------------------------------------------------------------------------------------
// Scene
//--------------------------------------------------------------------------------------

SceneCore::SceneCore() noexcept
	: m_baseColorDescTable(BASE_COLOR_DESC_TABLE_SIZE),
	m_normalDescTable(NORMAL_DESC_TABLE_SIZE),
	m_metallicRoughnessDescTable(METALLIC_ROUGHNESS_DESC_TABLE_SIZE),
	m_emissiveDescTable(EMISSIVE_DESC_TABLE_SIZE),
	m_sceneGraph(m_memoryPool, m_memoryPool),
	m_prevToWorlds(m_memoryPool),
	m_animOffsetToInstanceMap(m_memoryPool),
	m_animationOffsets(m_memoryPool),
	m_keyframes(m_memoryPool)
{
}

void SceneCore::Init(Renderer::Interface& rendererInterface) noexcept
{
	m_rendererInterface = rendererInterface;
	Assert(m_rendererInterface.Init, "Init() was null.");
	Assert(m_rendererInterface.Update, "Update() was null.");
	Assert(m_rendererInterface.Render, "Render() was null.");
	Assert(m_rendererInterface.Shutdown, "Shutdown() was null.");
	Assert(m_rendererInterface.OnWindowSizeChanged, "OnWindowSizeChanged() was null.");
	Assert(m_rendererInterface.DebugDrawRenderGraph, "DebugDrawRenderGraph() was null.");

	// level 0 is just a (dummy) root
	m_sceneGraph.reserve(2);

	m_sceneGraph.emplace_back(m_memoryPool);
	m_sceneGraph.emplace_back(m_memoryPool);

	m_sceneGraph[0].m_toWorlds.resize(1);
	m_sceneGraph[0].m_subtreeRanges.resize(1);
	m_sceneGraph[0].m_subtreeRanges[0] = Range(0, 0);

	v_float4x4 I = identity();
	m_sceneGraph[0].m_toWorlds[0] = float4x3(store(I));

	m_matBuffer.Init(XXH3_64bits(GlobalResource::MATERIAL_BUFFER, strlen(GlobalResource::MATERIAL_BUFFER)));
	m_baseColorDescTable.Init(XXH3_64bits(GlobalResource::BASE_COLOR_DESCRIPTOR_TABLE,
		strlen(GlobalResource::BASE_COLOR_DESCRIPTOR_TABLE)));
	m_normalDescTable.Init(XXH3_64bits(GlobalResource::NORMAL_DESCRIPTOR_TABLE, strlen(GlobalResource::NORMAL_DESCRIPTOR_TABLE)));
	m_metallicRoughnessDescTable.Init(XXH3_64bits(GlobalResource::METALLIC_ROUGHNESS_DESCRIPTOR_TABLE,
		strlen(GlobalResource::METALLIC_ROUGHNESS_DESCRIPTOR_TABLE)));
	m_emissiveDescTable.Init(XXH3_64bits(GlobalResource::EMISSIVE_DESCRIPTOR_TABLE, strlen(GlobalResource::EMISSIVE_DESCRIPTOR_TABLE)));

	CheckHR(App::GetRenderer().GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));

	m_rendererInterface.Init();

	// allocate a slot for the default material
	Material defaultMat;
	m_matBuffer.Add(DEFAULT_MATERIAL, defaultMat);
}

void SceneCore::OnWindowSizeChanged() noexcept
{
	//m_camera.OnWindowSizeChanged();
	m_rendererInterface.OnWindowSizeChanged();
}

void SceneCore::Update(double dt, TaskSet& sceneTS, TaskSet& sceneRendererTS) noexcept
{
	if (m_isPaused)
		return;

	auto updateWorldTransforms = sceneTS.EmplaceTask("Scene::UpdateWorldTransform", [this, dt]()
		{
			SmallVector<AnimationUpdateOut, App::FrameAllocator> animUpdates;
			UpdateAnimations((float)dt, animUpdates);
			UpdateLocalTransforms(animUpdates);

			SmallVector<BVH::BVHUpdateInput, App::FrameAllocator> toUpdateInstances;
			UpdateWorldTransformations(toUpdateInstances);

			if (m_rebuildBVHFlag)
			{
				RebuildBVH();
				m_rebuildBVHFlag = false;
			}
			else
				m_bvh.Update(toUpdateInstances);
		});

	auto frustumCull = sceneTS.EmplaceTask("Scene::FrustumCull", [this]()
		{
			//m_frameInstances.clear();
			m_frameInstances.free_memory();
			m_frameInstances.reserve(m_IDtoTreePos.size());

			const Camera& camera = App::GetCamera();
			m_bvh.DoFrustumCulling(camera.GetCameraFrustumViewSpace(), camera.GetViewInv(), m_frameInstances);

			App::AddFrameStat("Scene", "FrustumCulled", (uint32_t)(m_IDtoTreePos.size() - m_frameInstances.size()), (uint32_t)m_IDtoTreePos.size());
		});

	sceneTS.AddOutgoingEdge(updateWorldTransforms, frustumCull);

	const uint32_t numInstances = m_emissives.NumEmissiveInstances();
	m_staleEmissives = false;
	
	// full rebuild of the emissive buffers for the first time
	if (numInstances && m_emissives.RebuildFlag() && m_rendererInterface.IsRTASBuilt())
	{
		const uint32_t numTris = m_emissives.NumEmissiveTriangles();
		m_staleEmissives = true;

		constexpr size_t MAX_NUM_EMISSIVE_WORKERS = 4;
		constexpr size_t MIN_EMISSIVE_INSTANCES_PER_WORKER = 35;
		size_t threadOffsets[MAX_NUM_EMISSIVE_WORKERS];
		size_t threadSizes[MAX_NUM_EMISSIVE_WORKERS];
		uint32_t workerEmissiveCount[MAX_NUM_EMISSIVE_WORKERS];

		const int numEmissiveWorkers = (int)SubdivideRangeWithMin(numInstances,
			MAX_NUM_EMISSIVE_WORKERS,
			threadOffsets,
			threadSizes,
			MIN_EMISSIVE_INSTANCES_PER_WORKER);

		auto finishEmissive = sceneTS.EmplaceTask("BuildEmissiveBuffer", [this]()
			{
				m_emissives.RebuildEmissiveBuffer();
			});

		for (int i = 0; i < numEmissiveWorkers; i++)
		{
			StackStr(tname, n, "Scene::Emissive_%d", i);

			auto h = sceneTS.EmplaceTask(tname, [this, offset = threadOffsets[i], size = threadSizes[i]]()
				{
					auto emissvies = m_emissives.EmissiveInstances();
					auto tris = m_emissives.EmissiveTriagnles();
					v_float4x4 I = identity();

					// for every emissive instance, apply related transformation to all of its triangles
					for (int instance = (int)offset; instance < (int)offset + (int)size; instance++)
					{
						auto& e = emissvies[instance];
						const auto rtASInfo = GetInstanceRtASInfo(e.InstanceID);

						const v_float4x4 vW = load4x3(GetToWorld(e.InstanceID));
						if (equal(vW, I))
							continue;

						for (int t = e.BaseTriOffset; t < (int)e.BaseTriOffset + (int)e.NumTriangles; t++)
						{
							__m128 vV0;
							__m128 vV1;
							__m128 vV2;
							tris[t].LoadVertices(vV0, vV1, vV2);

							vV0 = mul(vW, vV0);
							vV1 = mul(vW, vV1);
							vV2 = mul(vW, vV2);

							tris[t].StoreVertices(vV0, vV1, vV2);

							// TODO ID initially contains triangle index within each mesh, after
							// hashing it below, it's lost and subsequent runs will give wrong results as it
							// won't match the computation in rt shaders
							uint32_t hash = Pcg3d(uint3(rtASInfo.GeometryIndex, rtASInfo.InstanceID, tris[t].ID)).x;

							Assert(!tris[t].IsIDPatched(), "rewriting emissive triangle after the first assignment is invalid.");
							tris[t].ResetID(hash);
						}
					}
				});

			sceneTS.AddOutgoingEdge(updateWorldTransforms, h);
			sceneTS.AddOutgoingEdge(h, finishEmissive);
		}
	}

	if (m_staleStaticInstances)
	{
		sceneTS.EmplaceTask("Scene::RebuildMeshBuffers", [this]()
			{
				m_meshes.RebuildBuffers();
			});
	}

	m_matBuffer.UpdateGPUBufferIfStale();

	m_rendererInterface.Update(sceneRendererTS);
}

void SceneCore::Recycle() noexcept
{
	if (m_baseColorDescTable.m_pending.empty() &&
		m_normalDescTable.m_pending.empty() &&
		m_metallicRoughnessDescTable.m_pending.empty() &&
		m_emissiveDescTable.m_pending.empty() &&
		m_matBuffer.m_pending.empty())
		return;

	App::GetRenderer().SignalDirectQueue(m_fence.Get(), m_nextFenceVal++);

	uint64_t completedFenceVal = m_fence->GetCompletedValue();
	m_baseColorDescTable.Recycle(completedFenceVal);
	m_normalDescTable.Recycle(completedFenceVal);
	m_metallicRoughnessDescTable.Recycle(completedFenceVal);
	m_emissiveDescTable.Recycle(completedFenceVal);
	m_matBuffer.Recycle(completedFenceVal);
}

void SceneCore::Shutdown() noexcept
{
	ID3D12Fence* fence = m_fence.Get();
	App::GetRenderer().SignalDirectQueue(fence, m_nextFenceVal);

	if (fence->GetCompletedValue() < m_nextFenceVal)
	{
		HANDLE handle = CreateEvent(nullptr, false, false, "");
		CheckWin32(handle);

		CheckHR(fence->SetEventOnCompletion(1, handle));
		WaitForSingleObject(handle, INFINITE);

		CloseHandle(handle);
	}

	m_matBuffer.Clear();
	m_baseColorDescTable.Clear();
	m_normalDescTable.Clear();
	m_metallicRoughnessDescTable.Clear();
	m_emissiveDescTable.Clear();
	m_meshes.Clear();
	m_emissives.Clear();
	m_bvh.Clear();

	m_baseColTableOffsetToID.free();
	m_normalTableOffsetToID.free();
	m_metallicRoughnessTableOffsetToID.free();
	m_emissiveTableOffsetToID.free();

	m_prevToWorlds.free_memory();
	//m_sceneMetadata.free();
	m_sceneGraph.free_memory();
	m_IDtoTreePos.free();

	m_rendererInterface.Shutdown();
}

void SceneCore::AddMeshes(uint64_t sceneID, SmallVector<Model::glTF::Asset::Mesh>&& meshes,
	SmallVector<Core::Vertex>&& vertices, SmallVector<uint32_t>&& indices) noexcept
{
	AcquireSRWLockExclusive(&m_meshLock);
	m_meshes.AddBatch(sceneID, ZetaMove(meshes), ZetaMove(vertices), ZetaMove(indices));
	ReleaseSRWLockExclusive(&m_meshLock);
}

void SceneCore::AddMaterial(uint64_t sceneID, const glTF::Asset::MaterialDesc& matDesc, Span<glTF::Asset::DDSImage> ddsImages) noexcept
{
	Assert(matDesc.Index >= 0, "invalid material index.");
	const uint64_t matFromSceneID = MaterialID(sceneID, matDesc.Index);
	Check(matFromSceneID != DEFAULT_MATERIAL, "This material ID is reserved.");

	Material mat;
	mat.BaseColorFactor = Float4ToRGBA8(matDesc.BaseColorFactor);
	mat.EmissiveFactorNormalScale = Float4ToRGBA8(float4(matDesc.EmissiveFactor, matDesc.NormalScale));
	mat.MetallicFactorAlphaCuttoff = Float2ToRG8(float2(matDesc.MetallicFactor, matDesc.AlphaCuttoff));
	mat.RoughnessFactor = half(matDesc.RoughnessFactor);
	mat.SetAlphaMode(matDesc.AlphaMode);
	mat.SetDoubleSided(matDesc.DoubleSided);

	auto addTex = [](uint64_t ID, const char* type, TexSRVDescriptorTable& table, uint32_t& tableOffset, Span<DDSImage> ddsImages) noexcept
	{
		auto idx = BinarySearch(ddsImages, ID, [](const DDSImage& obj) {return obj.ID; });
		Check(idx != -1, "%s image with ID %llu was not found.", type, ID);

		tableOffset = table.Add(ZetaMove(ddsImages[idx].T), ID);
	};

	AcquireSRWLockExclusive(&m_matLock);

	{
		uint32_t tableOffset = uint32_t(-1);	// i.e. index in GPU descriptor table
		
		if (matDesc.BaseColorTexPath != uint64_t(-1))
		{
			addTex(matDesc.BaseColorTexPath, "BaseColor", m_baseColorDescTable, tableOffset, ddsImages);
			m_baseColTableOffsetToID[tableOffset] = matDesc.BaseColorTexPath;
		}
		
		mat.BaseColorTexture = tableOffset;
	}

	{
		uint32_t tableOffset = uint32_t(-1);
		if (matDesc.NormalTexPath != uint64_t(-1))
		{
			addTex(matDesc.NormalTexPath, "NormalMap", m_normalDescTable, tableOffset, ddsImages);
			m_normalTableOffsetToID[tableOffset] = matDesc.NormalTexPath;
		}
		
		mat.NormalTexture = tableOffset;
	}

	{
		uint32_t tableOffset = uint32_t(-1);
		if (matDesc.MetallicRoughnessTexPath != uint64_t(-1))
		{
			addTex(matDesc.MetallicRoughnessTexPath, "MetallicRoughnessMap", m_metallicRoughnessDescTable, tableOffset, ddsImages);
			m_metallicRoughnessTableOffsetToID[tableOffset] = matDesc.MetallicRoughnessTexPath;
		}
		
		mat.MetallicRoughnessTexture = tableOffset;
	}

	{
		uint32_t tableOffset = uint32_t(-1);
		if (matDesc.EmissiveTexPath != uint64_t(-1))
		{
			addTex(matDesc.EmissiveTexPath, "EmissiveMap", m_emissiveDescTable, tableOffset, ddsImages);
			m_emissiveTableOffsetToID[tableOffset] = matDesc.EmissiveTexPath;
		}
		
		mat.SetEmissiveTex(tableOffset);
		mat.SetEmissiveStrength(matDesc.EmissiveStrength);
	}

	// add it to GPU material buffer, which offsets into descriptor tables above
	m_matBuffer.Add(matFromSceneID, mat);

	// remember from which glTF scene this material came from
	//m_sceneMetadata[sceneID].MaterialIDs.push_back(matFromSceneID);

	ReleaseSRWLockExclusive(&m_matLock);
}

void SceneCore::AddInstance(uint64_t sceneID, glTF::Asset::InstanceDesc&& instance) noexcept
{
	const uint64_t meshID = instance.MeshIdx == -1 ? NULL_MESH : MeshID(sceneID, instance.MeshIdx, instance.MeshPrimIdx);
	//const uint64_t instanceID = InstanceID(sceneID, instance.Name, instance.MeshIdx, instance.MeshPrimIdx);

	AcquireSRWLockExclusive(&m_instanceLock);

	if (instance.RtMeshMode == RT_MESH_MODE::STATIC && meshID != NULL_MESH)
	{
		m_numStaticInstances++;
		m_staleStaticInstances = true;
	}
	else
		m_numDynamicInstances++;
	
	int treeLevel = 1;
	int parentIdx = 0;
	
	// get parent's index from the hashmap
	if (instance.ParentID != ROOT_ID)
	{
		TreePos* p = FindTreePosFromID(instance.ParentID);
		Assert(p, "instance with ID %llu was not found in the scene graph.", instance.ParentID);

		treeLevel = p->Level + 1;
		parentIdx = p->Offset;
	}
	
	const int insertIdx = InsertAtLevel(instance.ID, treeLevel, parentIdx, instance.LocalTransform, meshID,
		instance.RtMeshMode, instance.RtInstanceMask);

	// update instance "dictionary"
	{
		Assert(m_IDtoTreePos.find(instance.ID) == nullptr, "instance with id %llu already exists.", instance.ID);
		//m_IDtoTreePos.emplace(instanceID, TreePos{ .Level = treeLevel, .Offset = insertIdx });
		m_IDtoTreePos.insert_or_assign(instance.ID, TreePos{ .Level = treeLevel, .Offset = insertIdx });

		// adjust tree positions of shifted instances
		for(int i = insertIdx + 1; i < m_sceneGraph[treeLevel].m_IDs.size(); i++)
		{
			uint64_t insID = m_sceneGraph[treeLevel].m_IDs[i];
			TreePos* p = m_IDtoTreePos.find(insID);
			Assert(p, "instance with ID %llu was not found in the scene graph.", insID);

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

int SceneCore::InsertAtLevel(uint64_t id, int treeLevel, int parentIdx, AffineTransformation& localTransform,
	uint64_t meshID, RT_MESH_MODE rtMeshMode, uint8_t rtInstanceMask) noexcept
{
	while(treeLevel >= m_sceneGraph.size())
		m_sceneGraph.emplace_back(m_memoryPool);

	auto& parentLevel = m_sceneGraph[treeLevel - 1];
	auto& currLevel = m_sceneGraph[treeLevel];
	auto& parentRange = parentLevel.m_subtreeRanges[parentIdx];

	// insert position is right next to parent's rightmost child
	const int insertIdx = parentRange.Base + parentRange.Count;

	// increment parent's children count
	parentRange.Count++;

	// append it to the end, then keep swapping it back until it's at insertIdx
	auto rearrange = []<typename T, typename... Args> requires std::is_swappable<T>::value
		(Vector<T, Support::PoolAllocator>& vec, int insertIdx, Args&&... args) noexcept
		{
			vec.emplace_back(T(ZetaForward(args)...));

			for (int i = (int)vec.size() - 1; i != insertIdx; --i)
				std::swap(vec[i], vec[i - 1]);
		};

	float4x3 I = float4x3(store(identity()));

	rearrange(currLevel.m_IDs, insertIdx, id);
	rearrange(currLevel.m_localTransforms, insertIdx, localTransform);
	rearrange(currLevel.m_toWorlds, insertIdx, I);
	rearrange(currLevel.m_meshIDs, insertIdx, meshID);
	const int newBase = currLevel.m_subtreeRanges.empty() ? 0 : currLevel.m_subtreeRanges.back().Base + currLevel.m_subtreeRanges.back().Count;
	rearrange(currLevel.m_subtreeRanges, insertIdx, newBase, 0);
	// set rebuild flag to true for any instance that is added for the first time
	rearrange(currLevel.m_rtFlags, insertIdx, SetRtFlags(rtMeshMode, rtInstanceMask, 1, 0));
	rearrange(currLevel.m_rtASInfo, insertIdx, RT_AS_Info());

	// shift base offset of parent's right siblings to right by one
	for (int siblingIdx = parentIdx + 1; siblingIdx != parentLevel.m_subtreeRanges.size(); siblingIdx++)
		parentLevel.m_subtreeRanges[siblingIdx].Base++;

	return insertIdx;
}

void SceneCore::AddAnimation(uint64_t id, Vector<Keyframe>&& keyframes, float tOffset, bool isSorted) noexcept
{
#ifdef _DEBUG
	TreePos *p = FindTreePosFromID(id);
	Assert(p, "instance with ID %llu was not found in the scene graph.", id);
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

	// save starting offset and number of keyframes
	const uint32_t currOffset = (uint32_t)m_keyframes.size();
	m_animationOffsets.emplace_back(currOffset, currOffset + (int)keyframes.size(), tOffset);

	// save  mapping from instance ID to starting offset in keyframe buffer
	m_animOffsetToInstanceMap.emplace_back(id, currOffset);

	// insertion sort
	int currIdx = Math::Max(0, (int)m_animOffsetToInstanceMap.size() - 2);
	while (currIdx >= 0 && id < m_animOffsetToInstanceMap[currIdx].Offset)
	{
		std::swap(m_animOffsetToInstanceMap[currIdx], m_animOffsetToInstanceMap[currIdx + 1]);
		currIdx--;
	}

	// append
	m_keyframes.append_range(keyframes.begin(), keyframes.end());
}

float4x3 SceneCore::GetPrevToWorld(uint64_t key) noexcept
{
	const auto idx = BinarySearch(Span(m_prevToWorlds), key, [](const PrevToWorld& p) {return p.ID; });
	if (idx != -1)
		return m_prevToWorlds[idx].W;

	return float4x3(store(identity()));
}

void SceneCore::AddEmissives(Util::SmallVector<Model::glTF::Asset::EmissiveInstance>&& emissiveInstances, 
	SmallVector<RT::EmissiveTriangle>&& emissiveTris) noexcept
{
	if (emissiveTris.empty())
		return;

	AcquireSRWLockExclusive(&m_emissiveLock);
	m_emissives.AddBatch(ZetaMove(emissiveInstances), ZetaMove(emissiveTris));
	ReleaseSRWLockExclusive(&m_emissiveLock);
}

void SceneCore::RebuildBVH() noexcept
{
	SmallVector<BVH::BVHInput, App::FrameAllocator> allInstances;
	allInstances.reserve(m_IDtoTreePos.size());

	m_instanceVisibilityIdx.resize(m_IDtoTreePos.size());

	const int numLevels = (int)m_sceneGraph.size();
	uint32_t currInsIdx = 0;

	for (int level = 1; level < numLevels; ++level)
	{
		for (int i = 0; i < m_sceneGraph[level].m_toWorlds.size(); ++i)
		{
			// find this intantce's Mesh
			const uint64_t meshID = m_sceneGraph[level].m_meshIDs[i];
			if (meshID == NULL_MESH)
				continue;

			TriangleMesh* mesh = m_meshes.GetMesh(meshID);
			Assert(mesh, "mesh with id %llu was not found", meshID);

			v_AABB vBox(mesh->m_AABB);
			v_float4x4 vM = load4x3(m_sceneGraph[level].m_toWorlds[i]);

			// transform AABB to world space
			vBox = transform(vM, vBox);
			const uint64_t insID = m_sceneGraph[level].m_IDs[i];

			allInstances.emplace_back(BVH::BVHInput{ .AABB = store(vBox), .ID = insID });

			m_instanceVisibilityIdx.emplace(insID, currInsIdx++);
		}
	}

	m_bvh.Build(allInstances);
}

void SceneCore::UpdateWorldTransformations(Vector<BVH::BVHUpdateInput, App::FrameAllocator>& toUpdateInstances) noexcept
{
	m_prevToWorlds.clear();
	const int numLevels = (int)m_sceneGraph.size();

	SmallVector<EmissiveInstance, App::FrameAllocator> modifiedEmissives;

	for (int level = 0; level < numLevels - 1; ++level)
	{
		for (int i = 0; i < m_sceneGraph[level].m_subtreeRanges.size(); i++)
		{
			v_float4x4 vParentTransform = load4x3(m_sceneGraph[level].m_toWorlds[i]);
			const auto& range = m_sceneGraph[level].m_subtreeRanges[i];

			for (int j = range.Base; j < range.Base + range.Count; j++)
			{
				AffineTransformation& tr = m_sceneGraph[level + 1].m_localTransforms[j];
				v_float4x4 vLocal = affineTransformation(tr.Scale, tr.Rotation, tr.Translation);
				v_float4x4 newW = mul(vLocal, vParentTransform);
				v_float4x4 prevW = load4x3(m_sceneGraph[level + 1].m_toWorlds[j]);

				if (!m_rebuildBVHFlag && !equal(newW, prevW))
				{
					const uint64_t meshID = m_sceneGraph[level + 1].m_meshIDs[j];
					TriangleMesh* mesh = m_meshes.GetMesh(meshID);
					Assert(mesh, "mesh with id %llu was not found", meshID);

					v_AABB vOldBox(mesh->m_AABB);
					vOldBox = transform(prevW, vOldBox);
					v_AABB vNewBox = transform(newW, vOldBox);
					const uint64_t ID = m_sceneGraph[level + 1].m_IDs[j];

					toUpdateInstances.emplace_back(BVH::BVHUpdateInput{ 
						.OldBox = store(vOldBox),
						.NewBox = store(vNewBox),
						.ID = ID });

					RT_Flags f = GetRtFlags(m_sceneGraph[level + 1].m_rtFlags[j]);
					Assert(f.MeshMode != RT_MESH_MODE::STATIC, "Transformation of static meshes can't change");
					Assert(!f.RebuildFlag, "Rebuild & update flags can't be set at the same time.");

					m_sceneGraph[level + 1].m_rtFlags[j] = SetRtFlags(f.MeshMode, f.InstanceMask, 0, 1);

					auto* emissive = m_emissives.FindEmissive(ID);
					if (emissive)
						modifiedEmissives.push_back(*emissive);
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

	if (!modifiedEmissives.empty())
	{
		m_staleEmissives = true;
		UpdateEmissives(modifiedEmissives);
	}
}

void SceneCore::UpdateAnimations(float t, Vector<AnimationUpdateOut, App::FrameAllocator>& animVec) noexcept
{
	for (int i = 0; i < m_animationOffsets.size(); i++)
	{
		AffineTransformation vRes;

		// interpolate
		const Keyframe& kStart = m_keyframes[m_animationOffsets[i].BegOffset];
		const Keyframe& kEnd = m_keyframes[m_animationOffsets[i].EndOffset - 1];
		const float startOffset = m_animationOffsets[i].BegTimeOffset;

		// fast path
		if (t <= kStart.Time + startOffset)
		{
			vRes = kStart.Transform;
		}
		else if (t >= kEnd.Time + startOffset)
		{
			vRes = kEnd.Transform;
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
			float4a temp5 = float4a(k1.Transform.Rotation);
			float4a temp6 = float4a(k2.Transform.Rotation);
			const __m128 vRot1 = _mm_load_ps(reinterpret_cast<float*>(&temp5));
			const __m128 vRot2 = _mm_load_ps(reinterpret_cast<float*>(&temp6));
			const __m128 vRotInt = slerp(vRot1, vRot2, interpolatedT);

			//vRes = affineTransformation(vScaleInt, vRotInt, vTranslateInt);
			vRes.Scale = storeFloat3(vScaleInt);
			vRes.Rotation = storeFloat4(vRotInt);
			vRes.Translation = storeFloat3(vTranslateInt);
		}

		animVec.emplace_back(AnimationUpdateOut{ .M = vRes, .Offset = m_animationOffsets[i].BegOffset });
	}
}

void SceneCore::UpdateLocalTransforms(Span<AnimationUpdateOut> animVec) noexcept
{
	for(auto& update : animVec)
	{
		uint64_t insID = uint64_t(-1);

		{
			// binary search
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
		Assert(t, "instance with ID %llu was not found in the scene graph.", insID);

		m_sceneGraph[t->Level].m_localTransforms[t->Offset] = update.M;
	}
}

// TODO following is untested
void SceneCore::UpdateEmissives(Util::Span<EmissiveInstance> instances) noexcept
{
	auto emissvies = m_emissives.EmissiveInstances();
	auto tris = m_emissives.EmissiveTriagnles();
	v_float4x4 I = identity();

	for (auto& e : instances)
	{
		const v_float4x4 vCurrW = load4x3(GetToWorld(e.InstanceID));
		if (equal(vCurrW, I))
			continue;
		
		// undo previous transformation
		v_float4x4 vPrevW_inv = load4x3(GetPrevToWorld(e.InstanceID));
		vPrevW_inv = inverseSRT(vPrevW_inv);
		// then apply the new transformation
		const v_float4x4 vNewW = mul(vPrevW_inv, vCurrW);

		// TODO repeated transformations can accumulate floating-point errors, do a rebuild
		// after certain number of updates

		for (int t = e.BaseTriOffset; t < (int)e.BaseTriOffset + (int)e.NumTriangles; t++)
		{
			__m128 vV0;
			__m128 vV1;
			__m128 vV2;
			tris[t].LoadVertices(vV0, vV1, vV2);

			vV0 = mul(vNewW, vV0);
			vV1 = mul(vNewW, vV1);
			vV2 = mul(vNewW, vV2);

			tris[t].StoreVertices(vV0, vV1, vV2);
		}
	}

	m_emissives.RebuildEmissiveBuffer();
}
