#include "DescriptorHeap.h"
#include "../App/Timer.h"
#include "Renderer.h"
#include "../Utility/Error.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;

//--------------------------------------------------------------------------------------
// DescriptorTable
//--------------------------------------------------------------------------------------

DescriptorTable::DescriptorTable(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle,
	uint32_t numDesc, 
	uint32_t descSize,
	DescriptorHeap* heap) noexcept
	: m_baseCpuHandle(cpuHandle),
	m_baseGpuHandle(gpuHandle),
	m_numDescriptors(numDesc),
	m_descHeap(heap),
	m_descriptorSize(descSize)
{
}

DescriptorTable::~DescriptorTable() noexcept
{
	Reset();
}

DescriptorTable::DescriptorTable(DescriptorTable&& other) noexcept
	: m_baseCpuHandle(other.m_baseCpuHandle),
	m_baseGpuHandle(other.m_baseGpuHandle),
	m_numDescriptors(other.m_numDescriptors),
	m_descHeap(other.m_descHeap),
	m_descriptorSize(other.m_descriptorSize)
{
	other.m_baseCpuHandle.ptr = 0;
	other.m_baseGpuHandle.ptr = 0;
	other.m_numDescriptors = 0;
	other.m_descHeap = nullptr;
	other.m_descriptorSize = 0;
}

DescriptorTable& DescriptorTable::operator=(DescriptorTable&& other) noexcept
{
	m_baseCpuHandle = other.m_baseCpuHandle;
	m_baseGpuHandle = other.m_baseGpuHandle;
	m_numDescriptors = other.m_numDescriptors;
	m_descHeap = other.m_descHeap;
	m_descriptorSize = other.m_descriptorSize;

	other.m_baseCpuHandle.ptr = 0;
	other.m_baseGpuHandle.ptr = 0;
	other.m_numDescriptors = 0;
	other.m_descHeap = nullptr;
	other.m_descriptorSize = 0;

	return *this;
}

// TODO: needs more testing
void DescriptorTable::Reset() noexcept
{
	if (m_baseCpuHandle.ptr)
		m_descHeap->Release(ZetaMove(*this));

	m_baseCpuHandle.ptr = 0;
	m_baseGpuHandle.ptr = 0;
	m_numDescriptors = 0;
	m_descHeap = nullptr;
	m_descriptorSize = 0;
}

//--------------------------------------------------------------------------------------
// DescriptorHeap
//--------------------------------------------------------------------------------------

void DescriptorHeap::Init(D3D12_DESCRIPTOR_HEAP_TYPE heapType, uint32_t numDescriptors, bool isHeapShaderVisible) noexcept
{
	Check(!isHeapShaderVisible || heapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		"Gpu descriptor heap type must be D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV");

	Check(!isHeapShaderVisible || numDescriptors <= 1000000,
		"GPU Resource Heap can't contain more than 1,000,000 elements");

	m_totalHeapSize = numDescriptors;

	D3D12_DESCRIPTOR_HEAP_DESC d;
	d.Type = heapType;
	d.NumDescriptors = numDescriptors;
	d.Flags = isHeapShaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	d.NodeMask = 0;

	auto* device = App::GetRenderer().GetDevice();
	CheckHR(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(m_heap.GetAddressOf())));

	m_descriptorSize = device->GetDescriptorHandleIncrementSize(heapType);
	m_freeDescCount = numDescriptors;
	m_baseCPUHandle = m_heap->GetCPUDescriptorHandleForHeapStart();

	m_heapBySizeMap.emplace(m_freeDescCount, 0);
	m_heapByOffsetMap.emplace(0, m_freeDescCount);

	if (isHeapShaderVisible)
	{
		m_baseGPUHandle = m_heap->GetGPUDescriptorHandleForHeapStart();
		CheckHR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));
	}
}

void DescriptorHeap::Shutdown() noexcept
{
	m_pending.free_memory();
}

DescriptorTable DescriptorHeap::Allocate(uint32_t count) noexcept
{
	Assert(count <= m_freeDescCount, "Not enough free descriptors.");

	//std::unique_lock lock(m_mutex);
	AcquireSRWLockExclusive(&m_lock);

	// find the smallest contigous region whoose range is greater than the requested number of descriptors
	auto it = m_heapBySizeMap.lower_bound(count);
	Assert(it != m_heapBySizeMap.end(), "Not enough free descriptors.");
	
	uint32_t size = it->first;
	uint32_t offset = it->second;
		
	if (size != count)
	{
		{
			auto nh = m_heapBySizeMap.extract(it);
			nh.key() = size - count;
			it = m_heapBySizeMap.insert(ZetaMove(nh));
			it->second = offset + count;
		}

		{
			auto nh = m_heapByOffsetMap.extract(offset);
			nh.key() = offset + count;
			auto[it2, success, node] = m_heapByOffsetMap.insert(ZetaMove(nh));
			Assert(success, "DescriptorHeap Corrupted.");
				
			it2->second = size - count;
		}
	}
	else
	{
		m_heapBySizeMap.erase(it);
			
		uint32_t numDeleted = static_cast<uint32_t>(m_heapByOffsetMap.erase(offset));
		Assert(numDeleted == 1, "DescriptorHeap Corrupted.");
	}

	m_freeDescCount -= count;

	ReleaseSRWLockExclusive(&m_lock);

	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle{ .ptr = m_baseCPUHandle.ptr + offset * m_descriptorSize };

	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = IsShaderVisible() ?
		D3D12_GPU_DESCRIPTOR_HANDLE{ .ptr = m_baseGPUHandle.ptr + offset * m_descriptorSize } :
		D3D12_GPU_DESCRIPTOR_HANDLE{ .ptr = 0 };

	return DescriptorTable(cpuHandle,
		gpuHandle,
		count,
		m_descriptorSize,
		this);
}

