#include "AssetManager.h"
#include "../RenderPass/Common/Material.h"
#include "../Core/DescriptorHeap.h"
#include "../Core/Renderer.h"
#include "../Core/GpuMemory.h"
#include "../Core/SharedShaderResources.h"
#include "../Win32/App.h"

using namespace ZetaRay;
using namespace ZetaRay::Internal;
using namespace ZetaRay::Win32;

//--------------------------------------------------------------------------------------
// TextureDescriptorTable
//--------------------------------------------------------------------------------------
				
TexSRVDescriptorTable::TexSRVDescriptorTable(uint64_t id) noexcept
	: ID(id)
{
}

void TexSRVDescriptorTable::Init() noexcept
{
	Table = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(NUM_DESCRIPTORS);
	Assert(!Table.IsEmpty(), "Allocating descriptors from the GPU descriptor heap failed.");

	auto& s = App::GetRenderer().GetSharedShaderResources();
	s.InsertOrAssingDescriptorTable(ID, Table);
}

uint32_t TexSRVDescriptorTable::Add(const Filesystem::Path& p, uint64_t id) noexcept
{				
	if (auto it = Cache.find(id); it != nullptr)
	{
		uint32_t offset = it->TableOffset;
		Assert(offset != -1, "invalid offset.");
		it->RefCount++;

		return offset;
	}

	// texture needs to be created
	Texture tex = App::GetRenderer().GetGpuMemory().GetTexture2DFromDisk(p.Get());
	auto* device = App::GetRenderer().GetDevice();

	// find the first free slot in the table
	DWORD freeSlot = -1;
	int i = 0;
	for (; i < NUM_MASKS; i++)
	{
		if (_BitScanForward64(&freeSlot, ~Bitset[i]))
			break;
	}

	Assert(freeSlot != -1, "No free-slot found");
	Bitset[i] |= (1llu << freeSlot);

	freeSlot += i * 64;		// each uint64_t covers 64 slots
	Assert(freeSlot < NUM_DESCRIPTORS, "Invalid table index.");

	// create the SRV
	const auto desc = tex.GetResource()->GetDesc();

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = desc.MipLevels;

	device->CreateShaderResourceView(tex.GetResource(), &srvDesc, Table.CPUHandle(freeSlot));

	// add this texture to the cache
	Cache.emplace_or_assign(id, CacheEntry{ .T = ZetaMove(tex), .TableOffset = freeSlot, .RefCount = 1 });

	return freeSlot;
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

void TexSRVDescriptorTable::Recycle(ID3D12Fence* fence) noexcept
{
	const uint64_t completed = fence->GetCompletedValue();

	for(auto it = Pending.begin(); it != Pending.end();)
	{
		// GPU is finished with this descriptor
		if (it->FenceVal <= completed)
		{
			// set the descriptor slot to free
			int idx = it->TableOffset >> 6;
			Assert(idx < NUM_MASKS, "Invalid index.");
			Bitset[idx] |= (1llu << (it->TableOffset & 63));

			it = Pending.erase(*it);
		}
		else
			it++;
	}
}

void TexSRVDescriptorTable::Clear() noexcept
{
	Assert(!Table.IsEmpty(), "");

	//auto& s = App::GetRenderer().GetSharedShaderResources();
	//s.RemoveDescriptorTable(ID);

	// Assumes GPU synchronization has been performed, so GPU is done with all the textures
	Pending.clear();
	Cache.clear();
	memset(Bitset, 0, NUM_MASKS * sizeof(uint64_t));
	Table.Reset();
}

//--------------------------------------------------------------------------------------
// MaterialBuffer
//--------------------------------------------------------------------------------------

MaterialBuffer::MaterialBuffer(uint64_t id) noexcept
	: ID(id)
{
}

void MaterialBuffer::Init() noexcept
{
	constexpr size_t s = NUM_MATERIALS * sizeof(Material);
	Buffer = App::GetRenderer().GetGpuMemory().GetUploadHeapBuffer(s);

	auto& r = App::GetRenderer().GetSharedShaderResources();
	r.InsertOrAssingUploadHeapBuffer(ID, Buffer);
}

void MaterialBuffer::Add(uint64_t id, Material& mat) noexcept
{
	// find the first free slot in the buffer
	DWORD freeIdx = -1;
	int i = 0;
	for (; i < NUM_MASKS; i++)
	{
		if (_BitScanForward64(&freeIdx, ~Bitset[i]))
			break;
	}

	Assert(freeIdx != -1, "No free-slot found");
	Bitset[i] |= (1llu << freeIdx);

	freeIdx += i << 6;		// each uint64_t covers 64 slots
	Assert(freeIdx < NUM_MATERIALS, "Invalid table index.");

	// set the offset
	mat.SetGpuBufferIndex(freeIdx);

	// copy over material data to the GPU buffer
	Buffer.Copy(freeIdx * sizeof(Material), sizeof(Material), reinterpret_cast<void*>(&mat));
	MaterialTable.emplace_or_assign(id, mat);
}

//void MaterialBuffer::Remove(uint64_t id, uint64_t nextFenceVal) noexcept
//{
//	auto it = Materials.find(id);
//	Assert(it != nullptr, "Attempting to remove material %s that doesn't exist.", id);
//
//	Pending.emplace_back(ToBeRemoved{ .FenceVal = nextFenceVal, .Offset = (uint16_t)it->GpuBufferIndex()});
//	Materials.erase(it);
//}

void MaterialBuffer::Recycle(ID3D12Fence* fence) noexcept
{
	const uint64_t completed = fence->GetCompletedValue();

	for (auto it = Pending.begin(); it != Pending.end();)
	{
		// GPU is finished with this material
		if (it->FenceVal <= completed)
		{
			// set the buffer slot to free
			int idx = it->Offset >> 6;
			Assert(idx < NUM_MASKS, "Invalid index.");
			Bitset[idx] |= (1llu << (it->Offset & 63));

			it = Pending.erase(*it);
		}
		else
			it++;
	}
}

void MaterialBuffer::Clear() noexcept
{
	//auto& s = App::GetRenderer().GetSharedShaderResources();
	//s.RemoveDescriptorTable(ID);

	// Assumes GPU synchronization has been performed, so GPU is done with the material buffer.
	// Furthermore, UploadHeapBuffer destructs with guard anyway
	Buffer.Reset();
}

//--------------------------------------------------------------------------------------
// MeshManager
//--------------------------------------------------------------------------------------

void MeshManager::Add(uint64_t id, 
	Vector<VertexPosNormalTexTangent>&& vertices,
	Vector<INDEX_TYPE>&& indices,
	uint64_t matID) noexcept
{
	m_meshes.emplace_or_assign(id, ZetaMove(vertices), ZetaMove(indices), matID);
}

/*
void MeshManager::Remove(uint64_t id, uint64_t nextFenceVal) noexcept
{
	const int sortedIdx = Find(id);
	if (sortedIdx == -1)
		return;

	const int meshesIdx = m_sortedIDs[sortedIdx];
	std::swap(m_meshes[meshesIdx], m_meshes[m_meshes.size() - 1]);

	// find position in m_sorted that contains m_sorted.size() - 1,
	// then swap it with the last element so that the first m_sorted.size() - 1
	// elements only contains numbers in range [0, m_sorted.size() - 1)
	for (int i = 0; i < m_sortedIDs.size(); i++)
	{
		if (m_sortedIDs[i] == m_sortedIDs.size() - 1)
		{
			std::swap(m_sortedIDs[i], m_sortedIDs[m_sortedIDs.size() - 1]);
			break;
		}
	}

	const auto& meshes = m_meshes;

	std::sort(m_sortedIDs.begin(), m_sortedIDs.end() - 1,
		[&meshes](int lhs, int rhs)
		{
			return meshes[lhs].ID < meshes[rhs].ID;
		});

	m_meshes.pop_back();
	m_sortedIDs.pop_back();
}
*/
