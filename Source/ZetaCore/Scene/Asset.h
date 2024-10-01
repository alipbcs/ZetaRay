#pragma once

#include "../Utility/HashTable.h"
#include "../Core/DescriptorHeap.h"
#include "../Model/glTFAsset.h"
#include "../RayTracing/RtCommon.h"
#include <Utility/Optional.h>

namespace ZetaRay::App::Filesystem
{
    struct Path;
}

namespace ZetaRay::Scene::Internal
{
    //--------------------------------------------------------------------------------------
    // TextureDescriptorTable: A descriptor table containing a contiguous set of textures, 
    // which are to be bound as unbounded descriptor tables in shaders. Each texture index in
    // a given Material refers to an offset in one such descriptor table
    //--------------------------------------------------------------------------------------

    struct TexSRVDescriptorTable
    {
        TexSRVDescriptorTable(const uint32_t descTableSize = 1024);
        ~TexSRVDescriptorTable() = default;

        TexSRVDescriptorTable(const TexSRVDescriptorTable&) = delete;
        TexSRVDescriptorTable& operator=(const TexSRVDescriptorTable&) = delete;

        void Init(uint64_t id);
        // Assumes proper GPU synchronization has been performed
        void Clear();
        // Returns offset of the given texture in the descriptor table. The texture is then loaded from
        // the disk. "id" is hash of the texture path.
        uint32_t Add(Core::GpuMemory::Texture&& tex, uint64_t id);
        void Recycle(uint64_t completedFenceVal);
        ZetaInline uint32_t GPUDescriptorHeapIndex() const { return m_descTable.GPUDescriptorHeapIndex(); }

    private:
        struct ToBeFreedTexture
        {
            Core::GpuMemory::Texture T;
            uint64_t FenceVal;
            uint32_t DescTableOffset;
        };

        struct CacheEntry
        {
            Core::GpuMemory::Texture T;
            uint32_t DescTableOffset = UINT32_MAX;
            uint32_t RefCount = 0;
        };

        static constexpr int MAX_NUM_DESCRIPTORS = 4096;
        static constexpr int MAX_NUM_MASKS = MAX_NUM_DESCRIPTORS >> 6;
        static_assert(MAX_NUM_MASKS * 64 == MAX_NUM_DESCRIPTORS, "these must match.");

        Util::SmallVector<ToBeFreedTexture> m_pending;
        const uint32_t m_descTableSize;
        const uint32_t m_numMasks;
        uint64_t m_inUseBitset[MAX_NUM_MASKS] = { 0 };
        Core::DescriptorTable m_descTable;
        Util::HashTable<CacheEntry> m_cache;
    };

    //--------------------------------------------------------------------------------------
    // MaterialBuffer: Wrapper over a GPU buffer containing all the materials
    //--------------------------------------------------------------------------------------

    struct MaterialBuffer
    {
        MaterialBuffer() = default;
        ~MaterialBuffer() = default;

        MaterialBuffer(const MaterialBuffer&) = delete;
        MaterialBuffer& operator=(const MaterialBuffer&) = delete;

        void Clear();
        void Add(uint32_t ID, const Material& mat);
        void Update(uint32_t ID, const Material& mat)
        {
            auto it = m_materials.find(ID);
            it.value()->Mat = mat;
            m_staleID = ID;
        }
        void UploadToGPU();
        void ResizeAdditionalMaterials(uint32_t num);

        ZetaInline Util::Optional<const Material*> Get(uint32_t ID, uint32* bufferIdx = nullptr) const
        {
            auto it = m_materials.find(ID);
            if (it)
            {
                auto* entry = it.value();

                if (bufferIdx)
                    *bufferIdx = entry->GpuBufferIdx;

                return &entry->Mat;
            }

            return {};
        }

    private:
        struct Entry
        {
            Material Mat;
            uint32_t GpuBufferIdx;
        };