void DescriptorHeap::Release(DescriptorTable&& table) noexcept
{
	const uint32_t offset = (uint32_t)((table.m_baseCpuHandle.ptr - m_baseCPUHandle.ptr) / m_descriptorSize);
	const uint64_t frame = App::GetTimer().GetTotalFrameCount();

	//std::unique_lock lock(m_mutex);
	AcquireSRWLockExclusive(&m_lock);
	m_pending.emplace_back(frame, offset, table.m_numDescriptors);
	ReleaseSRWLockExclusive(&m_lock);

	//LOG("%d descriptors were freed, #free: %d.\n", table.GetNumDescriptors(), m_freeDescCount);
}

void DescriptorHeap::Recycle() noexcept
{
	if (m_pending.empty())
		return;

	const bool isGpuDescHeap = IsShaderVisible();

	// Is it necessary to additionally signal compute queue?
	if(isGpuDescHeap)
		App::GetRenderer().SignalDirectQueue(m_fence.Get(), App::GetTimer().GetTotalFrameCount());

	if (m_totalHeapSize == 64)
	{
		int a = 2;
	}

	PendingDescTable* currPending = m_pending.begin();

	while (currPending != m_pending.end())
	{
		auto [frame, offset, numDescs] = *currPending;

		Assert(offset < m_totalHeapSize, "invalid offset");
		Assert(numDescs < m_totalHeapSize, "invalid #descs");

		// not safe to release just yet
		if (isGpuDescHeap && m_fence->GetCompletedValue() < frame)
		{
			currPending++;
			continue;
		}

		if (!m_heapByOffsetMap.empty())
		{
			const auto nexIt = m_heapByOffsetMap.upper_bound((int)offset);
			const auto prevIt = std::prev(nexIt);

			// whether space between prev and it is empty, so they can be joined
			bool hasPrev = prevIt != m_heapByOffsetMap.end() && (prevIt->first + prevIt->second == offset);
			bool hasNext = nexIt != m_heapByOffsetMap.end() && (offset + numDescs == nexIt->first);

			if (hasPrev)
			{
				if (hasNext)
				{
					// delete corresponding prev entry in m_heapBySizeMap
					for (auto it = m_heapBySizeMap.lower_bound(prevIt->second); it != m_heapBySizeMap.upper_bound(prevIt->second); 
						it = std::next(it))
					{
						if (it->second == prevIt->first)
						{
							m_heapBySizeMap.erase(it);
							break;
						}
					}

					// delete corresponding next entry in m_heapBySizeMap
					for (auto it = m_heapBySizeMap.lower_bound(nexIt->second); it != m_heapBySizeMap.upper_bound(nexIt->second); 
						it = std::next(it))
					{
						if (it->second == nexIt->first)
						{
							m_heapBySizeMap.erase(it);
							break;
						}
					}

					// merge prevIt, curr desc table being recycled and nextIt into prevIt
					prevIt->second += numDescs + nexIt->second;
					m_heapByOffsetMap.erase(nexIt);
					m_heapBySizeMap.emplace(prevIt->second, prevIt->first);
				}
				else
				{
					for (auto it = m_heapBySizeMap.lower_bound(prevIt->second); it != m_heapBySizeMap.upper_bound(prevIt->second); 
						it = std::next(it))
					{
						if (it->second == prevIt->first)
						{
							m_heapBySizeMap.erase(it);
							break;
						}
					}

					// adjust size of prevIt
					prevIt->second += numDescs;
					m_heapBySizeMap.emplace(prevIt->second, prevIt->first);
				}
			}
			else if (hasNext)
			{
				for (auto it = m_heapBySizeMap.lower_bound(nexIt->second); it != m_heapBySizeMap.upper_bound(nexIt->second); 
					it = std::next(it))
				{
					if (it->second == nexIt->first)
					{
						m_heapBySizeMap.erase(it);
						break;
					}
				}

				// bug
//				nexIt->second -= descTable.second;
				nexIt->second += numDescs;

				m_heapBySizeMap.emplace(nexIt->second, (uint32_t)offset);

				// key if nexIt needs to change
				auto nh = m_heapByOffsetMap.extract(nexIt);
				nh.key() = (uint32_t)offset;
				
				auto [it, success, node] = m_heapByOffsetMap.insert(std::move(nh));

				Assert(success, "DescriptorHeap Corrupted.");
			}
			else
			{
				m_heapByOffsetMap.emplace((uint32_t)offset, numDescs);
				m_heapBySizeMap.emplace(numDescs, (uint32_t)offset);
			}
		}
		// superflous?
		else
		{
			auto[it, success] = m_heapByOffsetMap.emplace((uint32_t)offset, numDescs);
			m_heapBySizeMap.emplace(numDescs, (uint32_t)offset);
		}

		m_freeDescCount += numDescs;
		currPending = m_pending.erase(*currPending);

//		g_pApp->GetLogger().LogToConsole(to_string("DescriptorTable with size: %u was free'd", numDescs));
	}
}


