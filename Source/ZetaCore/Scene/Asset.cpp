#include "Asset.h"
#include "../Core/RendererCore.h"
#include "../Core/SharedShaderResources.h"
#include "SceneRenderer.h"
#include "SceneCore.h"
#include "../Utility/Utility.h"
#include "../App/Log.h"
#include "../App/Timer.h"
#include <algorithm>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Scene::Internal;
using namespace ZetaRay::App;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;
using namespace ZetaRay::Model;
using namespace ZetaRay::Model::glTF;

//--------------------------------------------------------------------------------------
// TexSRVDescriptorTable
//--------------------------------------------------------------------------------------

TexSRVDescriptorTable::TexSRVDescriptorTable(const uint32_t descTableSize)
    : m_descTableSize(descTableSize),
    m_numMasks(descTableSize >> 6)
{
    Assert(Math::IsPow2(descTableSize), "descriptor table size must be a power of two.");
}

void TexSRVDescriptorTable::Init(uint64_t id)
{
    m_descTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate(m_descTableSize);
    Assert(!m_descTable.IsEmpty(), "Allocating descriptors from the GPU descriptor heap failed.");

    auto& s = App::GetRenderer().GetSharedShaderResources();
    s.InsertOrAssingDescriptorTable(id, m_descTable);
}

uint32_t TexSRVDescriptorTable::Add(Texture&& tex, uint64_t id)
{
    // If the texture already exists, just increase the ref count and return it
    if (auto it = m_cache.find(id); it != nullptr)
    {
        const uint32_t offset = it->DescTableOffset;
        Assert(offset < m_descTableSize, "invalid offset.");
        it->RefCount++;

        return offset;
    }

    Assert(tex.IsInitialized(), "Texture hasn't been initialized.");

    // Find the first free slot in the table
    DWORD freeSlot = DWORD(-1);
    int i = 0;
    for (; i < (int)m_numMasks; i++)
    {
        if (_BitScanForward64(&freeSlot, ~m_inUseBitset[i]))
            break;
    }

    Assert(freeSlot != DWORD(-1), "No free slot found");
    m_inUseBitset[i] |= (1llu << freeSlot);    // Set the slot to occupied

    freeSlot += i * 64;        // Each uint64_t covers 64 slots
    Assert(freeSlot < m_descTableSize, "Invalid table index.");

    auto descCpuHandle = m_descTable.CPUHandle(freeSlot);
    Direct3DUtil::CreateTexture2DSRV(tex, descCpuHandle);

    // Add this texture to the cache
    m_cache.insert_or_assign(id, CacheEntry{ 
        .T = ZetaMove(tex), 
        .DescTableOffset = freeSlot,
        .RefCount = 1 });

    return freeSlot;
}

void TexSRVDescriptorTable::Recycle(uint64_t completedFenceVal)
{
    for(auto it = m_pending.begin(); it != m_pending.end();)
    {
        // GPU is finished with this descriptor
        if (it->FenceVal <= completedFenceVal)
        {
            // Set the descriptor slot to free
            const uint32_t idx = it->DescTableOffset >> 6;
            Assert(idx < m_numMasks, "invalid index.");
            m_inUseBitset[idx] |= (1llu << (it->DescTableOffset & 63));

            it = m_pending.erase(*it);
        }
        else
            it++;
    }
}

void TexSRVDescriptorTable::Clear()
{
    for (auto it = m_cache.begin_it(); it < m_cache.end_it(); it = m_cache.next_it(it))
        it->Val.T.Reset(false);

    for (auto& t : m_pending)
        t.T.Reset(false);
}

//--------------------------------------------------------------------------------------
// MaterialBuffer
//--------------------------------------------------------------------------------------

uint32_t MaterialBuffer::Add(Material& mat)
{
    uint32_t idx = (uint32_t)m_materials.size() - 1;
    Add(mat, idx);

    return idx;
}

void MaterialBuffer::Add(Material& mat, uint32_t idx)
{
    // Find first free slot in buffer (i.e. first-fit)
    DWORD freeIdx = DWORD(-1);
    int i = 0;
    for (; i < NUM_MASKS; i++)
    {
        if (_BitScanForward64(&freeIdx, ~m_inUseBitset[i]))
            break;
    }

    Assert(freeIdx != -1, "No free slot found.");
    m_inUseBitset[i] |= (1llu << freeIdx);        // Set the slot to occupied

    freeIdx += i << 6;        // Each uint64_t covers 64 slots
    Assert(freeIdx < MAX_NUM_MATERIALS, "Invalid table index.");

    // Set offset in input material
    mat.SetGpuBufferIndex(freeIdx);
    m_materials[idx] = mat;
}

