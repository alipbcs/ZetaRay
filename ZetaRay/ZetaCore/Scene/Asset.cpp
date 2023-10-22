#include "Asset.h"
#include "../Core/RendererCore.h"
#include "../Core/SharedShaderResources.h"
#include "SceneRenderer.h"
#include "SceneCore.h"
#include "../Utility/Utility.h"
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
	// if the texture already exists, just increase the ref count and return it
	if (auto it = m_cache.find(id); it != nullptr)
	{
		const uint32_t offset = it->DescTableOffset;
		Assert(offset < m_descTableSize, "invalid offset.");
		it->RefCount++;

		return offset;
	}

	Assert(tex.IsInitialized(), "Texture hasn't been initialized.");

	// find the first free slot in the table
	DWORD freeSlot = DWORD(-1);
	int i = 0;
	for (; i < (int)m_numMasks; i++)
	{
		if (_BitScanForward64(&freeSlot, ~m_inUseBitset[i]))
			break;
	}

	Assert(freeSlot != DWORD(-1), "No free slot found");
	m_inUseBitset[i] |= (1llu << freeSlot);	// set the slot to occupied

	freeSlot += i * 64;		// each uint64_t covers 64 slots
	Assert(freeSlot < m_descTableSize, "Invalid table index.");

	// create the SRV
	auto descCpuHandle = m_descTable.CPUHandle(freeSlot);
	Direct3DUtil::CreateTexture2DSRV(tex, descCpuHandle);

	// add this texture to the cache
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
			// set the descriptor slot to free
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
	// Assumes GPU synchronization has been performed, so GPU is done with all the textures
	m_pending.clear();
	m_cache.clear();
	memset(m_inUseBitset, 0, m_numMasks * sizeof(uint64_t));
	m_descTable.Reset();
}

//--------------------------------------------------------------------------------------
// MaterialBuffer
//--------------------------------------------------------------------------------------

void MaterialBuffer::Init(uint64_t id)
{
	Assert(k_bufferID == -1, "This ID shouldn't be reassigned to after the first time.");
	k_bufferID = id;
}

void MaterialBuffer::Add(uint64_t id, Material& mat)
{
	// find first free slot in buffer (i.e. first-fit)
	DWORD freeIdx = DWORD(-1);
	int i = 0;
	for (; i < NUM_MASKS; i++)
	{
		if (_BitScanForward64(&freeIdx, ~m_inUseBitset[i]))
			break;
	}

	Assert(freeIdx != -1, "No free slot found");
	m_inUseBitset[i] |= (1llu << freeIdx);		// set the slot to occupied

	freeIdx += i << 6;		// each uint64_t covers 64 slots
	Assert(freeIdx < MAX_NUM_MATERIALS, "Invalid table index.");

	// set offset in input material
	mat.SetGpuBufferIndex(freeIdx);
	m_matTable.insert_or_assign(id, mat);

	m_stale = true;
}

void MaterialBuffer::UpdateGPUBufferIfStale()
{
	if (!m_stale)
		return;

	Assert(!m_matTable.empty(), "Stale flag is set, yet there aren't any materials.");

	auto it = m_matTable.begin_it();

	SmallVector<Material, FrameAllocator> buffer;
	buffer.resize(m_matTable.size());

	while (it != m_matTable.end_it())
	{
		const uint32_t indexInBuffer = it->Val.GpuBufferIndex();
		buffer[indexInBuffer] = it->Val;

		it = m_matTable.next_it(it);
	}

	auto& renderer = App::GetRenderer();

	m_buffer = GpuMemory::GetDefaultHeapBufferAndInit("MaterialBuffer",
		(uint32_t)(buffer.size() * sizeof(Material)),
		false,
		buffer.data());

	auto& r = renderer.GetSharedShaderResources();
	r.InsertOrAssignDefaultHeapBuffer(k_bufferID, m_buffer);

	m_stale = false;
}

void MaterialBuffer::Recycle(uint64_t completedFenceVal)
{
	for (auto it = m_pending.begin(); it != m_pending.end();)
	{
		// GPU is finished with this material
		if (it->FenceVal <= completedFenceVal)
		{
			// set the slot to free
			const int idx = it->Offset >> 6;
			Assert(idx < NUM_MASKS, "Invalid index.");
			m_inUseBitset[idx] |= (1llu << (it->Offset & 63));

			it = m_pending.erase(*it);
		}
		else
			it++;
	}
}

