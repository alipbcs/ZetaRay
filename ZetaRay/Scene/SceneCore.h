#pragma once

#include "Camera.h"
#include "../Math/BVH.h"
#include "../RenderPass/Common/Material.h"
#include "../RenderPass/Common/LightSourceData.h"
#include "../Math/Matrix.h"
#include "AssetManager.h"
#include "SceneRenderer/SceneRenderer.h"
#include <xxHash-0.8.1/xxhash.h>

namespace ZetaRay::Model
{
	struct TriangleMesh;
}

namespace ZetaRay::RT
{
	struct StaticBLAS;
	struct TLAS;
}

namespace ZetaRay::Scene
{
	struct RT_Flags
	{
		RT_MESH_MODE MeshMode;
		uint8_t InstanceMask;
		uint8_t RebuildFlag;
		uint8_t UpdateFlag;
	};

	inline uint8_t SetRtFlags(RT_MESH_MODE m, uint8_t instanceMask, uint8_t rebuild, uint8_t update)
	{
		return ((uint8_t)m << 6) | instanceMask | (rebuild << 4) | (update << 5);
	}
	inline RT_Flags GetRtFlags(uint8_t f)
	{
		return RT_Flags{
			.MeshMode = (RT_MESH_MODE)(f >> 6),
			.InstanceMask = (uint8_t)(f & 0xf),
			.RebuildFlag = uint8_t((f >> 4) & 0x1),
			.UpdateFlag = uint8_t((f >> 5) & 0x1) };
	}

	class SceneCore
	{
		friend struct RT::StaticBLAS;
		friend struct RT::TLAS;

	public:
		static const uint64_t ROOT_ID = -1;
		static inline uint64_t InstanceID(uint64_t sceneID, const char* name, int meshIdx, int meshPrimIdx) noexcept
		{
			uint64_t nameHash = XXH3_64bits(name, strlen(name));
			StackStr(str, n, "instancee_%llu_%llu_%d_%d", sceneID, nameHash, meshIdx, meshPrimIdx);
			uint64_t instanceFromSceneID = XXH3_64bits(str, n);

			return instanceFromSceneID;
		}

		SceneCore() noexcept;
		~SceneCore() noexcept = default;

		SceneCore(const SceneCore&) = delete;
		SceneCore& operator=(const SceneCore&) = delete;

		void Init() noexcept;
		void Pause() noexcept { m_isPaused = true; }
		void Resume() noexcept { m_isPaused = false; }
		void OnWindowSizeChanged() noexcept;

		// Per-frame update & render
		void Update(double dt, Support::TaskSet& sceneTS, Support::TaskSet& sceneRendererTS) noexcept;
		void Render(Support::TaskSet& ts) noexcept { m_sceneRenderer.Render(ts); };

		Math::AABB GetWorldAABB() noexcept;

		// needs to be called prior to loading a scene
		void ReserveScene(uint64_t sceneID, size_t numMeshes, size_t numMats, size_t numNodes) noexcept;
		//void UnloadScene(uint64_t sceneID) noexcept;

		//
		// Mesh
		//
		void AddMesh(uint64_t sceneID, Asset::MeshSubset&& mesh) noexcept;
		inline MeshData GetMeshData(uint64_t id) noexcept
		{
			//std::shared_lock<std::shared_mutex> lock(m_meshMtx);
			AcquireSRWLockShared(&m_meshLock);
			auto d = m_meshManager.GetMeshData(id);
			ReleaseSRWLockShared(&m_meshLock);

			return d;
		}
		inline Math::AABB GetMeshAABB(uint64_t id) noexcept
		{
			//std::shared_lock<std::shared_mutex> lock(m_meshMtx);
			AcquireSRWLockShared(&m_meshLock);
			auto aabb = m_meshManager.GetMeshAABB(id);
			ReleaseSRWLockShared(&m_meshLock);
			
			return aabb;
		}
		//void RemoveMesh(uint64_t id) noexcept;