void MaterialBuffer::UploadToGPU()
{
    // First time
    if (!m_buffer.IsInitialized())
    {
        SmallVector<Material, FrameAllocator> buffer;
        buffer.resize(m_materials.size());

        for(auto& mat : m_materials)
        {
            const uint32_t indexInBuffer = mat.GpuBufferIndex();
            buffer[indexInBuffer] = mat;
        }

        auto& renderer = App::GetRenderer();
        m_buffer = GpuMemory::GetDefaultHeapBufferAndInit("MaterialBuffer",
            (uint32_t)(buffer.size() * sizeof(Material)),
            false,
            buffer.data());

        auto& r = renderer.GetSharedShaderResources();
        r.InsertOrAssignDefaultHeapBuffer(GlobalResource::MATERIAL_BUFFER, m_buffer);
    }
    // Update a single material
    else if (m_staleIdx != -1)
    {
        GpuMemory::UploadToDefaultHeapBuffer(m_buffer, sizeof(Material),
            &m_materials[m_staleIdx], sizeof(Material) * m_staleIdx);

        m_staleIdx = -1;
    }
}

void MaterialBuffer::ResizeAdditionalMaterials(uint32_t num)
{
    m_materials.resize(m_materials.size() + num);
}

void MaterialBuffer::Clear()
{
    // Assumes CPU-GPU synchronization has been performed, so that GPU is done with the material buffer.
    m_buffer.Reset();
}

//--------------------------------------------------------------------------------------
// MeshContainer
//--------------------------------------------------------------------------------------

uint32_t MeshContainer::Add(SmallVector<Core::Vertex>&& vertices, SmallVector<uint32_t>&& indices,
    uint32_t matIdx)
{
    const uint32_t vtxOffset = (uint32_t)m_vertices.size();
    const uint32_t idxOffset = (uint32_t)m_indices.size();

    const uint32_t meshIdx = (uint32_t)m_meshes.size();
    const uint64_t meshFromSceneID = SceneCore::MeshID(meshIdx, 0);
    bool success = m_meshes.try_emplace(meshFromSceneID, vertices, vtxOffset, idxOffset, 
        (uint32_t)indices.size(), matIdx);
    Check(success, "mesh with given ID (from mesh index %u) already exists.", meshIdx);

    m_vertices.append_range(vertices.begin(), vertices.end());
    m_indices.append_range(indices.begin(), indices.end());

    return meshIdx;
}

void MeshContainer::AddBatch(SmallVector<Model::glTF::Asset::Mesh>&& meshes, 
    SmallVector<Core::Vertex>&& vertices, SmallVector<uint32_t>&& indices)
{
    const uint32_t vtxOffset = (uint32_t)m_vertices.size();
    const uint32_t idxOffset = (uint32_t)m_indices.size();
    m_meshes.resize(meshes.size(), true);

    // Each mesh primitive + material index combo must be unique
    for (auto& mesh : meshes)
    {
        const uint64_t meshFromSceneID = SceneCore::MeshID(mesh.MeshIdx, mesh.MeshPrimIdx);
        const uint32_t matFromSceneID = mesh.glTFMaterialIdx != -1 ?
            // Offset by one to account for default material at slot 0.
            mesh.glTFMaterialIdx + 1 :
            SceneCore::DEFAULT_MATERIAL_IDX;

        bool success = m_meshes.try_emplace(meshFromSceneID, Span(vertices.begin() + mesh.BaseVtxOffset, mesh.NumVertices),
            vtxOffset + mesh.BaseVtxOffset,
            idxOffset + mesh.BaseIdxOffset,
            mesh.NumIndices, 
            matFromSceneID);

        Assert(success, "Mesh with ID %llu already exists.", meshFromSceneID);
    }

    if (m_vertices.empty())
        m_vertices = ZetaMove(vertices);
    else
        m_vertices.append_range(vertices.begin(), vertices.end());

    if (m_indices.empty())
        m_indices = ZetaMove(indices);
    else
        m_indices.append_range(indices.begin(), indices.end());
}

void MeshContainer::Reserve(size_t numVertices, size_t numIndices)
{
    m_vertices.reserve(numVertices);
    m_indices.reserve(numIndices);
}

void MeshContainer::RebuildBuffers()
{
    Assert(m_vertices.size() > 0, "vertex buffer is empty");
    Assert(m_indices.size() > 0, "index buffer is empty");

    const uint32_t vbSizeInBytes = sizeof(Vertex) * (uint32_t)m_vertices.size();
    m_vertexBuffer = GpuMemory::GetDefaultHeapBufferAndInit(GlobalResource::SCENE_VERTEX_BUFFER, vbSizeInBytes,
        false, m_vertices.data(), true);

    const uint32_t ibSizeInBytes = sizeof(uint32_t) * (uint32_t)m_indices.size();
    m_indexBuffer = GpuMemory::GetDefaultHeapBufferAndInit(GlobalResource::SCENE_INDEX_BUFFER, ibSizeInBytes,
        false, m_indices.data(), true);

    auto& r = App::GetRenderer().GetSharedShaderResources();
    r.InsertOrAssignDefaultHeapBuffer(GlobalResource::SCENE_VERTEX_BUFFER, m_vertexBuffer);
    r.InsertOrAssignDefaultHeapBuffer(GlobalResource::SCENE_INDEX_BUFFER, m_indexBuffer);

    m_vertices.free_memory();
    m_indices.free_memory();
}

