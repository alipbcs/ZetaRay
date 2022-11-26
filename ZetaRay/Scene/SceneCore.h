#pragma once

#include "Camera.h"
#include "../Model/glTFAsset.h"
#include "../Math/BVH.h"
#include "Asset.h"
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
	struct AffineTransformation
	{
		Math::float3 Scale;
		Math::float4 RotQuat;
		Math::float3 Translation;
	};

	struct Keyframe
	{
		AffineTransformation Transform;
		float Time;
	};

	struct RT_Flags
	{
		Model::RT_MESH_MODE MeshMode;
		uint8_t InstanceMask;
		uint8_t RebuildFlag;
		uint8_t UpdateFlag;
	};

	inline uint8_t SetRtFlags(Model::RT_MESH_MODE m, uint8_t instanceMask, uint8_t rebuild, uint8_t update)
	{
		return ((uint8_t)m << 6) | instanceMask | (rebuild << 4) | (update << 5);
	}
	inline RT_Flags GetRtFlags(uint8_t f)
	{
		return RT_Flags{
			.MeshMode = (Model::RT_MESH_MODE)(f >> 6),
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

		SceneCore() noexcept = default;
		~SceneCore() noexcept = default;

		SceneCore(const SceneCore&) = delete;
		SceneCore& operator=(const SceneCore&) = delete;

		void Init() noexcept;
		void Pause() noexcept { m_isPaused = true; }
		void Resume() noexcept { m_isPaused = false; }
		void OnWindowSizeChanged() noexcept;

		void Update(double dt, Support::TaskSet& sceneTS, Support::TaskSet& sceneRendererTS) noexcept;
		void Render(Support::TaskSet& ts) noexcept { m_sceneRenderer.Render(ts); };

		Math::AABB GetWorldAABB() noexcept;

		// needs to be called prior to loading a scene
		void ReserveScene(uint64_t sceneID, size_t numMeshes, size_t numMats, size_t numNodes) noexcept;
		//void UnloadScene(uint64_t sceneID) noexcept;

		//
		// Mesh
		//
		void AddMesh(uint64_t sceneID, Model::glTF::Asset::MeshSubset&& mesh) noexcept;
		inline Model::TriangleMesh GetMesh(uint64_t id) noexcept
		{
			AcquireSRWLockShared(&m_meshLock);
			auto d = m_meshes.GetMesh(id);
			ReleaseSRWLockShared(&m_meshLock);

			return d;
		}

		const Core::DefaultHeapBuffer& GetMeshVB() noexcept { return m_meshes.GetVB(); }
		const Core::DefaultHeapBuffer& GetMeshIB() noexcept { return m_meshes.GetIB(); }

		//void RemoveMesh(uint64_t id) noexcept;

		//
		// Material
		//
		void AddMaterial(uint64_t sceneID, Model::glTF::Asset::MaterialDesc&& mat) noexcept;
		inline Material GetMaterial(uint64_t id) noexcept
		{
			AcquireSRWLockShared(&m_matLock);
			auto mat = m_matBuffer.Get(id);
			ReleaseSRWLockShared(&m_matLock);

			return mat;
		}
		//void RemoveMaterial(uint64_t id) noexcept;

		uint32_t GetBaseColMapsDescHeapOffset() const;
		uint32_t GetNormalMapsDescHeapOffset() const;
		uint32_t GetMetalnessRougnessMapsDescHeapOffset() const;
		uint32_t GetEmissiveMapsDescHeapOffset() const;

		//
		// Instance
		//
		void AddInstance(uint64_t sceneID, Model::glTF::Asset::InstanceDesc&& instance) noexcept;
		//void RemoveInstance(uint64_t id) noexcept;
		Math::float4x3 GetToWorld(uint64_t id) noexcept;
		Math::float4x3 GetPrevToWorld(uint64_t id) noexcept;
		uint64_t GetMeshIDForInstance(uint64_t id) noexcept;
		Util::Span<uint64_t> GetFrameInstances() { return m_frameInstances; }

		void AddAnimation(uint64_t id, Util::Vector<Keyframe>&& keyframes, float tOffset, bool isSorted = true) noexcept;

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
			uint64_t meshID, Model::RT_MESH_MODE rtMeshMode, uint8_t rtInstanceMask) noexcept;
		//void RemoveFromLevel(int idx, int level) noexcept;

		void UpdateWorldTransformations(Util::Vector<Math::BVH::BVHUpdateInput, App::PoolAllocator>& toUpdateInstances) noexcept;
		void RebuildBVH() noexcept;

		struct AnimationUpdateOut
		{
			Math::float4x3 M;
			int Offset;
		};

		void UpdateAnimations(float t, Util::Vector<AnimationUpdateOut, App::PoolAllocator>& animVec) noexcept;
		void UpdateLocalTransforms(Util::Span<AnimationUpdateOut> animVec) noexcept;

		// TODO remove, should be polled from App
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
			Util::SmallVector<uint64_t, App::PoolAllocator> m_IDs;
			Util::SmallVector<Math::float4x3, App::PoolAllocator> m_localTransforms;
			Util::SmallVector<Math::float4x3, App::PoolAllocator> m_toWorlds;
			Util::SmallVector<uint64_t, App::PoolAllocator> m_meshIDs;
			Util::SmallVector<Range, App::PoolAllocator> m_subtreeRanges;
			Util::SmallVector<int, App::PoolAllocator> m_parendIndices;
			// first six bits encode MeshInstanceFlags, last two bits indicate RT_MESH_MODE
			Util::SmallVector<uint8_t, App::PoolAllocator> m_rtFlags;
		};

		Util::SmallVector<TreeLevel, App::PoolAllocator> m_sceneGraph;

		//
		// scene metadata
		//

		struct SceneMetadata
		{
			Util::SmallVector<uint64_t, App::PoolAllocator> Meshes;
			Util::SmallVector<uint64_t, App::PoolAllocator> MaterialIDs;
			Util::SmallVector<uint64_t, App::PoolAllocator> Instances;
		};

		Util::HashTable<SceneMetadata> m_sceneMetadata;

		uint32_t m_numStaticInstances = 0;
		uint32_t m_numDynamicInstances = 0;

		// TODO this is managed by TLAS, is there a better way?
		bool m_staleStaticInstances = false;

		//
		// previous frame's ToWorld transformations
		//
		
		struct PrevToWorld
		{
			Math::float4x3 W;
			uint64_t ID;
		};

		Util::SmallVector<PrevToWorld, App::PoolAllocator> m_prevToWorlds;

		//
		// BVH
		//

		Math::BVH m_bvh;
		bool m_rebuildBVHFlag = false;

		//
		// instances
		//

		Util::SmallVector<uint64_t, App::PoolAllocator> m_frameInstances;

		//
		// assets
		//

		Internal::MaterialBuffer m_matBuffer;
		Internal::MeshContainer m_meshes;
		Internal::TexSRVDescriptorTable m_baseColorDescTable;
		Internal::TexSRVDescriptorTable m_normalDescTable;
		Internal::TexSRVDescriptorTable m_metalnessRoughnessDescTable;
		Internal::TexSRVDescriptorTable m_emissiveDescTable;

		// mapping from descriptor-table offset to (hash) of the corresponding texture path.
		// textures are only referenced from materials, which use descriptor table offsets
		// to identify their target texture. But textures are stored in the hash table
		// using hash(path), so this mapping is required
		Util::HashTable<uint64_t> m_baseColTableOffsetToID;
		Util::HashTable<uint64_t> m_normalTableOffsetToID;
		Util::HashTable<uint64_t> m_metalnessRougnessrTableOffsetToID;
		Util::HashTable<uint64_t> m_emissiveTableOffsetToID;

		ComPtr<ID3D12Fence> m_fence;
		uint64_t m_nextFenceVal = 1;

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

		Util::SmallVector<InstanceToAnimationMap, App::PoolAllocator> m_animOffsetToInstanceMap;
		Util::SmallVector<AnimationOffset, App::PoolAllocator> m_animationOffsets;
		Util::SmallVector<Keyframe, App::PoolAllocator> m_keyframes;

		//
		// Scene Renderer
		//
		SceneRenderer m_sceneRenderer;
	};
}
