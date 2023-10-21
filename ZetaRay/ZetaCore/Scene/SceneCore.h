#pragma once

#include "../Model/glTFAsset.h"
#include "../Math/BVH.h"
#include "Asset.h"
#include "SceneRenderer.h"
#include <Utility/Optional.h>
#include <Utility/Utility.h>
#include <xxHash/xxhash.h>

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
	struct Keyframe
	{
		Math::AffineTransformation Transform;
		float Time;
	};

	struct RT_Flags
	{
		Model::RT_MESH_MODE MeshMode;
		uint8_t InstanceMask;
		bool IsOpaque;
		bool RebuildFlag;
		bool UpdateFlag;
	};

	// 7        6     5         4       3     2     1     0
	//  meshmode    update    build   opaque     instance
	ZetaInline uint8_t SetRtFlags(Model::RT_MESH_MODE m, uint8_t instanceMask, uint8_t rebuild, uint8_t update, bool isOpaque)
	{
		return ((uint8_t)m << 6) | instanceMask | (isOpaque << 3) | (rebuild << 4) | (update << 5);
	}
	ZetaInline RT_Flags GetRtFlags(uint8_t f)
	{
		return RT_Flags{
			.MeshMode = (Model::RT_MESH_MODE)(f >> 6),
			.InstanceMask = (uint8_t)(f & 0x7),
			.IsOpaque = bool((f >> 3) & 0x1),
			.RebuildFlag = bool((f >> 4) & 0x1),
			.UpdateFlag = bool((f >> 5) & 0x1) };
	}

	struct RT_AS_Info
	{
		uint32_t GeometryIndex;
		uint32_t InstanceID;
	};

	class SceneCore
	{
		friend struct RT::StaticBLAS;
		friend struct RT::TLAS;

	public:
		static constexpr uint64_t ROOT_ID = uint64_t(-1);
		static constexpr uint64_t NULL_MESH = uint64_t(-1);
		static constexpr uint64_t DEFAULT_MATERIAL = uint64_t(0);

		static ZetaInline uint64_t InstanceID(uint64_t sceneID, int nodeIdx, int mesh, int meshPrim)
		{
			StackStr(str, n, "instancee%d_%d_%d_%d", sceneID, nodeIdx, mesh, meshPrim);
			uint64_t instanceFromSceneID = XXH3_64bits(str, n);

			return instanceFromSceneID;
		}

		static ZetaInline uint64_t MaterialID(uint64_t sceneID, int materialIdx)
		{
			StackStr(str, n, "mat_%llu_%d", sceneID, materialIdx);
			uint64_t matFromSceneID = XXH3_64bits(str, n);

			return matFromSceneID;
		}

		static ZetaInline uint64_t MeshID(uint64_t sceneID, int meshIdx, int meshPrimIdx)
		{
			StackStr(str, n, "mesh_%llu_%d_%d", sceneID, meshIdx, meshPrimIdx);
			uint64_t meshFromSceneID = XXH3_64bits(str, n);

			return meshFromSceneID;
		}

		SceneCore();
		~SceneCore() = default;

		SceneCore(const SceneCore&) = delete;
		SceneCore& operator=(const SceneCore&) = delete;

		void Init(Renderer::Interface& rendererInterface);
		void Pause() { m_isPaused = true; }
		void Resume() { m_isPaused = false; }
		void OnWindowSizeChanged();

		void Update(double dt, Support::TaskSet& sceneTS, Support::TaskSet& sceneRendererTS);
		void Render(Support::TaskSet& ts) { m_rendererInterface.Render(ts); };

	 	ZetaInline Math::AABB GetWorldAABB() { return m_bvh.GetWorldAABB(); }

		//
		// Mesh
		//
		void AddMeshes(uint64_t sceneID, Util::SmallVector<Model::glTF::Asset::Mesh>&& meshes,
			Util::SmallVector<Core::Vertex>&& vertices,
			Util::SmallVector<uint32_t>&& indices);
		ZetaInline Util::Optional<Model::TriangleMesh*> GetMesh(uint64_t id)
		{
			return m_meshes.GetMesh(id);
		}

		ZetaInline const Core::GpuMemory::DefaultHeapBuffer& GetMeshVB() { return m_meshes.GetVB(); }
		ZetaInline const Core::GpuMemory::DefaultHeapBuffer& GetMeshIB() { return m_meshes.GetIB(); }

		//
		// Material
		//
		void AddMaterial(uint64_t sceneID, const Model::glTF::Asset::MaterialDesc& mat, Util::Span<Model::glTF::Asset::DDSImage> ddsImages);
		ZetaInline Util::Optional<Material*> GetMaterial(uint64_t id)
		{
			return m_matBuffer.Get(id);
		}

		ZetaInline uint32_t GetBaseColMapsDescHeapOffset() const { return m_baseColorDescTable.m_descTable.GPUDesciptorHeapIndex(); }
		ZetaInline uint32_t GetNormalMapsDescHeapOffset() const { return m_normalDescTable.m_descTable.GPUDesciptorHeapIndex(); }
		ZetaInline uint32_t GetMetallicRougnessMapsDescHeapOffset() const { return m_metallicRoughnessDescTable.m_descTable.GPUDesciptorHeapIndex(); }
		ZetaInline uint32_t GetEmissiveMapsDescHeapOffset() const { return m_emissiveDescTable.m_descTable.GPUDesciptorHeapIndex(); }

		//
		// Instance
		//
		void AddInstance(uint64_t sceneID, Model::glTF::Asset::InstanceDesc&& instance);
		ZetaInline Util::Optional<Math::float4x3*> GetPrevToWorld(uint64_t id)
		{
			const auto idx = Util::BinarySearch(Util::Span(m_prevToWorlds), id, [](const PrevToWorld& p) {return p.ID; });
			if (idx != -1)
				return &m_prevToWorlds[idx].W;

			return {};
		}
		
		//
		// emissive
		//
		void AddEmissives(Util::SmallVector<Model::glTF::Asset::EmissiveInstance>&& emissiveInstances, 
			Util::SmallVector<RT::EmissiveTriangle>&& emissiveTris);
		size_t NumEmissiveInstances() const { return m_emissives.NumEmissiveInstances(); }
		size_t NumEmissiveTriangles() const { return m_emissives.NumEmissiveTriangles(); }
		bool AreEmissivesStale() const { return m_staleEmissives; }

		ZetaInline const Math::float4x3& GetToWorld(uint64_t id) const
		{
			const TreePos& p = FindTreePosFromID(id).value();
			return m_sceneGraph[p.Level].m_toWorlds[p.Offset];
		}

		ZetaInline uint64_t GetInstanceMeshID(uint64_t id) const
		{
			const TreePos& p = FindTreePosFromID(id).value();
			return m_sceneGraph[p.Level].m_meshIDs[p.Offset];
		}

		ZetaInline RT_AS_Info GetInstanceRtASInfo(uint64_t id) const
		{
			const TreePos& p = FindTreePosFromID(id).value();
			return m_sceneGraph[p.Level].m_rtASInfo[p.Offset];
		}

		ZetaInline uint32_t GetInstanceVisibilityIndex(uint64_t id) const
		{
			auto* e = m_instanceVisibilityIdx.find(id);
			Assert(e, "instance with ID %llu was not found.", id);

			return *e;
		}

		ZetaInline uint32_t GetTotalNumInstances() const { return (uint32_t)m_IDtoTreePos.size(); }
		ZetaInline uint32_t GetNumOpaqueInstances() const { return m_numOpaqueInstances; }
		ZetaInline uint32_t GetNumNonOpaqueInstances() const { return m_numNonOpaqueInstances; }
		ZetaInline Util::Span<Math::BVH::BVHInput> GetFrameInstances() { return m_frameInstances; }

		void AddAnimation(uint64_t id, Util::Vector<Keyframe>&& keyframes, float tOffset, bool isSorted = true);

		//
		// Cleanup
		//
		void Recycle();
		void Shutdown();

		ZetaInline Core::RenderGraph* GetRenderGraph() { return m_rendererInterface.GetRenderGraph(); }
		ZetaInline void DebugDrawRenderGraph() { m_rendererInterface.DebugDrawRenderGraph(); }

	private:
		static constexpr uint32_t BASE_COLOR_DESC_TABLE_SIZE = 256;
		static constexpr uint32_t NORMAL_DESC_TABLE_SIZE = 256;
		static constexpr uint32_t METALLIC_ROUGHNESS_DESC_TABLE_SIZE = 256;
		static constexpr uint32_t EMISSIVE_DESC_TABLE_SIZE = 64;

		// make sure memory pool is declared first -- "members are guaranteed to be initialized 
		// by order of declaration and destroyed in reverse order"
		Support::MemoryPool m_memoryPool;

		struct TreePos
		{
			int Level;
			int Offset;
		};

		ZetaInline Util::Optional<TreePos> FindTreePosFromID(uint64_t id) const
		{ 
			auto pos = m_IDtoTreePos.find(id);
			if (pos)
				return *pos;

			return {};
		}

		int InsertAtLevel(uint64_t id, int treeLevel, int parentIdx, Math::AffineTransformation& localTransform,
			uint64_t meshID, Model::RT_MESH_MODE rtMeshMode, uint8_t rtInstanceMask, bool isOpaque);

		void UpdateWorldTransformations(Util::Vector<Math::BVH::BVHUpdateInput, App::FrameAllocator>& toUpdateInstances);
		void RebuildBVH();

		struct AnimationUpdateOut
		{
			Math::AffineTransformation M;
			int Offset;
		};

		void UpdateAnimations(float t, Util::Vector<AnimationUpdateOut, App::FrameAllocator>& animVec);
		void UpdateLocalTransforms(Util::Span<AnimationUpdateOut> animVec);
		void UpdateEmissives(Util::Span<Model::glTF::Asset::EmissiveInstance> instances);

		bool m_isPaused = false;

		//
		// scene graph
		//

		// Maps instance ID to tree position
		Util::HashTable<TreePos> m_IDtoTreePos;

		struct Range
		{
			Range() = default;
			Range(int b, int c)
				: Base(b),
				Count(c)
			{}

			int Base;
			int Count;
		};

		struct TreeLevel
		{
			TreeLevel(Support::MemoryPool& mp)
				: m_IDs(mp),
				m_localTransforms(mp),
				m_toWorlds(mp),
				m_meshIDs(mp),
				m_subtreeRanges(mp),
				m_rtFlags(mp),
				m_rtASInfo(mp)
			{}

			Util::SmallVector<uint64_t, Support::PoolAllocator> m_IDs;
			Util::SmallVector<Math::AffineTransformation, Support::PoolAllocator> m_localTransforms;
			Util::SmallVector<Math::float4x3, Support::PoolAllocator> m_toWorlds;
			Util::SmallVector<uint64_t, Support::PoolAllocator> m_meshIDs;
			Util::SmallVector<Range, Support::PoolAllocator> m_subtreeRanges;
			// first six bits encode MeshInstanceFlags, last two bits indicate RT_MESH_MODE
			Util::SmallVector<uint8_t, Support::PoolAllocator> m_rtFlags;
			Util::SmallVector<RT_AS_Info, Support::PoolAllocator> m_rtASInfo;
		};

		Util::SmallVector<TreeLevel, Support::PoolAllocator> m_sceneGraph;

		//
		// scene metadata
		//

		// TODO use MemoryPool for these
		struct SceneMetadata
		{
			Util::SmallVector<uint64_t> Meshes;
			Util::SmallVector<uint64_t> MaterialIDs;
			Util::SmallVector<uint64_t> Instances;
		};

		uint32_t m_numStaticInstances = 0;
		uint32_t m_numDynamicInstances = 0;
		uint32_t m_numOpaqueInstances = 0;
		uint32_t m_numNonOpaqueInstances = 0;
		bool m_staleEmissives = false;

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

		Util::SmallVector<PrevToWorld, Support::PoolAllocator> m_prevToWorlds;

		//
		// BVH
		//

		Math::BVH m_bvh;
		bool m_rebuildBVHFlag = false;

		//
		// instances
		//

		Util::SmallVector<Math::BVH::BVHInput, App::FrameAllocator> m_frameInstances;
		Util::HashTable<uint32_t> m_instanceVisibilityIdx;

		//
		// assets
		//

		Internal::MaterialBuffer m_matBuffer;
		Internal::MeshContainer m_meshes;
		Internal::EmissiveBuffer m_emissives;
		Internal::TexSRVDescriptorTable m_baseColorDescTable;
		Internal::TexSRVDescriptorTable m_normalDescTable;
		Internal::TexSRVDescriptorTable m_metallicRoughnessDescTable;
		Internal::TexSRVDescriptorTable m_emissiveDescTable;

		// mapping from descriptor-table offset to (hash) of the corresponding texture path.
		// textures are only referenced from materials, which use descriptor table offsets
		// to identify their target texture. But textures are stored in the hash table
		// using hash(path), so this mapping is required
		Util::HashTable<uint64_t> m_baseColTableOffsetToID;
		Util::HashTable<uint64_t> m_normalTableOffsetToID;
		Util::HashTable<uint64_t> m_metallicRoughnessTableOffsetToID;
		Util::HashTable<uint64_t> m_emissiveTableOffsetToID;

		ComPtr<ID3D12Fence> m_fence;
		uint64_t m_nextFenceVal = 1;

		SRWLOCK m_matLock = SRWLOCK_INIT;
		SRWLOCK m_meshLock = SRWLOCK_INIT;
		SRWLOCK m_instanceLock = SRWLOCK_INIT;
		SRWLOCK m_emissiveLock = SRWLOCK_INIT;

		//
		// animations
		//

		// has to remain sorted by "Offset"
		struct InstanceToAnimationMap
		{
			InstanceToAnimationMap() = default;
			InstanceToAnimationMap(uint64_t id, int o)
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
			AnimationOffset(int b, int e, float t)
				: BegOffset(b),
				EndOffset(e),
				BegTimeOffset(t)
			{}

			int BegOffset;
			int EndOffset;
			float BegTimeOffset;
		};

		Util::SmallVector<InstanceToAnimationMap, Support::PoolAllocator> m_animOffsetToInstanceMap;
		Util::SmallVector<AnimationOffset, Support::PoolAllocator> m_animationOffsets;
		Util::SmallVector<Keyframe, Support::PoolAllocator> m_keyframes;

		//
		// Scene Renderer
		//
		Renderer::Interface m_rendererInterface;
	};
}