		//
		// Material
		//
		void AddMaterial(uint64_t sceneID, Asset::MaterialDesc&& mat) noexcept;
		inline Material GetMaterial(uint64_t id) noexcept
		{
			//std::shared_lock<std::shared_mutex> lock(m_matMtx);
			AcquireSRWLockShared(&m_matLock);
			auto mat = m_matBuffer.Get(id);
			ReleaseSRWLockShared(&m_matLock);

			return mat;
		}
		//void RemoveMaterial(uint64_t id) noexcept;

		uint32_t GetBaseColMapsDescHeapOffset() const { return m_baseColorDescTable.Table.GPUDesciptorHeapIndex(); }
		uint32_t GetNormalMapsDescHeapOffset() const { return m_normalsDescTable.Table.GPUDesciptorHeapIndex(); }
		uint32_t GetMetallicRougnessMapsDescHeapOffset() const { return m_metalnessRoughnessDescTable.Table.GPUDesciptorHeapIndex(); }
		uint32_t GetEmissiveMapsDescHeapOffset() const { return m_emissiveDescTable.Table.GPUDesciptorHeapIndex(); }

		//
		// Instance
		//
		void AddInstance(uint64_t sceneID, Asset::InstanceDesc&& instance) noexcept;
		//void RemoveInstance(uint64_t id) noexcept;
		Math::float4x3 GetToWorld(uint64_t id) noexcept;
		Math::float4x3 GetPrevToWorld(uint64_t id) noexcept;
		uint64_t GetMeshIDForInstance(uint64_t id) noexcept;
		const Util::Vector<uint64_t>& GetFrameInstances() { return m_frameInstances; }

		void AddAnimation(uint64_t id, Util::Vector<Keyframe>&& keyframes, float tOffset, bool isSorted = true) noexcept;

		//
		// Light Source
		//
		// TODO not thread-safe
		//void AddAnalyticalLightSource(LightSourceType t) noexcept { m_sceneRenderer.AddAnalyticalLightSource(t); }
		void AddEnvLightSource(const Win32::Filesystem::Path& p) noexcept;

		//
		// Cleanup
		//
		void Recycle() noexcept;
		void Shutdown() noexcept;

		inline void DebugDrawRenderGraph() { m_sceneRenderer.DebugDrawRenderGraph(); }

	private:
		struct TreePos
		{
			int Level;
			int Offset;
		};

		TreePos* FindTreePosFromID(uint64_t id) noexcept;
		int InsertAtLevel(uint64_t id, int treeLevel, int parentIdx, Math::float4x3& localTransform,
			uint64_t meshID, RT_MESH_MODE rtMeshMode, uint8_t rtInstanceMask) noexcept;
		void RemoveFromLevel(int idx, int level) noexcept;

		void UpdateWorldTransformations(Util::Vector<Math::BVH::BVHUpdateInput>& toUpdateInstances) noexcept;
		void RebuildBVH() noexcept;

		const uint64_t k_matBuffID;
		const uint64_t k_baseColorID;
		const uint64_t k_metallicRoughnessID;
		const uint64_t k_normalID;
		const uint64_t k_emissiveID;

		struct AnimationUpdateOut
		{
			Math::float4x3 M;
			int Offset;
		};

		void UpdateAnimations(float t, Util::Vector<AnimationUpdateOut>& animVec) noexcept;
		void UpdateLocalTransforms(Util::Vector<AnimationUpdateOut>&& animVec) noexcept;

		bool m_isPaused = false;

		//
		// scene-graph
		//

		// Maps instance ID to tree-position
		Util::HashTable<TreePos> m_IDtoTreePos;

		struct Range
		{
			Range() noexcept = default;
			Range(int b, int c) noexcept
				: Base(b),
				Count(c)
			{}

			int Base;
			int Count;
		};

