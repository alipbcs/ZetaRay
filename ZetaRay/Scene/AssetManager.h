#pragma once

#include "Assets.h"
#include "../Model/Mesh.h"
#include "../RenderPass/Common/Material.h"
#include "../Utility/HashTable.h"

namespace ZetaRay
{
	struct MeshData
	{
		uint64_t MatID;
		D3D12_GPU_VIRTUAL_ADDRESS VB;
		D3D12_GPU_VIRTUAL_ADDRESS IB;
		int NumVertices;
		int NumIndices;
		uint32_t DescHeapIdx;
	};
}

namespace ZetaRay::Internal
{
	//--------------------------------------------------------------------------------------
	// TextureDescriptorTable: manages a descriptor table containing textures of one of the
	// following groups: BaseColor/Normal/Metallic-Roughness/Emissive 
	//--------------------------------------------------------------------------------------

	struct TexSRVDescriptorTable
	{
		explicit TexSRVDescriptorTable(uint64_t id) noexcept;
		~TexSRVDescriptorTable() noexcept = default;

		TexSRVDescriptorTable(const TexSRVDescriptorTable&) = delete;
		TexSRVDescriptorTable& operator=(const TexSRVDescriptorTable&) = delete;

		void Init() noexcept;

		// Returns offset of the given texture in the desc. table. Texture is loaded from
		// the disk If not found. "id" is hash of the texture path
		uint32_t Add(const Win32::Filesystem::Path& p, uint64_t id) noexcept;
		//void Remove(uint64_t id, uint64_t nextFenceVal) noexcept;
		void Recycle(ID3D12Fence* fence) noexcept;

		// Assumes fence is already signalled to nextFenceVal
		void Clear() noexcept;

		struct ToBeFreedTexture
		{
			Texture T;
			uint64_t FenceVal;
			uint32_t TableOffset;
		};

		SmallVector<ToBeFreedTexture> Pending;

		static constexpr int NUM_DESCRIPTORS = 1024;
		static constexpr int NUM_MASKS = NUM_DESCRIPTORS >> 6;

		struct CacheEntry
		{
			Texture T;
			uint32_t TableOffset = -1;
			uint32_t RefCount = 0;
		};

		DescriptorTable Table;
		HashTable<CacheEntry> Cache;

		// 32 * 64 = 2048 texture slots
		uint64_t Bitset[NUM_MASKS] = { 0 };

		// each SRV Descriptor Table gets a unique ID
		const uint64_t ID;
	};

	//--------------------------------------------------------------------------------------
	// MaterialBuffer
	//--------------------------------------------------------------------------------------

	struct MaterialBuffer
	{
		explicit MaterialBuffer(uint64_t id) noexcept;
		~MaterialBuffer() noexcept = default;

		MaterialBuffer(const MaterialBuffer&) = delete;
		MaterialBuffer& operator=(const MaterialBuffer&) = delete;

		void Init() noexcept;

		// Copies the given material to the GPU buffer and fills its buffer offset
		void Add(uint64_t id, Material& mat) noexcept;
		//void Remove(uint64_t id, uint64_t nextFenceVal) noexcept;

		// returns a copy since references to elements are not stable
		inline Material Get(uint64_t id) noexcept
		{
			Material* m = MaterialTable.find(id);
			Assert(m, "material with id %llu was not found", id);

			if (!m)
				return Material();

			return *m;
		}

		void Recycle(ID3D12Fence* fence) noexcept;
		void Clear() noexcept;

		struct ToBeRemoved
		{
			uint64_t FenceVal;
			uint16_t Offset;
		};

		SmallVector<ToBeRemoved> Pending;

	private:
		static constexpr int NUM_MATERIALS = 2048;
		static constexpr int NUM_MASKS = NUM_MATERIALS >> 6;

		UploadHeapBuffer Buffer;
			
		// references to elements are not stable
		HashTable<Material> MaterialTable;

		// 32 * 64 = 2048 texture slots
		uint64_t Bitset[NUM_MASKS] = { 0 };

		// for registering the Upload Buffer
		const uint64_t ID;
	};

	//--------------------------------------------------------------------------------------
	// MeshManager
	//--------------------------------------------------------------------------------------

	struct MeshManager
	{
		void Add(uint64_t id, 
			Vector<VertexPosNormalTexTangent>&& vertices,
			Vector<INDEX_TYPE>&& indices, uint64_t matID) noexcept;

		// TODO: running time is O(n * logn) 
		//void Remove(uint64_t id, uint64_t nextFenceVal) noexcept;

		inline MeshData GetMeshData(uint64_t id) noexcept
		{
			auto* mesh = m_meshes.find(id);
			Assert(mesh, "Mesh with id %llu was not found", id);

			MeshData ret;

			if (mesh)
			{
				ret.VB = mesh->m_vertexBuffer.GetGpuVA();
				ret.NumVertices = (int)mesh->m_numVertices;
				ret.IB = mesh->m_indexBuffer.GetGpuVA();
				ret.NumIndices = (int)mesh->m_numIndices;
				ret.MatID = mesh->m_materialID;
				ret.DescHeapIdx = mesh->m_descTable.GPUDesciptorHeapIndex();
			}

			return ret;
		}

		inline Math::AABB GetMeshAABB(uint64_t id) noexcept
		{
			auto* m = m_meshes.find(id);
			Assert(m, "Mesh with id %llu was not found", id);

			if (m)
				return m->m_AABB;

			return Math::AABB();
		}

		inline void Clear() noexcept
		{
			m_meshes.clear();
		}

	private:
		HashTable<TriangleMesh> m_meshes;
	};
}