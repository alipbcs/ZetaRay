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
    // MaterialBuffer: wrapper over a GPU buffer containing all the materials required
    // for rendering.
    //--------------------------------------------------------------------------------------

    struct MaterialBuffer
    {
        MaterialBuffer() = default;
        ~MaterialBuffer() = default;

        MaterialBuffer(const MaterialBuffer&) = delete;
        MaterialBuffer& operator=(const MaterialBuffer&) = delete;

        uint32_t Add(Material& mat);
        void Add(Material& mat, uint32_t idx);
        void UpdateGPUBufferIfStale();
        void ResizeAdditionalMaterials(uint32_t num);

        ZetaInline Util::Optional<const Material*> Get(uint32_t idx) const
        {
            if(idx >= m_materials.size())
                return {};

            return &m_materials[idx];
        }

        void Clear();

    private:
        static constexpr int MAX_NUM_MATERIALS = 2048;
        static constexpr int NUM_MASKS = MAX_NUM_MATERIALS >> 6;
        static_assert(NUM_MASKS * 64 == MAX_NUM_MATERIALS, "these must match.");
        uint64_t m_inUseBitset[NUM_MASKS] = { 0 };

        Core::GpuMemory::DefaultHeapBuffer m_buffer;
        Util::SmallVector<Material> m_materials;

        bool m_stale = false;
    };

    //--------------------------------------------------------------------------------------
    // MeshContainer
    //--------------------------------------------------------------------------------------

    struct MeshContainer
    {
        uint32_t Add(Util::SmallVector<Core::Vertex>&& vertices, Util::SmallVector<uint32_t>&& indices, 
            uint32_t matIdx);
        void AddBatch(Util::SmallVector<Model::glTF::Asset::Mesh>&& meshes, Util::SmallVector<Core::Vertex>&& vertices,
            Util::SmallVector<uint32_t>&& indices);
        void Reserve(size_t numVertices, size_t numIndices);
        void RebuildBuffers();

        // Note: not thread safe
        ZetaInline Util::Optional<const Model::TriangleMesh*> GetMesh(uint64_t id) const
        {
            auto mesh = m_meshes.find(id);
            if (mesh)
                return mesh;

            return {};
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
        struct InitialPos
        {
            Math::float3 Vtx0;
            Math::float3 Vtx1;
            Math::float3 Vtx2;
        };

        EmissiveBuffer() = default;
        ~EmissiveBuffer() = default;

        EmissiveBuffer(const EmissiveBuffer&) = delete;
        MaterialBuffer& operator=(const EmissiveBuffer&) = delete;

        bool IsFirstTime() const { return m_firstTime; }
        void Clear();
        Util::Optional<Model::glTF::Asset::EmissiveInstance*> FindEmissive(uint64_t ID);
        void AddBatch(Util::SmallVector<Model::glTF::Asset::EmissiveInstance>&& emissiveInstance, 
            Util::SmallVector<RT::EmissiveTriangle>&& emissiveTris);
        void AllocateAndCopyEmissiveBuffer();
        void UpdateEmissiveBuffer(uint32_t minTriIdx, uint32_t maxTriIdx);
        ZetaInline uint32_t NumEmissiveInstances() const { return (uint32_t)m_emissivesInstances.size(); }
        ZetaInline uint32_t NumEmissiveTriangles() const { return (uint32_t)m_emissivesTrisCpu.size(); }

        Util::Span<Model::glTF::Asset::EmissiveInstance> EmissiveInstances() { return m_emissivesInstances; }
        Util::MutableSpan<InitialPos> InitialTriVtxPos() { return m_initialPos; }
        Util::MutableSpan<RT::EmissiveTriangle> EmissiveTriagnles() { return m_emissivesTrisCpu; }

    private:
        Util::SmallVector<Model::glTF::Asset::EmissiveInstance> m_emissivesInstances;
        Util::SmallVector<RT::EmissiveTriangle> m_emissivesTrisCpu;
        Util::SmallVector<InitialPos> m_initialPos;
        Core::GpuMemory::DefaultHeapBuffer m_emissiveTrisGpu;
        bool m_firstTime = true;
    };
}