void MaterialBuffer::Clear()
{
	// Assumes CPU-GPU synchronization has been performed, so that GPU is done with the material buffer.
	// DefaultHeapBuffer's destructor takes care of the rest
	m_buffer.Reset();
}

//--------------------------------------------------------------------------------------
// MeshContainer
//--------------------------------------------------------------------------------------

void MeshContainer::Add(uint64_t id, Span<Vertex> vertices, Span<uint32_t> indices, uint64_t matID)
{
	const size_t vtxOffset = m_vertices.size();
	const size_t idxOffset = m_indices.size();

	m_meshes.emplace(id, vertices, vtxOffset, idxOffset, (uint32_t)indices.size(), matID);

	m_vertices.append_range(vertices.begin(), vertices.end());
	m_indices.append_range(indices.begin(), indices.end());
}

void MeshContainer::AddBatch(uint64_t sceneID, SmallVector<Model::glTF::Asset::Mesh>&& meshes, 
	SmallVector<Core::Vertex>&& vertices, SmallVector<uint32_t>&& indices)
{
	const size_t vtxOffset = m_vertices.size();
	const size_t idxOffset = m_indices.size();

	for (auto& mesh : meshes)
	{
		const uint64_t meshFromSceneID = SceneCore::MeshID(sceneID, mesh.MeshIdx, mesh.MeshPrimIdx);
		const uint64_t matFromSceneID = mesh.MaterialIdx != -1 ? SceneCore::MaterialID(sceneID, mesh.MaterialIdx) : SceneCore::DEFAULT_MATERIAL;

		m_meshes.emplace(meshFromSceneID, Span(vertices.begin() + mesh.BaseVtxOffset, mesh.NumVertices), 
			vtxOffset + mesh.BaseVtxOffset,
			idxOffset + mesh.BaseIdxOffset,
			mesh.NumIndices, 
			matFromSceneID);
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
	m_meshes.clear();
	m_vertexBuffer.Reset();
	m_indexBuffer.Reset();

	m_vertices.free_memory();
	m_indices.free_memory();
}

//--------------------------------------------------------------------------------------
// EmissiveBuffer
//--------------------------------------------------------------------------------------

void EmissiveBuffer::AddBatch(SmallVector<Asset::EmissiveInstance>&& emissiveInstances, SmallVector<RT::EmissiveTriangle>&& emissiveTris)
{
	if (m_emissivesTrisCpu.empty())
	{
		m_emissivesTrisCpu = ZetaMove(emissiveTris);
		m_emissivesInstances = ZetaMove(emissiveInstances);
	}
	else
	{
		// todo implement
		Check(false, "not supported yet.");
	}

	std::sort(m_emissivesInstances.begin(), m_emissivesInstances.end(),
		[](const Asset::EmissiveInstance& a1, const Asset::EmissiveInstance& a2)
		{
			return a1.InstanceID < a2.InstanceID;
		});
}

void EmissiveBuffer::RebuildEmissiveBuffer()
{
	if (m_emissivesTrisCpu.empty())
		return;

	const uint32_t sizeInBytes = sizeof(RT::EmissiveTriangle) * (uint32_t)m_emissivesTrisCpu.size();
	m_emissiveTrisGpu = GpuMemory::GetDefaultHeapBufferAndInit(GlobalResource::EMISSIVE_TRIANGLE_BUFFER,
		sizeInBytes,
		false, 
		m_emissivesTrisCpu.data());

	auto& r = App::GetRenderer().GetSharedShaderResources();
	r.InsertOrAssignDefaultHeapBuffer(GlobalResource::EMISSIVE_TRIANGLE_BUFFER, m_emissiveTrisGpu);

	m_rebuildFlag = false;
}

void EmissiveBuffer::Clear()
{
	m_emissiveTrisGpu.Reset();
	//m_emissivesTrisCpu.free_memory();
}

Optional<Asset::EmissiveInstance*> EmissiveBuffer::FindEmissive(uint64_t ID)
{
	auto idx = BinarySearch(Span(m_emissivesInstances), ID, [](const Asset::EmissiveInstance& e) {return e.InstanceID; });
	if (idx != -1)
		return &m_emissivesInstances[idx];

	return {};
}
