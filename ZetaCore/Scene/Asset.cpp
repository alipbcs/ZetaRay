#include "Asset.h"
#include "../Core/DescriptorHeap.h"
#include "../Core/Direct3DHelpers.h"
#include "../Core/RendererCore.h"
#include "../Core/GpuMemory.h"
#include "../Core/SharedShaderResources.h"
#include "../App/Filesystem.h"
#include "SceneRenderer.h"
#include "SceneCore.h"
#include "../Model/glTFAsset.h"

using namespace ZetaRay::Core;
using namespace ZetaRay::Scene::Internal;
using namespace ZetaRay::App;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;
using namespace ZetaRay::Model;

//--------------------------------------------------------------------------------------
// TexSRVDescriptorTable
//--------------------------------------------------------------------------------------
				
TexSRVDescriptorTable::TexSRVDescriptorTable(const uint32_t descTableSize) noexcept
	: m_descTableSize(descTableSize),
	m_numMasks(descTableSize >> 6)
{
	Assert(Math::IsPow2(descTableSize), "descriptor table size must be a power of two.");
}

void TexSRVDescriptorTable::Init(uint64_t id) noexcept
{
	m_descTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(m_descTableSize);
	Assert(!m_descTable.IsEmpty(), "Allocating descriptors from the GPU descriptor heap failed.");

	auto& s = App::GetRenderer().GetSharedShaderResources();
	s.InsertOrAssingDescriptorTable(id, m_descTable);
}

uint32_t TexSRVDescriptorTable::Add(Core::Texture&& tex, uint64_t id) noexcept
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
	auto descTpuHandle = m_descTable.CPUHandle(freeSlot);
	Direct3DHelper::CreateTexture2DSRV(tex, descTpuHandle);

	// add this texture to the cache
	m_cache.insert_or_assign(id, CacheEntry{ 
		.T = ZetaMove(tex), 
		.DescTableOffset = freeSlot,
		.RefCount = 1 });

	return freeSlot;
}

void TexSRVDescriptorTable::Recycle(uint64_t completedFenceVal) noexcept
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

void TexSRVDescriptorTable::Clear() noexcept
{
	//auto& s = App::GetRenderer().GetSharedShaderResources();
	//s.RemoveDescriptorTable(ID);

	// Assumes GPU synchronization has been performed, so GPU is done with all the textures
	m_pending.clear();
	m_cache.clear();
	memset(m_inUseBitset, 0, m_numMasks * sizeof(uint64_t));
	m_descTable.Reset();
}

//void TexSRVDescriptorTable::Remove(uint64_t id, uint64_t nextFenceVal) noexcept
//{
//	auto it = Cache.find(id);
//	Check(it != nullptr, "Invalid texture ID %llu", id);
//	Check(it->TableOffset < NUM_DESCRIPTORS, "Descriptor table offset %u was out-of-bounds.", NUM_DESCRIPTORS);
//	it->RefCount--;
//
//	TODO remove the corresponding bit from the bitmask
// 
//	// no more references to this texture
//	if (it->RefCount == 0)
//	{
//		auto numErased = Cache.erase(id);
//		Assert(numErased == 1, "Number of removed elements must be exactly one.");
//
//		Pending.emplace_back(ToBeFreedTexture{
//			.T = ZetaMove(it->second.T), 
//			.FenceVal = nextFenceVal, 
//			.TableOffset = it->second.TableOffset});
//	}
//}

//--------------------------------------------------------------------------------------
// MaterialBuffer
//--------------------------------------------------------------------------------------

void MaterialBuffer::Init(uint64_t id) noexcept
{
	Assert(k_bufferID == -1, "This ID shouldn't be reassigned to after the first time.");
	k_bufferID = id;
}

void MaterialBuffer::Add(uint64_t id, Material& mat) noexcept
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

void MaterialBuffer::UpdateGPUBufferIfStale() noexcept
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

	m_buffer = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit("MaterialBuffer",
		buffer.size() * sizeof(Material), 
		D3D12_RESOURCE_STATE_COMMON, 
		false,
		buffer.data());

	auto& r = renderer.GetSharedShaderResources();
	r.InsertOrAssignDefaultHeapBuffer(k_bufferID, m_buffer);

	m_stale = false;
}

void MaterialBuffer::Recycle(uint64_t completedFenceVal) noexcept
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

void MaterialBuffer::Clear() noexcept
{
	//auto& s = App::GetRenderer().GetSharedShaderResources();
	//s.RemoveDescriptorTable(ID);

	// Assumes CPU-GPU synchronization has been performed, so that GPU is done with the material buffer.
	// UploadHeapBuffer's destructor takes care of the rest
	m_buffer.Reset();
}

//void MaterialBuffer::Remove(uint64_t id, uint64_t nextFenceVal) noexcept
//{
//	auto it = Materials.find(id);
//	Assert(it != nullptr, "Attempting to remove material %s that doesn't exist.", id);
//
//	Pending.emplace_back(ToBeRemoved{ .FenceVal = nextFenceVal, .Offset = (uint16_t)it->GpuBufferIndex()});
//	Materials.erase(it);
//}

//--------------------------------------------------------------------------------------
// MeshContainer
//--------------------------------------------------------------------------------------

void MeshContainer::Add(uint64_t id, Span<Vertex> vertices, Span<uint32_t> indices, uint64_t matID) noexcept
{
	const size_t vtxOffset = m_vertices.size();
	const size_t idxOffset = m_indices.size();

	m_meshes.emplace(id, vertices, vtxOffset, idxOffset, (uint32_t)indices.size(), matID);

	m_vertices.append_range(vertices.begin(), vertices.end());
	m_indices.append_range(indices.begin(), indices.end());
}

void MeshContainer::AddBatch(uint64_t sceneID, SmallVector<Model::glTF::Asset::MeshSubset>&& meshes, 
	SmallVector<Core::Vertex>&& vertices, SmallVector<uint32_t>&& indices) noexcept
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

void MeshContainer::Reserve(size_t numVertices, size_t numIndices) noexcept
{
	m_vertices.reserve(numVertices);
	m_indices.reserve(numIndices);
}

void MeshContainer::RebuildBuffers() noexcept
{
	Assert(m_vertices.size() > 0, "vertex buffer is empty");
	Assert(m_indices.size() > 0, "index buffer is empty");

	const size_t vbSizeInBytes = sizeof(Vertex) * m_vertices.size();
	m_vertexBuffer = App::GetRenderer().GetGpuMemory().GetDefaultHeapBufferAndInit(GlobalResource::SCENE_VERTEX_BUFFER, vbSizeInBytes,
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false, m_vertices.data());

	const size_t ibSizeInBytes = sizeof(uint32_t) * m_indices.size();
	m_indexBuffer = App::GetRenderer().GetGpuMemory().GetDefaultHeapBufferAndInit(GlobalResource::SCENE_INDEX_BUFFER, ibSizeInBytes,
		D3D12_RESOURCE_STATE_INDEX_BUFFER, false, m_indices.data());

	auto& r = App::GetRenderer().GetSharedShaderResources();
	r.InsertOrAssignDefaultHeapBuffer(GlobalResource::SCENE_VERTEX_BUFFER, m_vertexBuffer);
	r.InsertOrAssignDefaultHeapBuffer(GlobalResource::SCENE_INDEX_BUFFER, m_indexBuffer);

	m_vertices.free_memory();
	m_indices.free_memory();
}

void MeshContainer::Clear() noexcept
{
	m_meshes.clear();
	m_vertexBuffer.Reset();
	m_indexBuffer.Reset();

	m_vertices.free_memory();
	m_indices.free_memory();
}

