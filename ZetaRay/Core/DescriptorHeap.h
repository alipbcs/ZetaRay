#pragma once

#include "../Utility/SmallVector.h"
#include "Device.h"
#include "../Win32/App.h"
#include <map>

namespace ZetaRay::Core
{
	struct DescriptorTable;

	// TODO replace the current approach with something simpler
	// A collection of contiguous descriptors, out of which descriptor tables can be allocated
	struct DescriptorHeap
	{
		DescriptorHeap() noexcept = default;
		~DescriptorHeap() noexcept = default;
		
		DescriptorHeap(const DescriptorHeap&) = delete;
		DescriptorHeap& operator=(const DescriptorHeap&) = delete;

		void Init(D3D12_DESCRIPTOR_HEAP_TYPE heapType, uint32_t numDescriptors, bool isShaderVisible) noexcept;
		bool IsShaderVisible() const { return m_heap->GetDesc().Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; }
		void Shutdown() noexcept;

		// Allocates a Descriptor Table with given number of desciptors or an empty one if there isn't 
		// enough free space in the heap
		DescriptorTable Allocate(uint32_t count) noexcept;

		// Marks the Descriptor Table as ready for reuse. it'll become available for allocation during the next Recycle call
		void Release(DescriptorTable&& descTable) noexcept;

		// Previously freed Descriptor Tables whoose fence value has passed become available of resuse (not thread-safe)
		void Recycle() noexcept;

		uint32_t GetDescriptorSize() const { return m_descriptorSize; }
		uint32_t GetFreeDescriptorsCount() const { return m_freeDescCount; }
		uint64_t GetBaseGpuHandle() const { return m_baseGPUHandle.ptr; }

		ID3D12DescriptorHeap* GetHeap() { return m_heap.Get(); }
		uint32_t GetNumFreeSlots() { return m_freeDescCount; }
		uint32_t GetHeapSize() { return m_totalHeapSize; }

	private:
		//std::shared_mutex m_mutex;
		SRWLOCK m_lock = SRWLOCK_INIT;
 
		// TODO replace std::map & std::multimap with something more efficient
		
		// key: Offset,	value: number of free descriptors
		std::map<uint32_t, uint32_t> m_heapByOffsetMap;

		// key: number of free descriptors,	value: Offset 
		std::multimap<uint32_t, uint32_t> m_heapBySizeMap;

		struct PendingDescTable
		{
			PendingDescTable() = default;
			PendingDescTable(uint64_t frame, uint32_t offset, uint32_t count)
				: FrameNumber(frame),
				Offset(offset),
				Count(count)
			{}

			uint64_t FrameNumber;
			uint32_t Offset;
			uint32_t Count;
		};

		Util::SmallVector<PendingDescTable, App::PoolAllocator> m_pending;

		ComPtr<ID3D12Fence> m_fence;

		// D3D Descitptor Heap object
		ComPtr<ID3D12DescriptorHeap> m_heap;

		// CPU handle for heap start
		D3D12_CPU_DESCRIPTOR_HANDLE m_baseCPUHandle;

		// GPU handle for heap start (if GPU-visible)
		D3D12_GPU_DESCRIPTOR_HANDLE m_baseGPUHandle;

		uint32_t m_descriptorSize = 0;
		uint32_t m_freeDescCount = 0;
		uint32_t m_totalHeapSize = 0;
	};
	

	// A contigous range of descriptors that are allocated from a certain DescriptorHeapPage
	struct DescriptorTable
	{
		friend struct DescriptorHeap;

		DescriptorTable() noexcept = default;
		DescriptorTable(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
			D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle,
			uint32_t numDesc,
			uint32_t descSize,
			DescriptorHeap* heap) noexcept;
		~DescriptorTable() noexcept;

		DescriptorTable(const DescriptorTable&) = delete;
		DescriptorTable&operator=(const DescriptorTable&) = delete;

		DescriptorTable(DescriptorTable&& other) noexcept;

		// NOTE: Potenital bug where rhs is not empty. In that scenario, destructor of rhs bypasses 
		// m_page->Free(this). Use swap instead.
		// m_cpuHandle = std::exchange(rhs.m_cpuHandle, CD3DX12_CPU_DESCRIPTOR_HANDLE(D3D12_DEFAULT));
		DescriptorTable& operator=(DescriptorTable&& other) noexcept;

		void Reset() noexcept;
		bool IsEmpty() const { return m_numDescriptors == 0; }

		void Swap(DescriptorTable& other) noexcept
		{
			Assert(m_descriptorSize == other.m_descriptorSize, "Invalid swap");
			std::swap(m_baseCpuHandle, other.m_baseCpuHandle);
			std::swap(m_baseGpuHandle, other.m_baseGpuHandle);
			std::swap(m_numDescriptors, other.m_numDescriptors);
			std::swap(m_descHeap, other.m_descHeap);
		}

		// CPU handle for descriptor
		inline D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle(uint32_t offset) const noexcept
		{
			Assert(offset < m_numDescriptors, "Descriptor offset is out-of-bounds");
			return D3D12_CPU_DESCRIPTOR_HANDLE{ .ptr = m_baseCpuHandle.ptr + offset * m_descriptorSize };
		}
		
		// GPU handle to descriptor (if corresponding DescriptorHeapPage is GPU-visible)
		inline D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle(uint32_t offset) const noexcept
		{
			Assert(offset < m_numDescriptors, "Descriptor offset is out-of-bounds");
			Assert(m_descHeap->IsShaderVisible(), "This descriptor doesn't belong to a shader-visible heap.");
			return D3D12_GPU_DESCRIPTOR_HANDLE{ .ptr = m_baseGpuHandle.ptr + offset * m_descriptorSize };
		}
		
		// Number of descriptors in this DescriptorPile
		uint32_t GetNumDescriptors() const { return m_numDescriptors; };
		
		// Offset to beginning of this desc. table in the descriptor heap
		inline uint32_t GPUDesciptorHeapIndex(uint32_t offset = 0) const noexcept
		{
			Assert(m_descHeap->IsShaderVisible(), "Descriptor table is not shader-visible.");
			Assert(offset < m_numDescriptors, "Descriptor offset is out-of-bounds");

			uint32_t idx = (uint32_t)((m_baseGpuHandle.ptr - m_descHeap->GetBaseGpuHandle()) / m_descriptorSize);

			return idx + offset;
		}

	private:
		// DescriptorHeap from which this table was allocated
		DescriptorHeap* m_descHeap = nullptr;
		
		// CPU handle to start of table
		D3D12_CPU_DESCRIPTOR_HANDLE m_baseCpuHandle = { 0 };

		// GPU handle to start of table (if GPU-visible)
		D3D12_GPU_DESCRIPTOR_HANDLE m_baseGpuHandle = { 0 };

		uint32_t m_numDescriptors = 0;
		
		// Size of each descriptor in this DescriptorPile
		uint32_t m_descriptorSize = 0;
	};
}