		struct TreeLevel
		{
			Util::SmallVector<uint64_t> m_IDs;
			Util::SmallVector<Math::float4x3> m_localTransforms;
			Util::SmallVector<Math::float4x3> m_toWorlds;
			Util::SmallVector<uint64_t> m_meshIDs;
			Util::SmallVector<Range> m_subtreeRanges;
			Util::SmallVector<int> m_parendIndices;
			// first six bits encode MeshInstanceFlags, last two bits indicate RT_MESH_MODE
			Util::SmallVector<uint8_t> m_rtFlags;
		};

		Util::SmallVector<TreeLevel> m_sceneGraph;

		//
		// scene metadata
		//

		struct SceneMetadata
		{
			Util::SmallVector<uint64_t> Meshes;
			Util::SmallVector<uint64_t> MaterialIDs;
			Util::SmallVector<uint64_t> Instances;
		};

		Util::HashTable<SceneMetadata> m_sceneMetadata;

		int m_numStaticInstances = 0;
		int m_numDynamicInstances = 0;

		// this is managed by TLAS, is there a better way?
		bool m_staleStaticInstances = false;

		//
		// previous frame's ToWorld transformations
		//
		
		struct PrevToWorld
		{
			Math::float4x3 W;
			uint64_t ID;
		};

		Util::SmallVector<PrevToWorld> m_prevToWorlds;

		//
		// BVH
		//

		Math::BVH m_bvh;
		bool m_rebuildBVHFlag = false;

		//
		// instances
		//

		Util::SmallVector<uint64_t> m_frameInstances;

		//
		// asset management
		//

		Internal::MaterialBuffer m_matBuffer;
		Internal::MeshManager m_meshManager;
		Internal::TexSRVDescriptorTable m_baseColorDescTable;
		Internal::TexSRVDescriptorTable m_normalsDescTable;
		Internal::TexSRVDescriptorTable m_metalnessRoughnessDescTable;
		Internal::TexSRVDescriptorTable m_emissiveDescTable;

		// mapping from descriptor-table offset to (hash) of corresponding texture-path.
		// textures are only referenced from materials that use descriptor table offsets
		// to identify their target. But textures are stored in the hash table
		// using hash(path), so this mapping is required
		Util::HashTable<uint64_t> m_baseColTableOffsetToID;
		Util::HashTable<uint64_t> m_normalTableOffsetToID;
		Util::HashTable<uint64_t> m_metalnessRougnessrTableOffsetToID;
		Util::HashTable<uint64_t> m_emissiveTableOffsetToID;

		ComPtr<ID3D12Fence> m_fence;
		uint64_t m_nextFenceVal = 1;

		//std::shared_mutex m_matMtx;
		//std::shared_mutex m_meshMtx;
		//std::shared_mutex m_instanceMtx;
		SRWLOCK m_matLock = SRWLOCK_INIT;
		SRWLOCK m_meshLock = SRWLOCK_INIT;
		SRWLOCK m_instanceLock = SRWLOCK_INIT;

		//
		// animations
		//

		// has to remain sorted by "Offset"
		struct InstanceToAnimationMap
		{
			InstanceToAnimationMap() = default;
			InstanceToAnimationMap(uint64_t id, int o) noexcept
				: InstanceID(id),
				Offset(o)
			{}

			uint64_t InstanceID;
			int Offset;
		};

		// offsets into "m_keyframes"
		struct AnimationOffset
		{
			AnimationOffset() = default;
			AnimationOffset(int b, int e, float t) noexcept
				: BegOffset(b),
				EndOffset(e),
				BegTimeOffset(t)
			{}

			int BegOffset;
			int EndOffset;
			float BegTimeOffset;
		};

		Util::SmallVector<InstanceToAnimationMap> m_animOffsetToInstanceMap;
		Util::SmallVector<AnimationOffset> m_animationOffsets;
		Util::SmallVector<Keyframe> m_keyframes;

		//
		// Scene Renderer
		//
		SceneRenderer m_sceneRenderer;
	};
}