void MeshContainer::Clear()
{
    m_vertexBuffer.Reset(false);
    m_indexBuffer.Reset(false);
}

//--------------------------------------------------------------------------------------
// EmissiveBuffer
//--------------------------------------------------------------------------------------

void EmissiveBuffer::AddBatch(SmallVector<Asset::EmissiveInstance>&& instances, 
    SmallVector<RT::EmissiveTriangle>&& tris)
{
    App::DeltaTimer timer;
    timer.Start();

    // TODO implement
    Check(m_trisCpu.empty(), "Not implemented.");
    m_instances = instances;

    // Map instance ID to index in instances
    HashTable<uint32, uint64, App::FrameAllocator> idToIdxMap;
    idToIdxMap.resize(instances.size(), true);

    for (int i = 0; i < (int)instances.size(); i++)
        idToIdxMap[instances[i].InstanceID] = i;

    // Sort by material - this makes updates simpler and faster
    std::sort(m_instances.begin(), m_instances.end(),
        [](const Asset::EmissiveInstance& a1, const Asset::EmissiveInstance& a2)
        {
            return a1.MaterialIdx < a2.MaterialIdx;
        });

    m_trisCpu.resize(tris.size());
    uint32_t currNumTris = 0;

    // Shuffle triangles according to new sorted order
    for (int i = 0; i < (int)m_instances.size(); i++)
    {
        const uint64_t currID = m_instances[i].InstanceID;
        const uint32_t idx = *idToIdxMap.find2(currID).value();

        memcpy(&m_trisCpu[currNumTris], 
            &tris[instances[idx].BaseTriOffset],
            instances[idx].NumTriangles * sizeof(RT::EmissiveTriangle));

        m_instances[i].BaseTriOffset = currNumTris;
        currNumTris += instances[idx].NumTriangles;
    }

    for (int i = 0; i < (int)m_instances.size(); i++)
        m_idToIdxMap[m_instances[i].InstanceID] = i;

    timer.End();
    LOG_UI_INFO("Emissive buffers processed in %u [us].", (uint32_t)timer.DeltaMicro());
}

void EmissiveBuffer::UploadToGPU()
{
    if (m_trisCpu.empty())
        return;

    if (!m_trisGpu.IsInitialized())
    {
        const size_t sizeInBytes = sizeof(RT::EmissiveTriangle) * m_trisCpu.size();
        m_trisGpu = GpuMemory::GetDefaultHeapBufferAndInit(GlobalResource::EMISSIVE_TRIANGLE_BUFFER,
            (uint32)sizeInBytes,
            false, 
            m_trisCpu.data());

        auto& r = App::GetRenderer().GetSharedShaderResources();
        r.InsertOrAssignDefaultHeapBuffer(GlobalResource::EMISSIVE_TRIANGLE_BUFFER, m_trisGpu);
    }
    else if(m_staleNumTris > 0)
    {
        Assert(m_staleBaseOffset != UINT32_MAX, "Invalid base offset.");
        LOG_UI_INFO("Frame %llu: Uploading %d emissive triangles (%d MB)...", 
            App::GetTimer().GetTotalFrameCount(),
            m_staleNumTris, sizeof(RT::EmissiveTriangle) * m_staleNumTris / (1024 * 1024));


        GpuMemory::UploadToDefaultHeapBuffer(m_trisGpu, sizeof(RT::EmissiveTriangle) * m_staleNumTris,
            &m_trisCpu[m_staleBaseOffset], m_staleBaseOffset * sizeof(RT::EmissiveTriangle));

        m_staleNumTris = 0;
        m_staleBaseOffset = UINT32_MAX;
    }
}

void EmissiveBuffer::Clear()
{
    m_trisGpu.Reset(false);
}

void EmissiveBuffer::Update(uint64_t instanceID, const float3& emissiveFactor, float strength)
{
    auto& instance = *FindInstance(instanceID).value();
    const int modifiedMatIdx = instance.MaterialIdx;

    // BinarySearch() must return the leftmost index when keys aren't unique
    auto idx = BinarySearch(Span(m_instances), modifiedMatIdx,
        [](const Asset::EmissiveInstance& e) { return e.MaterialIdx; });
    Assert(idx != -1, "Material was not found.");

    const uint32 newEmissiveFactor = Float3ToRGB8(emissiveFactor);
    const half newStrength(strength);

    m_staleBaseOffset = m_instances[idx].BaseTriOffset;

    // Find every instance that uses this material
    while (idx < (int64)m_instances.size() && m_instances[idx].MaterialIdx == modifiedMatIdx)
    {
        // Update all triangles for this instance
        for (int64 i = m_instances[idx].BaseTriOffset; 
            i < m_instances[idx].BaseTriOffset + m_instances[idx].NumTriangles; i++)
        {
            m_trisCpu[i].SetEmissiveFactor(newEmissiveFactor);
            m_trisCpu[i].SetStrength(newStrength);
        }

        m_staleNumTris += m_instances[idx].NumTriangles;
        idx++;
    } 
}