        static constexpr int MAX_NUM_MATERIALS = 4096;
        static constexpr int NUM_MASKS = MAX_NUM_MATERIALS >> 6;
        static_assert(NUM_MASKS * 64 == MAX_NUM_MATERIALS, "these must match.");
        uint64_t m_inUseBitset[NUM_MASKS] = { 0 };

        Core::GpuMemory::Buffer m_buffer;
        Util::HashTable<Entry, uint32_t> m_materials;
        uint32 m_staleID = UINT32_MAX;
    };

    //--------------------------------------------------------------------------------------
    // MeshContainer
    //--------------------------------------------------------------------------------------

    struct MeshContainer
    {
        uint32_t Add(Util::SmallVector<Core::Vertex>&& vertices, Util::SmallVector<uint32_t>&& indices,
            uint32_t matIdx);
        void AddBatch(Util::SmallVector<Model::glTF::Asset::Mesh>&& meshes, 
            Util::SmallVector<Core::Vertex>&& vertices,
            Util::SmallVector<uint32_t>&& indices);
        void Reserve(size_t numVertices, size_t numIndices);
        void RebuildBuffers();
        void Clear();

        // Note: not thread safe for reading and writing at the same time
        ZetaInline Util::Optional<const Model::TriangleMesh*> GetMesh(uint64_t id) const
        {
            auto it = m_meshes.find(id);
            if (it)
                return it.value();

            return {};
        }

        const Core::GpuMemory::Buffer& GetVB() { return m_vertexBuffer; }
        const Core::GpuMemory::Buffer& GetIB() { return m_indexBuffer; }

    private:
        Util::HashTable<Model::TriangleMesh> m_meshes;
        Util::SmallVector<Core::Vertex> m_vertices;
        Util::SmallVector<uint32_t> m_indices;

        Core::GpuMemory::Buffer m_vertexBuffer;
        Core::GpuMemory::Buffer m_indexBuffer;
    };

    //--------------------------------------------------------------------------------------
    // EmissiveBuffer
    //--------------------------------------------------------------------------------------

    struct EmissiveBuffer
    {
        using Instance = Model::glTF::Asset::EmissiveInstance;

        EmissiveBuffer() = default;
        ~EmissiveBuffer() = default;

        EmissiveBuffer(const EmissiveBuffer&) = delete;
        EmissiveBuffer& operator=(const EmissiveBuffer&) = delete;

        ZetaInline uint32_t NumInstances() const { return (uint32_t)m_instances.size(); }
        ZetaInline uint32_t NumTriangles() const { return (uint32_t)m_trisCpu.size(); }
        ZetaInline Util::Span<Instance> Instances() { return m_instances; }
        ZetaInline Util::MutableSpan<RT::EmissiveTriangle> Triagnles() { return m_trisCpu; }
        ZetaInline bool HasStaleMaterials() const { return m_staleNumTris > 0; }
        ZetaInline bool TransformedToWorldSpace() const { return m_trisGpu.IsInitialized(); }
        ZetaInline Util::Optional<const Instance*> FindInstance(uint64_t ID)
        {
            auto it = m_idToIdxMap.find(ID);
            if (it)
                return &m_instances[*it.value()];

            return {};
        }

        // Assumes proper GPU synchronization has been performed
        void Clear();
        void Update(uint64_t instanceID, const Math::float3& emissiveFactor, float strength);
        void AddBatch(Util::SmallVector<Instance>&& instances,
            Util::SmallVector<RT::EmissiveTriangle>&& tris);
        void UploadToGPU();

    private:
        Util::SmallVector<Instance> m_instances;
        Util::SmallVector<RT::EmissiveTriangle> m_trisCpu;
        // Maps instance ID to index in m_instances
        Util::HashTable<uint32_t> m_idToIdxMap;
        Core::GpuMemory::Buffer m_trisGpu;
        uint32_t m_staleBaseOffset = UINT32_MAX;
        uint32_t m_staleNumTris = 0;
    };
}