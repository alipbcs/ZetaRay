#pragma once

#include "../Model/Mesh.h"
#include "../Core/Material.h"
#include "../Utility/HashTable.h"
#include "../Core/DescriptorHeap.h"
#include "../Core/GpuMemory.h"
#include "../Model/glTFAsset.h"
#include "../RayTracing/RtCommon.h"

namespace ZetaRay::App::Filesystem
{
	struct Path;
}

namespace ZetaRay::Scene::Internal
{
	//--------------------------------------------------------------------------------------
	// TextureDescriptorTable: a descriptor table containing a contiguous set of textures, which
	// are to be bound as unbounded descriptor tables in the shaders. Each texture index in
	// a given Material refers to an offset in one such descriptor table
	//--------------------------------------------------------------------------------------

	struct TexSRVDescriptorTable
	{
		TexSRVDescriptorTable(const uint32_t descTableSize = 1024);
		~TexSRVDescriptorTable() = default;

		TexSRVDescriptorTable(const TexSRVDescriptorTable&) = delete;
		TexSRVDescriptorTable& operator=(const TexSRVDescriptorTable&) = delete;

		void Init(uint64_t id);

		// Returns offset of the given texture in the desc. table. The texture is then loaded from
		// the disk. "id" is hash of the texture path
		uint32_t Add(Core::GpuMemory::Texture&& tex, uint64_t id);
		//void Remove(uint64_t id, uint64_t nextFenceVal);

		void Clear();
		void Recycle(uint64_t completedFenceVal);

		struct ToBeFreedTexture
		{
			Core::GpuMemory::Texture T;
			uint64_t FenceVal;
			uint32_t DescTableOffset;
		};

		Util::SmallVector<ToBeFreedTexture> m_pending;

		static constexpr int MAX_NUM_DESCRIPTORS = 1024;
		static constexpr int MAX_NUM_MASKS = MAX_NUM_DESCRIPTORS >> 6;
		static_assert(MAX_NUM_MASKS * 64 == MAX_NUM_DESCRIPTORS, "these must match.");
		
		const uint32_t m_descTableSize;
		const uint32_t m_numMasks;
		uint64_t m_inUseBitset[MAX_NUM_MASKS] = { 0 };

		struct CacheEntry
		{
			Core::GpuMemory::Texture T;
			uint32_t DescTableOffset = uint32_t(-1);
			uint32_t RefCount = 0;
		};

		Core::DescriptorTable m_descTable;
		Util::HashTable<CacheEntry> m_cache;
	};

	//--------------------------------------------------------------------------------------
	// MaterialBuffer: a wrapper over an upload-heap buffer containing all the materials required
	// for the current frame
	//--------------------------------------------------------------------------------------

	struct MaterialBuffer
	{
		MaterialBuffer() = default;
		~MaterialBuffer() = default;

		MaterialBuffer(const MaterialBuffer&) = delete;
		MaterialBuffer& operator=(const MaterialBuffer&) = delete;

		void Init(uint64_t id);

		// Allocates an entry for the given material. Index to the allocated entry is also set
		void Add(uint64_t id, Material& mat);
		void UpdateGPUBufferIfStale();
		//void Remove(uint64_t id, uint64_t nextFenceVal);

		// Note: not thread safe
		ZetaInline Material* Get(uint64_t id)
		{
			return m_matTable.find(id);
		}

		void Recycle(uint64_t completedFenceVal);
		void Clear();

		struct ToBeRemoved
		{
			uint64_t FenceVal;
			uint16_t Offset;
		};

		Util::SmallVector<ToBeRemoved> m_pending;

	private:
		static constexpr int MAX_NUM_MATERIALS = 2048;
		static constexpr int NUM_MASKS = MAX_NUM_MATERIALS >> 6;
		static_assert(NUM_MASKS * 64 == MAX_NUM_MATERIALS, "these must match.");
		uint64_t m_inUseBitset[NUM_MASKS] = { 0 };

		Core::GpuMemory::DefaultHeapBuffer m_buffer;
			
		// references to elements are not stable
		Util::HashTable<Material> m_matTable;

		uint64_t k_bufferID = uint64_t (-1);
		bool m_stale = false;
	};

	//--------------------------------------------------------------------------------------
	// MeshContainer
	//--------------------------------------------------------------------------------------

	struct MeshContainer
	{
		void Add(uint64_t id, Util::Span<Core::Vertex> vertices, Util::Span<uint32_t> indices, uint64_t matID);
		void AddBatch(uint64_t sceneID, Util::SmallVector<Model::glTF::Asset::Mesh>&& meshes, Util::SmallVector<Core::Vertex>&& vertices,
			Util::SmallVector<uint32_t>&& indices);
		void Reserve(size_t numVertices, size_t numIndices);
		void RebuildBuffers();
		
		// Note: not thread safe
		ZetaInline Model::TriangleMesh* GetMesh(uint64_t id)
		{
			return m_meshes.find(id);
		}

		const Core::GpuMemory::DefaultHeapBuffer& GetVB() { return m_vertexBuffer; }
		const Core::GpuMemory::DefaultHeapBuffer& GetIB() { return m_indexBuffer; }

		void Clear();

	private:
		Util::HashTable<Model::TriangleMesh> m_meshes;
		Util::SmallVector<Core::Vertex> m_vertices;
		Util::SmallVector<uint32_t> m_indices;

		Core::GpuMemory::DefaultHeapBuffer m_vertexBuffer;
		Core::GpuMemory::DefaultHeapBuffer m_indexBuffer;
	};

	//--------------------------------------------------------------------------------------
	// EmissiveBuffer
	//--------------------------------------------------------------------------------------

	struct EmissiveBuffer
	{
		EmissiveBuffer() = default;
		~EmissiveBuffer() = default;

		EmissiveBuffer(const EmissiveBuffer&) = delete;
		MaterialBuffer& operator=(const EmissiveBuffer&) = delete;

		bool RebuildFlag() { return m_rebuildFlag; }
		void Clear();
		Model::glTF::Asset::EmissiveInstance* FindEmissive(uint64_t ID);
		bool IsStale() { return !m_emissivesTrisCpu.empty(); };
		void AddBatch(Util::SmallVector<Model::glTF::Asset::EmissiveInstance>&& emissiveInstance, 
			Util::SmallVector<RT::EmissiveTriangle>&& emissiveTris);
		void RebuildEmissiveBuffer();
		ZetaInline uint32_t NumEmissiveInstances() const { return (uint32_t)m_emissivesInstances.size(); }
		ZetaInline uint32_t NumEmissiveTriangles() const { return (uint32_t)m_emissivesTrisCpu.size(); }

		Util::Span<Model::glTF::Asset::EmissiveInstance> EmissiveInstances() { return m_emissivesInstances; }
		Util::Span<RT::EmissiveTriangle> EmissiveTriagnles() { return m_emissivesTrisCpu; }

	private:
		Util::SmallVector<Model::glTF::Asset::EmissiveInstance> m_emissivesInstances;
		Util::SmallVector<RT::EmissiveTriangle> m_emissivesTrisCpu;
		Core::GpuMemory::DefaultHeapBuffer m_emissiveTrisGpu;
		bool m_rebuildFlag = true;
	};
}