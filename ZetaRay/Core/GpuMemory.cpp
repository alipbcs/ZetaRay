#include "../App/Timer.h"
#include "../Math/Common.h"
#include "Renderer.h"
#include "CommandList.h"
#include "Direct3DHelpers.h"
#include "../Support/Task.h"
#include "../Support/MemoryArena.h"
#include <thread>
#include <algorithm>
#include <xxHash-0.8.1/xxhash.h>

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;

namespace ZetaRay::Core::Internal
{
	// Upload heap resource management is mostly based on "GraphicsMemory" class from DirectXTK12 
	// library (MIT License), available here:
	// https://github.com/microsoft/DirectXTK12

	//--------------------------------------------------------------------------------------
	// LinearAllocatorPage
	//--------------------------------------------------------------------------------------

	// An upload heap buffer of a certain size that can be shared among different allocations while 
	// taking the alignment requirement into account
	struct LinearAllocatorPage
	{
		friend struct UploadHeapManager;
		friend struct LinearAllocator;

		LinearAllocatorPage() = default;
		explicit LinearAllocatorPage(size_t pageSize) noexcept
			: m_size(pageSize)
		{
			D3D12_HEAP_PROPERTIES uploadHeap = Direct3DHelper::UploadHeapProp();
			D3D12_RESOURCE_DESC bufferDesc = Direct3DHelper::BufferResourceDesc(pageSize);

			auto* device = App::GetRenderer().GetDevice();

			CheckHR(device->CreateCommittedResource(&uploadHeap,
				D3D12_HEAP_FLAG_CREATE_NOT_ZEROED,
				&bufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(m_uploadResource.GetAddressOf())));

			SET_D3D_OBJ_NAME(m_uploadResource, "LinearAllocator");
			//m_gpuAddress = m_uploadResource->GetGPUVirtualAddress();

			// From MS docs:
			// "Resources on D3D12_HEAP_TYPE_UPLOAD heaps can be persistently mapped, meaning Map can be called 
			// once, immediately after resource creation. Unmap never needs to be called, but the address returned 
			// from Map must no longer be used after the last reference to the resource is released. When using 
			// persistent map, the application must ensure the CPU finishes writing data into memory before the GPU 
			// executes a command list that reads the memory."
			CheckHR(m_uploadResource->Map(0, nullptr, &m_memory));
			//memset(pMemory, 0, m_increment);

			CheckHR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));

			m_lastFrameUsed = (uint32_t)App::GetTimer().GetTotalFrameCount();
		}

		LinearAllocatorPage(LinearAllocatorPage&& rhs) noexcept
			: m_size(rhs.m_size)
		{
			m_uploadResource.Swap(rhs.m_uploadResource);
			m_fence.Swap(rhs.m_fence);
			std::swap(m_pendingFence, rhs.m_pendingFence);
			std::swap(m_memory, rhs.m_memory);
			std::swap(m_offset, rhs.m_offset);
			std::swap(m_refCount, rhs.m_refCount);
		}

		LinearAllocatorPage& operator=(LinearAllocatorPage&& rhs) noexcept
		{
			if (this == &rhs)
				return *this;

			m_uploadResource.Swap(rhs.m_uploadResource);
			m_fence.Swap(rhs.m_fence);
			std::swap(m_pendingFence, rhs.m_pendingFence);
			std::swap(m_memory, rhs.m_memory);
			std::swap(m_offset, rhs.m_offset);
			std::swap(m_refCount, rhs.m_refCount);

			return *this;
		}

		ZetaInline size_t Suballocate(size_t size, size_t alignment) noexcept
		{
			size_t offset = Math::AlignUp(m_offset, alignment);
			Assert(offset + size <= m_size, "Out of free memory in page suballoc");

			m_offset = offset + size;

			return offset;
		}

		void Release() noexcept
		{
			Assert(m_refCount > 0, "Reference count should always be greater than 0.");
			m_refCount -= 1;

			if (m_refCount == 0)
			{
				m_uploadResource->Unmap(0, nullptr);
				m_uploadResource.Reset();
				m_fence.Reset();
				//delete this;
			}
		}

		ID3D12Resource* GetResource() { return m_uploadResource.Get(); }

	private:
		ComPtr<ID3D12Resource> m_uploadResource;
		ComPtr<ID3D12Fence> m_fence;
		uint64_t m_pendingFence = 0;
		void* m_memory = nullptr;

		size_t m_offset = 0;
		size_t m_size = 0;
		uint32_t m_refCount = 1;
		uint32_t m_lastFrameUsed = 0;
	};

	//--------------------------------------------------------------------------------------
	// LinearAllocator
	//--------------------------------------------------------------------------------------

	// A linked-list of LinearAllocatorPages of the same size
	struct LinearAllocator
	{
		friend struct UploadHeapManager;

		LinearAllocator() noexcept = default;
		LinearAllocator(size_t pageSize) noexcept
			: m_pageSize(pageSize)
		{
		}

		~LinearAllocator() noexcept
		{
			App::GetRenderer().FlushAllCommandQueues();

			for (auto& page : m_reuseReadyPages)
			{
				page.m_refCount = 1;
				page.Release();
			}
		}

		LinearAllocator(const LinearAllocator&) = delete;
		LinearAllocator& operator=(const LinearAllocator&) = delete;

		LinearAllocatorPage& FindPageForAlloc(size_t size, size_t alignment, bool forceCleanPage) noexcept
		{
			Assert(size <= m_pageSize, "Size must be less or equal to the allocator's page size");
			Assert(alignment <= m_pageSize, "Alignment must be less or equal to the allocator's increment");

			// Fast path
			if (forceCleanPage || (size == m_pageSize && (alignment == 0 || alignment == m_pageSize || ((size & (alignment - 1)) == 0))))
				return GetCleanPageForAlloc();

			// Find a page that is currently in use with enough space.
			for (auto& page : m_inUsePages)
			{
				size_t offset = Math::AlignUp(page.m_offset, alignment);

				if (offset + size <= m_pageSize)
				{
					// update the last used counter
					page.m_lastFrameUsed = (uint32_t)App::GetTimer().GetTotalFrameCount();
					return page;
				}
			}

			auto& page = GetCleanPageForAlloc();

			return page;
		}

		void FenceCommittedPages() noexcept
		{
			if (m_inUsePages.empty())
				return;

			AcquireSRWLockExclusive(&m_inUseLock);

			// Fence all the in-flight pages that have a reference count of 1.
			for (size_t i = 0; i < m_inUsePages.size();)
			{
				auto& page = m_inUsePages[i];

				// This implies the allocator is the only remaining reference to the page, and therefore the memory is ready for re-use.
				if (page.m_refCount == 1)
				{
					App::GetRenderer().SignalDirectQueue(page.m_fence.Get(), ++page.m_pendingFence);
					m_pendingFencePages.push_back(ZetaMove(m_inUsePages[i]));
					m_inUsePages.erase(i);
				}
				else
					i++;
			}

			// Resource addresses are unique
			std::sort(m_inUsePages.begin(), m_inUsePages.end(),
				[](LinearAllocatorPage& lhs, LinearAllocatorPage& rhs)
				{
					return lhs.GetResource() < rhs.GetResource();
				});

			ReleaseSRWLockExclusive(&m_inUseLock);
		}

		void RetirePendingPages() noexcept
		{
			const auto frame = App::GetTimer().GetTotalFrameCount();

			for (size_t i = 0; i < m_reuseReadyPages.size();)
			{
				auto& page = m_reuseReadyPages[i];

				if (frame - page.m_lastFrameUsed >= 10)
				{
					AcquireSRWLockExclusive(&m_gcLock);
					m_toGarbackCollectPages.push_back(ZetaMove(page));
					ReleaseSRWLockExclusive(&m_gcLock);

					m_reuseReadyPages.erase(i);
				}
				else
					i++;
			}

			if (m_pendingFencePages.empty())
				return;

			// Check each page that we know has a fence pending. If the fence has passed,
			// we can mark the page for re-use by appending it to m_toBeGarbageCollectedPages list.
			for (size_t i = 0; i < m_pendingFencePages.size();)
			{
				auto& page = m_pendingFencePages[i];

				// This implies the allocator is the only remaining reference to the page, and therefore the memory is ready for re-use.
				if (page.m_fence->GetCompletedValue() >= page.m_pendingFence)
				{
					// free the memory
					page.m_offset = 0;
					//memset(page->mMemory, 0, m_increment);

					m_reuseReadyPages.push_back(ZetaMove(m_pendingFencePages[i]));
					m_reuseReadyPages.back().m_lastFrameUsed = (uint32_t)frame;
					m_pendingFencePages.erase(i);
				}
				else
					i++;
			}
		}

		// Throws away all currently unused pages
		void Shrink() noexcept
		{
			for(auto& page : m_toGarbackCollectPages)
			{
				AcquireSRWLockExclusive(&m_gcLock);

				if (m_toGarbackCollectPages.empty())
				{
					ReleaseSRWLockExclusive(&m_gcLock);
					return;
				}

				LinearAllocatorPage gcPage = ZetaMove(m_toGarbackCollectPages.back());
				m_toGarbackCollectPages.pop_back();
				ReleaseSRWLockExclusive(&m_gcLock);

				gcPage.Release();
			}
		}

	private:
		LinearAllocatorPage& GetCleanPageForAlloc() noexcept
		{
			// see if there exists a page that can be reused
			if (!m_reuseReadyPages.empty())
			{
				m_inUsePages.push_back(ZetaMove(m_reuseReadyPages.back()));
				m_reuseReadyPages.pop_back();

				// insertion sort
				size_t i = m_inUsePages.size() - 1;
				while (i > 0 && m_inUsePages[i].GetResource() < m_inUsePages[i - 1].GetResource())
				{
					std::swap(m_inUsePages[i], m_inUsePages[i - 1]);
					i--;
				}

				// update the last used counter
				m_inUsePages[i].m_lastFrameUsed = (uint32_t)App::GetTimer().GetTotalFrameCount();
				return m_inUsePages[i];
			}

			// Allocate a new page
			m_inUsePages.emplace_back(m_pageSize);

			size_t i = m_inUsePages.size() - 1;
			while (i > 0 && m_inUsePages[i].GetResource() < m_inUsePages[i - 1].GetResource())
			{
				std::swap(m_inUsePages[i], m_inUsePages[i - 1]);
				i--;
			}

			return m_inUsePages[i];
		}

		// TODO PoolAllocator isn't a good fit here -- come up with a custom allocator
		// that handles allocations that persist over multiple frames
		SmallVector<LinearAllocatorPage> m_pendingFencePages;		// Pages that are pending fence becoming signalled by the GPU
		SmallVector<LinearAllocatorPage> m_inUsePages;				// Pages with reference count > 1
		SmallVector<LinearAllocatorPage> m_reuseReadyPages;			// Pages that are unused and can be free'd or otherwise reused
		SmallVector<LinearAllocatorPage> m_toGarbackCollectPages;	// Pages that are unused and can be free'd or otherwise reused
		size_t m_pageSize = -1;
		SRWLOCK m_inUseLock = SRWLOCK_INIT;
		SRWLOCK m_gcLock = SRWLOCK_INIT;
	};

	//--------------------------------------------------------------------------------------
	// UploadHeapManager
	//--------------------------------------------------------------------------------------

	// https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_type
	// textures (unlike buffers) can't be heap type UPLOAD or READBACK.
	struct UploadHeapManager
	{
		UploadHeapManager() noexcept
		{
			for (size_t i = 0; i < POOL_COUNT; i++)
			{
				size_t pageSize = GetPageSizeFromPoolIndex(i);
				new (&m_pools[i]) LinearAllocator(pageSize);
			}
		}

		~UploadHeapManager() noexcept
		{
		}

		UploadHeapManager(UploadHeapManager&&) = delete;
		UploadHeapManager& operator=(UploadHeapManager&&) = delete;

		UploadHeapBuffer GetBuffer(int threadIdx, size_t size, size_t alignment = -1, bool forceCleanPage = false) noexcept
		{
			Check(size <= MAX_ALLOC_SIZE, "allocations larger than %llu MB are not supported.",
				MAX_ALLOC_SIZE / (1024 * 1024));

			if (alignment == -1)
				alignment = 4;

			Assert(alignment >= 4, "Should use at least DWORD alignment");
			Assert(Math::IsPow2(alignment), "alignment must be a power of two");

			// Which memory pool does it live in?
			const size_t alignedSize = (size + alignment - 1) & ~(alignment - 1);
			const size_t poolIndex = GetPoolIndexFromSize(alignedSize);
			Assert(poolIndex < POOL_COUNT, "invalid pool index");

			auto& allocator = m_pools[poolIndex];
			Assert(alignedSize <= MIN_ALLOC_SIZE || Math::NextPow2(alignedSize) == allocator.m_pageSize, "wrong LinearAllocator to allocate from");

			AcquireSRWLockExclusive(&m_pools[poolIndex].m_inUseLock);

			LinearAllocatorPage& page = allocator.FindPageForAlloc(size, alignment, forceCleanPage);
			size_t offset = page.Suballocate(size, alignment);
			Assert(!forceCleanPage || offset == 0, "Returned page wasn't clean");
			page.m_refCount++;

			// Return the information to the user
			auto buff = UploadHeapBuffer(PageHandle{ .PoolIdx = (int)poolIndex, .ThreadIdx = threadIdx },
				offset,
				page.m_uploadResource->GetGPUVirtualAddress() + offset,
				page.m_uploadResource.Get(),
				(uint8_t*)(page.m_memory) + offset,
				size);

			ReleaseSRWLockExclusive(&m_pools[poolIndex].m_inUseLock);

			return buff;
		}

		// Submits all the pending one-shot memory to the GPU. 
		// The memory will be recycled once the GPU is done with it.
		void Recycle() noexcept
		{
			for (LinearAllocator& i : m_pools)
			{
				i.FenceCommittedPages();
				i.RetirePendingPages();
			}
		}

		// This frees up any unused memory. 
		// If you want to make sure all memory is reclaimed, idle the GPU before calling this.
		// It is not recommended that you call this unless absolutely necessary (e.g. your
		// memory budget changes at run-time, or perhaps you're changing levels in your game.)
		void GarbageCollect() noexcept
		{
			const bool doShrinkThisFrame = App::GetTimer().GetTotalFrameCount() % SHRINK_FREQUECNY == SHRINK_FREQUECNY - 1;

			if (doShrinkThisFrame)
			{
				Task t("Shrinking memroy for UploadHeap", TASK_PRIORITY::BACKGRUND, [this]()
				{
					for (auto& i : m_pools)
						i.Shrink();
				});

				// submit
				App::SubmitBackground(ZetaMove(t));
			}
		}

		void ReleaseBuffer(size_t poolIdx, ID3D12Resource* r) noexcept
		{
			AcquireSRWLockExclusive(&m_pools[poolIdx].m_inUseLock);

			size_t idxInPool = -1;
			size_t beg = 0;
			size_t end = m_pools[poolIdx].m_inUsePages.size();
			size_t mid = end >> 1;

			while (true)
			{
				if (end - beg <= 2)
					break;

				if (m_pools[poolIdx].m_inUsePages[mid].m_uploadResource.Get() < r)
					beg = mid + 1;
				else
					end = mid + 1;

				mid = beg + ((end - beg) >> 1);
			}

			if (m_pools[poolIdx].m_inUsePages[beg].m_uploadResource.Get() == r)
				idxInPool = beg;
			else if (m_pools[poolIdx].m_inUsePages[mid].m_uploadResource.Get() == r)
				idxInPool = mid;

			Assert(idxInPool != -1, "Resource was not found");
			m_pools[poolIdx].m_inUsePages[idxInPool].Release();

			ReleaseSRWLockExclusive(&m_pools[poolIdx].m_inUseLock);
		}

	private:
		static constexpr size_t SHRINK_FREQUECNY = 10;
		static constexpr size_t MIN_PAGE_SIZE = 64 * 1024;
		static constexpr size_t MIN_ALLOC_SIZE = 64 * 1024;
		static constexpr size_t ALLOCATOR_INDEX_SHIFT = 16; // start block sizes at 64KB
		// log2(128 mb) - log2(64 kb)
		static constexpr size_t POOL_COUNT = 12; // allocation sizes up to 128 MB supported
		static constexpr size_t MAX_ALLOC_SIZE = 1 << (POOL_COUNT - 1 + ALLOCATOR_INDEX_SHIFT);

		static_assert((1 << ALLOCATOR_INDEX_SHIFT) == MIN_ALLOC_SIZE, "1 << ALLOCATOR_INDEX_SHIFT must == MIN_PAGE_SIZE (in KiB)");
		static_assert((MIN_PAGE_SIZE& (MIN_PAGE_SIZE - 1)) == 0, "MIN_PAGE_SIZE must be a power of 2");
		static_assert((MIN_ALLOC_SIZE& (MIN_ALLOC_SIZE - 1)) == 0, "MIN_ALLOC_SIZE size must be a power of 2");
		static_assert(MIN_ALLOC_SIZE >= (4 * 1024), "MIN_ALLOC_SIZE size must be greater than 4K");

		ZetaInline int64_t GetPoolIndexFromSize(size_t x)
		{
			x = std::max(64llu * 1024, Math::NextPow2(x));
			DWORD bitIndex;
			_BitScanForward64(&bitIndex, x);

			return bitIndex - ALLOCATOR_INDEX_SHIFT;
		}

		ZetaInline size_t GetPageSizeFromPoolIndex(size_t x)
		{
			//return std::max(MIN_PAGE_SIZE, 1llu << (x + ALLOCATOR_INDEX_SHIFT));
			return 1llu << (x + ALLOCATOR_INDEX_SHIFT);
		}

		LinearAllocator m_pools[POOL_COUNT];
	};

	//--------------------------------------------------------------------------------------
	// DefaultHeap
	//--------------------------------------------------------------------------------------

	struct DefaultHeapManager
	{
	public:
		DefaultHeapManager() noexcept
		{
			auto* device = App::GetRenderer().GetDevice();
			CheckHR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));
		}

		DefaultHeapManager(DefaultHeapManager&&) = delete;
		DefaultHeapManager&& operator=(DefaultHeapManager&&) = delete;

		DefaultHeapBuffer GetBuffer(const char* name, uint64_t sizeInBytes, D3D12_RESOURCE_STATES initState, 
			bool allowUAV, bool initToZero) noexcept
		{
			Assert((initState == D3D12_RESOURCE_STATE_COMMON) || initState & Constants::VALID_BUFFER_STATES, "Invalid initial state for a buffer.");
			const size_t key = Math::AlignUp(sizeInBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

			return AllocateBuffer(name, key, initState, allowUAV, initToZero);
		}

		void ReleaseBuffer(ComPtr<ID3D12Resource>&& buff) noexcept
		{
			m_doGarbageCollectionThisFrame = true;

			// use the same fence value for all the releases in this frame, then
			// at the end of the frame, signal it
			m_relesedResources.emplace_back(ZetaMove(buff), m_currFenceVal);
		}

		void Recycle() noexcept
		{
			// no resource was released this frame
			if (!m_doGarbageCollectionThisFrame)
				return;

//			Task t("Recycle memroy for DefaultHeap", TASK_PRIORITY::BACKGRUND, [this]()
//				{
					// for next frame
					m_doGarbageCollectionThisFrame = false;

					// signal the direct queue and advance the fence value
					// Is it necessary to additionally signal the compute queue?
					App::GetRenderer().SignalDirectQueue(m_fence.Get(), m_currFenceVal++);

					const uint64_t lastCompletedFence = m_fence->GetCompletedValue();

					for (auto it = m_relesedResources.begin(); it != m_relesedResources.end();)
					{
						if (lastCompletedFence > it->FenceValWhenReleased)
							it = m_relesedResources.erase(*it);
						else
							it++;
					}
//				});

			// submit
//			App::SubmitBackground(ZetaMove(t));
		}

	private:
		DefaultHeapBuffer AllocateBuffer(const char* name, uint64_t sizeInBytes, D3D12_RESOURCE_STATES initState, 
			bool allowUAV, bool initToZero) noexcept
		{
			D3D12_RESOURCE_FLAGS f = allowUAV ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;

			D3D12_HEAP_PROPERTIES heapDesc = Direct3DHelper::DefaultHeapProp();
			D3D12_RESOURCE_DESC bufferDesc = Direct3DHelper::BufferResourceDesc(sizeInBytes, f);

			ComPtr<ID3D12Resource> r;
			auto* device = App::GetRenderer().GetDevice();

			D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
			if (!initToZero)
				heapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

			CheckHR(device->CreateCommittedResource(&heapDesc,
				heapFlags,
				&bufferDesc,
				initState,
				nullptr,
				IID_PPV_ARGS(r.GetAddressOf())));

			return DefaultHeapBuffer(name, ZetaMove(r));
		}

		struct ReleasedResource
		{
			ReleasedResource() = default;
			ReleasedResource(ComPtr<ID3D12Resource>&& b, uint64_t f) noexcept
				: R(ZetaMove(b)),
				FenceValWhenReleased(f)
			{}

			ComPtr<ID3D12Resource> R;
			uint64_t FenceValWhenReleased;
		};

		SmallVector<ReleasedResource, App::ThreadAllocator> m_relesedResources;

		uint64_t m_currFenceVal = 1;
		ComPtr<ID3D12Fence> m_fence;

		// do garbage collection only if at least one resource was released this frame
		bool m_doGarbageCollectionThisFrame = false;
	};

	//--------------------------------------------------------------------------------------
	// ResourceUploadBatch
	//--------------------------------------------------------------------------------------

	struct ResourceUploadBatch
	{
		ResourceUploadBatch() noexcept
			: m_arena(4 * 1096),
			m_scratchResources(m_arena)
		{
			CheckHR(App::GetRenderer().GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));
			m_event = CreateEventA(nullptr, false, false, "");
			CheckWin32(m_event);

			m_scratchResourcesReleased.store(false, std::memory_order_relaxed);
		}
		~ResourceUploadBatch() noexcept = default;

		ResourceUploadBatch(ResourceUploadBatch&& other) = delete;
		ResourceUploadBatch& operator=(ResourceUploadBatch&& other) = delete;

		void Begin() noexcept
		{
			Assert(!m_inBeginEndBlock, "Can't Begin: already in a Begin-End block.");
			m_inBeginEndBlock = true;

			if (m_scratchResourcesReleased.load(std::memory_order_acquire))
			{
				// between the check above and the next statement (inside the if block or not), m_scratchResourcesReleased 
				// could've changed from false to true, which is ok. It's only important that it didn't change from true
				// to false, which can't happen by a background thread

				// at this point no other thread is referencing the memory in MemoryArena
				m_arena.Reset();

				m_scratchResourcesReleased.store(false, std::memory_order_relaxed);
			}
		}

		// Works by:
		// 1. Allocates an intermediate upload heap buffer whoose size is calculated by GetCopyableFootprints
		// 2. Maps the intermediate buffer
		// 3. Copies all the subresources (MemcpySubresource)
		// 4. Unmaps the intermediate buffer
		// 5. Records either a CopyBufferRegion or a CopyTextureRegion call for each subresource 
		// on the command list
		void UploadTexture(int threadIdx, UploadHeapManager& uploadHeap, ID3D12Resource* resource, int firstSubresourceIndex,
			D3D12_SUBRESOURCE_DATA* subResData, int numSubresources, D3D12_RESOURCE_STATES postCopyState) noexcept
		{
			Assert(m_inBeginEndBlock, "Can't call Upload on a closed ResourceUploadBatch.");
			Check(resource, "resource was NULL");

			if (!m_directCmdList)
			{
				m_directCmdList = App::GetRenderer().GetGraphicsCmdList();
				m_directCmdList->SetName("ResourceUploadBatch");
			}

			constexpr int MAX_NUM_SUBRESOURCES = 20;
			Assert(MAX_NUM_SUBRESOURCES >= numSubresources, "MAX_NUM_SUBRESOURCES is too small");

			D3D12_PLACED_SUBRESOURCE_FOOTPRINT subresLayout[MAX_NUM_SUBRESOURCES];
			UINT subresNumRows[MAX_NUM_SUBRESOURCES];
			UINT64 subresRowSize[MAX_NUM_SUBRESOURCES];

			auto destDesc = resource->GetDesc();
			// As buffers have a 64 KB alignment, GetCopyableFootprints() returns the padded size. Using subresRowSize[0]
			// as the copy size could lead to access violations since size of data pointed to by subresData can be smaller
			Assert(destDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER, "This functions is for uploading textures.");

			// required size of an intermediate buffer that is to be used to initialize this resource
			auto* device = App::GetRenderer().GetDevice();
			UINT64 totalSize;
			device->GetCopyableFootprints(&destDesc,			// resource description
				firstSubresourceIndex,							// index of the first subresource
				numSubresources,								// number of subresources
				0,												// offset to the resource
				subresLayout,									// description and placement of each subresource
				subresNumRows,									// number of rows for each subresource
				subresRowSize,									// unpadded size of a row of each subresource
				&totalSize);									// total size

			// alignment is not important since we're requesting a clean page which we know
			// will be 64 kb aligned
			UploadHeapBuffer intermediateBuff = uploadHeap.GetBuffer(threadIdx, totalSize, -1, true);
			Assert(intermediateBuff.GetOffset() == 0, "Offset must be zero");
			//Assert(intermediateBuff.GetSize() >= totalSize + subresLayout[0].Offset, "intermediate buffer is too small.");

			// for each subresource in Destination
			for (int i = 0; i < numSubresources; i++)
			{
				// destination
				size_t destOffset = subresLayout[i].Offset;
				const size_t destSubresSlicePitch = subresLayout[i].Footprint.RowPitch * subresNumRows[i];

				// for each slice of that subresource
				for (int slice = 0; slice < (int)subresLayout[i].Footprint.Depth; slice++)
				{
					uintptr_t sourceSubres = reinterpret_cast<uintptr_t>(subResData[i].pData) + subResData[i].SlicePitch * slice;

					// for each row of that subresource slice
					for (int row = 0; row < (int)subresNumRows[i]; row++)
					{
						// subresRowSize[i]: #bytes to copy for each row (unpadded)
						// subresLayout[i].Footprint.RowPitch: padded size in bytes of each row
						intermediateBuff.Copy(destOffset + row * subresLayout[i].Footprint.RowPitch,
							subresRowSize[i],
							reinterpret_cast<void*>(sourceSubres + subResData[i].RowPitch * row));
					}

					destOffset += destSubresSlicePitch;
				}
			}

			for (int i = 0; i < numSubresources; i++)
			{
				D3D12_TEXTURE_COPY_LOCATION dst;
				dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dst.pResource = resource;
				dst.SubresourceIndex = i + firstSubresourceIndex;

				D3D12_TEXTURE_COPY_LOCATION src;
				src.pResource = intermediateBuff.GetResource();
				src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				src.PlacedFootprint = subresLayout[i];

				m_directCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			}

			if (postCopyState != D3D12_RESOURCE_STATE_COPY_DEST)
				m_directCmdList->TransitionResource(resource, D3D12_RESOURCE_STATE_COPY_DEST, postCopyState);

			// Preserve the scratch buffer for as long as GPU is using it 
			m_scratchResources.push_back(ZetaMove(intermediateBuff));
		}

		void UploadBuffer(int threadIdx, UploadHeapManager& uploadHeap, ID3D12Resource* resource, void* data,
			size_t sizeInBytes, D3D12_RESOURCE_STATES postCopyState) noexcept
		{
			Assert(m_inBeginEndBlock, "Can't call Upload on a closed ResourceUploadBatch.");
			Check(resource, "resource was NULL");

			if (!m_directCmdList)
			{
				m_directCmdList = App::GetRenderer().GetGraphicsCmdList();
				m_directCmdList->SetName("ResourceUploadBatch");
			}

			// required size of a buffer which is to be used to initialize this resource
			//uint64_t uploadSize = Direct3DHelper::GetRequiredIntermediateSize(resource, 0, 1);
			// TODO GetRequiredIntermediateSize() is not necessary, but recheck
			uint64_t uploadSize = sizeInBytes;

			D3D12_HEAP_PROPERTIES heapProps = Direct3DHelper::UploadHeapProp();
			D3D12_RESOURCE_DESC resDesc = Direct3DHelper::BufferResourceDesc(uploadSize);

			// Create a scratch buffer
			UploadHeapBuffer scratchBuff = uploadHeap.GetBuffer(threadIdx, uploadSize);
			scratchBuff.Copy(0, sizeInBytes, data);

			// Submit resource copy to command list
			// Note: can't use CopyResource() since the UploadHeap might not have the exact same size as resource
			// (due to subresource allocation, pool sizes being successive powers of two, etc)

			// reminder UploadHeapBuffer might have an offset from ths start of underlying
			// CommitedHeap
			m_directCmdList->CopyBufferRegion(resource,
				0,
				scratchBuff.GetResource(),
				scratchBuff.GetOffset(),
				sizeInBytes);

			// "decay" into the COMMON state
			if (postCopyState != D3D12_RESOURCE_STATE_COPY_DEST &&
				postCopyState != D3D12_RESOURCE_STATE_COMMON &&
				postCopyState != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
				m_directCmdList->TransitionResource(resource, D3D12_RESOURCE_STATE_COPY_DEST, postCopyState);

			// Preserve the scratch buffer for as long as GPU is using it 
			m_scratchResources.push_back(ZetaMove(scratchBuff));
		}

		void UploadTexture(int threadIdx, UploadHeapManager& uploadHeap, ID3D12Resource* dstResource,
			uint8_t* pixels, D3D12_RESOURCE_STATES postCopyState) noexcept
		{
			if (!m_directCmdList)
			{
				m_directCmdList = App::GetRenderer().GetGraphicsCmdList();
				m_directCmdList->SetName("ResourceUploadBatch");
			}

			auto desc = dstResource->GetDesc();

			const uint32_t numBytesPerPixel = (uint32_t)Direct3DHelper::BitsPerPixel(desc.Format) >> 3;
			const UINT rowSizeInBytes = (uint32_t)desc.Width * numBytesPerPixel;
			const UINT rowPitch = (uint32_t)Math::AlignUp(rowSizeInBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
			const UINT uploadSize = desc.Height * rowPitch;

			// Create a scratch buffer
			UploadHeapBuffer scratchBuff = uploadHeap.GetBuffer(threadIdx, uploadSize);

			// scratch buffer is already mapped
			for (uint32_t y = 0; y < desc.Height; y++)
				scratchBuff.Copy(y * rowPitch, rowSizeInBytes, pixels + y * rowSizeInBytes);

			D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
			srcLocation.pResource = scratchBuff.GetResource();
			srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			srcLocation.PlacedFootprint.Offset = scratchBuff.GetOffset();
			srcLocation.PlacedFootprint.Footprint.Format = desc.Format;
			srcLocation.PlacedFootprint.Footprint.Width = (UINT)desc.Width;
			srcLocation.PlacedFootprint.Footprint.Height = (UINT)desc.Height;
			srcLocation.PlacedFootprint.Footprint.Depth = 1;
			srcLocation.PlacedFootprint.Footprint.RowPitch = rowPitch;

			D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
			dstLocation.pResource = dstResource;
			dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLocation.SubresourceIndex = 0;

			m_directCmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, NULL);

			if (postCopyState != D3D12_RESOURCE_STATE_COPY_DEST)
				m_directCmdList->TransitionResource(dstResource, D3D12_RESOURCE_STATE_COPY_DEST, postCopyState);

			// Preserve the scratch buffer for as long as GPU is using it 
			m_scratchResources.push_back(ZetaMove(scratchBuff));
		}

		// Submits all the uploads
		// No more uploads can happen after this call until Begin is called again.
		void End() noexcept
		{
			Assert(m_inBeginEndBlock, "ResourceUploadBatch already closed.");

			if (!m_scratchResources.empty())
			{
				auto& renderer = App::GetRenderer();

				uint64_t completionFence = renderer.ExecuteCmdList(m_directCmdList);
				// compute queue needs to wait for direct queue
				renderer.WaitForDirectQueueOnComputeQueue(completionFence);

				// signal the fence
				renderer.SignalDirectQueue(m_fence.Get(), m_fenceVal++);

				size_t numRes = m_scratchResources.size();
				StackStr(tname, m, "Releasing %llu gpu-copy scratch buffers_%d", numRes, rand());

				Task t(tname, TASK_PRIORITY::BACKGRUND, [this, res = ZetaMove(m_scratchResources)] () mutable
				{
					if (m_fence->GetCompletedValue() < m_fenceVal - 1)
					{
						CheckHR(m_fence->SetEventOnCompletion(m_fenceVal - 1, m_event));
						WaitForSingleObject(m_event, INFINITE);
					}

					res.free_memory();
					m_scratchResourcesReleased.store(true, std::memory_order_release);
				});

				Assert(m_scratchResources.empty(), "");

				App::SubmitBackground(ZetaMove(t));				
				m_directCmdList = nullptr;
			}

			m_inBeginEndBlock = false;
		}

	private:
		// scratch resources need to stay alive while GPU is using them
		MemoryArena m_arena;
		SmallVector<UploadHeapBuffer, Support::ArenaAllocator> m_scratchResources;
		GraphicsCmdList* m_directCmdList = nullptr;
		ComPtr<ID3D12Fence> m_fence;
		HANDLE m_event;
		uint64_t m_fenceVal = 1;
		bool m_inBeginEndBlock = false;

		std::atomic_bool m_scratchResourcesReleased;
	};
}

using namespace ZetaRay::Core::Internal;

//--------------------------------------------------------------------------------------
// UploadHeapBuffer
//--------------------------------------------------------------------------------------

UploadHeapBuffer::UploadHeapBuffer(Internal::PageHandle handle, size_t offset, D3D12_GPU_VIRTUAL_ADDRESS gpuAddress,
	ID3D12Resource* resource, void* memory, size_t size) noexcept
	: PageHandle(handle),
	GpuAddress(gpuAddress),
	Resource(resource),
	MappedMemory(memory),
	Size(size),
	OffsetFromResource(offset)
{
	Assert(resource->GetGPUVirtualAddress() + offset == gpuAddress, "bug");
}

UploadHeapBuffer::~UploadHeapBuffer() noexcept
{
	Reset();
}

UploadHeapBuffer::UploadHeapBuffer(UploadHeapBuffer&& rhs) noexcept
{
	std::swap(PageHandle, rhs.PageHandle);
	std::swap(GpuAddress, rhs.GpuAddress);
	std::swap(Resource, rhs.Resource);
	std::swap(MappedMemory, rhs.MappedMemory);
	std::swap(Size, rhs.Size);
	std::swap(OffsetFromResource, rhs.OffsetFromResource);
}

UploadHeapBuffer& UploadHeapBuffer::operator=(UploadHeapBuffer&& rhs) noexcept
{
	if (this == &rhs)
		return *this;

	// release the current Page (if any)
	// TODO following two lines seem unnecessary at first glance, yet
	// can lead to serious memory leak if avoided. The overall upload heap
	// design is too complicated, making reasoning and debugging harder
	Reset();
	PageHandle.Reset();

	std::swap(PageHandle, rhs.PageHandle);
	std::swap(GpuAddress, rhs.GpuAddress);
	std::swap(Resource, rhs.Resource);
	std::swap(MappedMemory, rhs.MappedMemory);
	std::swap(Size, rhs.Size);
	std::swap(OffsetFromResource, rhs.OffsetFromResource);

	return *this;
}

void UploadHeapBuffer::Reset() noexcept
{
	if (Resource)
		App::GetRenderer().GetGpuMemory().ReleaseUploadHeapBuffer(*this);

	PageHandle.Reset();
	GpuAddress = 0;
	Resource = nullptr;
	MappedMemory = nullptr;
	Size = 0;
	OffsetFromResource = 0;
}

void UploadHeapBuffer::Copy(size_t offset, size_t numBytesToCopy, void* data) noexcept
{
	Assert(offset + numBytesToCopy <= Size, "Copy destination region was out-of-bound.");
	memcpy(reinterpret_cast<uint8_t*>(MappedMemory) + offset, data, numBytesToCopy);
}

//--------------------------------------------------------------------------------------
// DefaultHeapBuffer
//--------------------------------------------------------------------------------------

DefaultHeapBuffer::DefaultHeapBuffer(const char* p, ComPtr<ID3D12Resource>&& r) noexcept
{
	m_pathID = XXH3_64bits(p, strlen(p));
	m_resource.Swap(r);

	SET_D3D_OBJ_NAME(m_resource, p);
}

DefaultHeapBuffer::~DefaultHeapBuffer() noexcept
{
	Reset();
}

DefaultHeapBuffer::DefaultHeapBuffer(DefaultHeapBuffer&& rhs) noexcept
	: m_pathID(rhs.m_pathID)
{
	m_resource.Swap(rhs.m_resource);
}

DefaultHeapBuffer& DefaultHeapBuffer::operator=(DefaultHeapBuffer&& rhs) noexcept
{
	if (this == &rhs)
		return *this;

	m_resource.Swap(rhs.m_resource);
	m_pathID = rhs.m_pathID;

	return *this;
}

void DefaultHeapBuffer::Reset() noexcept
{
	if (m_resource)
		App::GetRenderer().GetGpuMemory().ReleaseDefaultHeapBuffer(ZetaMove(*this));

	m_resource = nullptr;
	m_pathID = -1;
}

//--------------------------------------------------------------------------------------
// ReadbackHeapBuffer
//--------------------------------------------------------------------------------------

ReadbackHeapBuffer::ReadbackHeapBuffer(ComPtr<ID3D12Resource>&& r) noexcept
{
	m_resource.Swap(r);

	// buffers have only one subresource
	CheckHR(m_resource->Map(0, nullptr, &m_mappedMemory));
}

ReadbackHeapBuffer::~ReadbackHeapBuffer() noexcept
{
	Reset();
}

ReadbackHeapBuffer::ReadbackHeapBuffer(ReadbackHeapBuffer&& other) noexcept
	: m_mappedMemory(std::exchange(other.m_mappedMemory, nullptr))
{
	m_resource.Swap(other.m_resource);
}

ReadbackHeapBuffer& ReadbackHeapBuffer::operator=(ReadbackHeapBuffer&& rhs) noexcept
{
	if (this == &rhs)
		return *this;

	m_resource.Swap(rhs.m_resource);
	m_mappedMemory = rhs.m_mappedMemory;
	rhs.m_mappedMemory = nullptr;

	return *this;
}

void ReadbackHeapBuffer::Reset() noexcept
{
	if (m_mappedMemory)
	{
		Assert(m_resource, "non-null mapped memory for null resource");
		m_resource->Unmap(0, nullptr);
	}

	m_resource = nullptr;
	m_mappedMemory = nullptr;
}

void ReadbackHeapBuffer::Map() noexcept
{
	if (m_mappedMemory)
		return;

	// buffers have only 1 subresource
	CheckHR(m_resource->Map(0, nullptr, &m_mappedMemory));
}

void ReadbackHeapBuffer::Unmap() noexcept
{
	if (!m_mappedMemory)
		return;

	m_resource->Unmap(0, nullptr);
	m_mappedMemory = nullptr;
}

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------

Texture::Texture(const char* p, ComPtr<ID3D12Resource>&& r) noexcept
{
	m_pathID = XXH3_64bits(p, strlen(p));
	m_resource.Swap(r);

	SET_D3D_OBJ_NAME(m_resource, p);
}

Texture::~Texture() noexcept
{
	Reset();
}

Texture::Texture(Texture&& other) noexcept
{
	m_resource.Swap(other.m_resource);
	m_pathID = other.m_pathID;
}

Texture& Texture::operator=(Texture&& rhs) noexcept
{
	if (this == &rhs)
		return *this;

	m_resource.Swap(rhs.m_resource);
	m_pathID = rhs.m_pathID;

	return *this;
}

void Texture::Reset(bool guardDestruction) noexcept
{
	if (m_resource)
	{
		if (guardDestruction)
			App::GetRenderer().GetGpuMemory().ReleaseTexture(ZetaMove(*this));
		else
			m_resource.Reset();
	}

	m_pathID = -1;
	m_resource = nullptr;
}

void Texture::GetAllocationInfo(size_t* size, size_t* alignment) noexcept
{
	if (!size && !alignment)
		return;

	ID3D12Device* d = nullptr;
	CheckHR(m_resource->GetDevice(IID_PPV_ARGS(&d)));

	auto desc = m_resource->GetDesc();
	D3D12_RESOURCE_ALLOCATION_INFO info = d->GetResourceAllocationInfo(0, 1, &desc);
	d->Release();

	if (size)
		*size = info.SizeInBytes;

	if (alignment)
		*alignment = info.Alignment;
}

//--------------------------------------------------------------------------------------
// GpuMemory
//--------------------------------------------------------------------------------------

GpuMemory::GpuMemory() noexcept
{
}

GpuMemory::~GpuMemory() noexcept
{
}

void GpuMemory::Init() noexcept
{
	auto* device = App::GetRenderer().GetDevice();

	CheckHR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fenceDirect.GetAddressOf())));
	CheckHR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fenceCompute.GetAddressOf())));

	SET_D3D_OBJ_NAME(m_fenceDirect.Get(), "GpuMemory_Dir");
	SET_D3D_OBJ_NAME(m_fenceCompute.Get(), "GpuMemory_Compute");

	int i = 0;

	{
		auto workerThreadIDs = App::GetWorkerThreadIDs();

		for (; i < workerThreadIDs.size(); i++)
		{
			m_threadIDs[i] = workerThreadIDs[i];

			m_threadContext[i].UploadHeap.reset(new(std::nothrow) UploadHeapManager);
			m_threadContext[i].DefaultHeap.reset(new(std::nothrow) DefaultHeapManager);
			m_threadContext[i].ResUploader.reset(new(std::nothrow) ResourceUploadBatch);
		}
	}

	{
		auto backgroundThreadIDs = App::GetBackgroundThreadIDs();
		const size_t numMainThreads = i;

		for (i = 0; i < backgroundThreadIDs.size(); i++)
			m_threadIDs[numMainThreads + i] = backgroundThreadIDs[i];
	}
}

void GpuMemory::BeginFrame() noexcept
{
	const int numThreads = App::GetNumWorkerThreads();

	for (int i = 0; i < numThreads; i++)
		m_threadContext[i].ResUploader->Begin();
}

void GpuMemory::SubmitResourceCopies() noexcept
{
	const int numThreads = App::GetNumWorkerThreads();

	for (int i = 0; i < numThreads; i++)
		m_threadContext[i].ResUploader->End();
}

void GpuMemory::Recycle() noexcept
{
	CheckHR(m_fenceDirect->Signal(m_nextFenceVal));
	CheckHR(m_fenceCompute->Signal(m_nextFenceVal));

	const uint64_t completedFenceValDir = m_fenceDirect->GetCompletedValue();
	const uint64_t completedFenceValCompute = m_fenceCompute->GetCompletedValue();

	const int numMainThreads = App::GetNumWorkerThreads();
	const int numBackgroundThreads = App::GetNumBackgroundThreads();
	
	// TODO FrameAllocator can't be used with background tasks and PoolAllocator isn't a good 
	// fit here due to threading issues -- come up with a custom allocator than handles allocations 
	// that persist over multiple frames
	SmallVector<PendingTexture> textures;
	size_t n = 0;

	for (int i = 0; i < numMainThreads + numBackgroundThreads; i++)
		n += m_threadContext[i].ToReleaseTextures.size();

	textures.resize(n);		// n = 0 is ok

	for (int i = 0; i < numMainThreads + numBackgroundThreads; i++)
	{
		if (i < numMainThreads)
		{
			m_threadContext[i].UploadHeap->Recycle();
			m_threadContext[i].DefaultHeap->Recycle();			// TODO move to a background task
			m_threadContext[i].UploadHeap->GarbageCollect();	// lanches a background task
		}

		for (auto it = m_threadContext[i].ToReleaseTextures.begin(); it != m_threadContext[i].ToReleaseTextures.end();)
		{
			if (it->ReleaseFence <= completedFenceValDir && it->ReleaseFence <= completedFenceValCompute)
			{
				const size_t pos = it - m_threadContext[i].ToReleaseTextures.begin();
				textures.emplace_back(ZetaMove(*it));
				it = m_threadContext[i].ToReleaseTextures.erase(pos);
			}
			else
				it++;
		}
	}

	if (!textures.empty())
	{
		Task t("Releasing textures", TASK_PRIORITY::BACKGRUND, [this, Textures = ZetaMove(textures)]()
			{
				Assert(Textures.size() > 0, "input texture vec is empty");
				// resources in textures are free'd now
			});

		App::SubmitBackground(ZetaMove(t));
	}

	m_nextFenceVal++;
}

void GpuMemory::Shutdown() noexcept
{
	CheckHR(m_fenceDirect->Signal(m_nextFenceVal));
	CheckHR(m_fenceCompute->Signal(m_nextFenceVal));

	HANDLE h1 = CreateEventA(nullptr, false, false, "");
	HANDLE h2 = CreateEventA(nullptr, false, false, "");

	CheckHR(m_fenceDirect->SetEventOnCompletion(m_nextFenceVal, h1));
	CheckHR(m_fenceCompute->SetEventOnCompletion(m_nextFenceVal, h2));

	HANDLE handles[] = { h1, h2 };

	WaitForMultipleObjects(ZetaArrayLen(handles), handles, true, INFINITE);

	auto mainThreadIDs = App::GetWorkerThreadIDs();

	for (int i = 0; i < mainThreadIDs.size(); i++)
	{
		m_threadContext[i].ResUploader.reset();
		m_threadContext[i].DefaultHeap.reset();
		m_threadContext[i].UploadHeap.reset();
	}

	auto allThreadIDs = App::GetAllThreadIDs();

	for (int i = 0; i < allThreadIDs.size(); i++)
		m_threadContext[i].ToReleaseTextures.free_memory();
}

UploadHeapBuffer GpuMemory::GetUploadHeapBuffer(size_t sizeInBytes, size_t alignment) noexcept
{
	const int idx = GetIndexForThread();
	//const size_t alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
	const size_t alignedSize = (sizeInBytes + alignment - 1) & ~(alignment - 1);

	return m_threadContext[idx].UploadHeap->GetBuffer(idx, sizeInBytes, alignment);
}

void GpuMemory::ReleaseUploadHeapBuffer(UploadHeapBuffer& buff) noexcept
{
	// might not be the original UploadHeap
	m_threadContext[buff.PageHandle.ThreadIdx].UploadHeap->ReleaseBuffer(buff.PageHandle.PoolIdx, buff.Resource);
}

ReadbackHeapBuffer GpuMemory::GetReadbackHeapBuffer(size_t sizeInBytes) noexcept
{
	auto* device = App::GetRenderer().GetDevice();

	D3D12_HEAP_PROPERTIES readbackHeap = Direct3DHelper::ReadbackHeapProp();
	D3D12_RESOURCE_DESC desc = Direct3DHelper::BufferResourceDesc(sizeInBytes);

	ComPtr<ID3D12Resource> buff;

	CheckHR(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
		&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(buff.GetAddressOf())));

	return ReadbackHeapBuffer(ZetaMove(buff));
}

DefaultHeapBuffer GpuMemory::GetDefaultHeapBuffer(const char* name, size_t size,
	D3D12_RESOURCE_STATES initState, bool allowUAV, bool initToZero) noexcept
{
	const int idx = GetIndexForThread();
	return m_threadContext[idx].DefaultHeap->GetBuffer(name, size, initState, allowUAV, initToZero);
}

DefaultHeapBuffer GpuMemory::GetDefaultHeapBufferAndInit(const char* name, size_t sizeInBytes,
	D3D12_RESOURCE_STATES postCopyState, bool allowUAV, void* data) noexcept
{
	const int idx = GetIndexForThread();

	// Note: setting the state to D3D12_RESOURCE_STATE_COPY_DEST leads to a Warning
	auto buff = m_threadContext[idx].DefaultHeap->GetBuffer(name, 
		sizeInBytes,
		D3D12_RESOURCE_STATE_COMMON, 
		allowUAV, 
		false);

	auto desc = buff.GetDesc();
	m_threadContext[idx].ResUploader->UploadBuffer(idx, *m_threadContext[idx].UploadHeap,
		buff.GetResource(), data, sizeInBytes, postCopyState);

	return buff;
}

void GpuMemory::ReleaseDefaultHeapBuffer(DefaultHeapBuffer&& buff) noexcept
{
	const int idx = GetIndexForThread();
	m_threadContext[idx].DefaultHeap->ReleaseBuffer(ZetaMove(buff.m_resource));
}

void GpuMemory::ReleaseTexture(Texture&& t) noexcept
{
	const int idx = GetIndexForThread();

	m_threadContext[idx].ToReleaseTextures.emplace_back(PendingTexture{
			.Res = ZetaMove(t.m_resource),
			.ReleaseFence = m_nextFenceVal
		});
}

Texture GpuMemory::GetTexture2D(const char* name, uint64_t width, uint32_t height, DXGI_FORMAT format,
	D3D12_RESOURCE_STATES initialState, uint32_t flags, uint16_t mipLevels, D3D12_CLEAR_VALUE* clearVal) noexcept
{
	Assert(width < D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid width");
	Assert(height < D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid height");
	Assert(mipLevels <= D3D12_REQ_MIP_LEVELS, "Invalid number of mip levels");
	Assert(((flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET) & (flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL)) == 0,
		"Texures can't be used as both Render Target and Depth Stencil");
	Assert(((flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL) & (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)) == 0,
		"A Depth-Stencil texture can't be used for unordered access");

	D3D12_RESOURCE_FLAGS f = D3D12_RESOURCE_FLAG_NONE;

	if (flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL)
		f |= (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL & ~D3D12_RESOURCE_FLAG_NONE);
	if (flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET)
		f |= (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET & ~D3D12_RESOURCE_FLAG_NONE);
	if (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)
		f |= (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS & ~D3D12_RESOURCE_FLAG_NONE);

	D3D12_HEAP_PROPERTIES defaultHeap = Direct3DHelper::DefaultHeapProp();
	D3D12_RESOURCE_DESC desc = Direct3DHelper::Tex2D(format, width, height, 1, mipLevels, f);

	ComPtr<ID3D12Resource> texture;
	auto* device = App::GetRenderer().GetDevice();

	D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;

	if ((flags & TEXTURE_FLAGS::INIT_TO_ZERO) == 0)
		heapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
	//const uint8_t isRtOrDS = flags & (TEXTURE_FLAGS::ALLOW_RENDER_TARGET | TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL);
	//heapFlags |= isRtOrDS ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;

	CheckHR(device->CreateCommittedResource(&defaultHeap,
		heapFlags,
		&desc,
		initialState,
		clearVal,
		IID_PPV_ARGS(texture.GetAddressOf())));

	return Texture(name, ZetaMove(texture));
}

Texture GpuMemory::GetTexture3D(const char* name, uint64_t width, uint32_t height, uint32_t depth,
	DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState, uint32_t flags, uint16_t mipLevels) noexcept
{
	Assert(width < D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION, "Invalid width");
	Assert(height < D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION, "Invalid height");
	Assert(depth < D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION, "Invalid depth");
	Assert(mipLevels <= D3D12_REQ_MIP_LEVELS, "Invalid number of mip levels");
	Assert(!(flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL), "3D Texure can't be used as Depth Stencil");

	D3D12_RESOURCE_FLAGS f = D3D12_RESOURCE_FLAG_NONE;

	if (flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET)
		f |= (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET & ~D3D12_RESOURCE_FLAG_NONE);
	if (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)
		f |= (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS & ~D3D12_RESOURCE_FLAG_NONE);

	D3D12_HEAP_PROPERTIES defaultHeap = Direct3DHelper::DefaultHeapProp();
	D3D12_RESOURCE_DESC desc = Direct3DHelper::Tex3D(format, width, height, depth, mipLevels, f);

	ComPtr<ID3D12Resource> texture;
	auto* device = App::GetRenderer().GetDevice();

	D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
	if ((flags & TEXTURE_FLAGS::INIT_TO_ZERO) == 0)
		heapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

	CheckHR(device->CreateCommittedResource(&defaultHeap,
		heapFlags,
		&desc,
		initialState,
		nullptr,
		IID_PPV_ARGS(texture.GetAddressOf())));

	return Texture(ZetaMove(name), ZetaMove(texture));
}

Texture GpuMemory::GetTextureCube(const char* name, uint64_t width, uint32_t height, DXGI_FORMAT format,
	D3D12_RESOURCE_STATES initialState, uint32_t flags, uint16_t mipLevels) noexcept
{
	Check(width < D3D12_REQ_TEXTURECUBE_DIMENSION, "Invalid width");
	Check(height < D3D12_REQ_TEXTURECUBE_DIMENSION, "Invalid height");
	Check(mipLevels <= D3D12_REQ_MIP_LEVELS, "Invalid number of mip levels");
	Check(!(flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL), "Texture Cube can't be used as Depth Stencil");

	D3D12_RESOURCE_FLAGS f = D3D12_RESOURCE_FLAG_NONE;

	if (flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET)
		f |= (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET & ~D3D12_RESOURCE_FLAG_NONE);
	if (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)
		f |= (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS & ~D3D12_RESOURCE_FLAG_NONE);

	D3D12_HEAP_PROPERTIES defaultHeap = Direct3DHelper::DefaultHeapProp();
	D3D12_RESOURCE_DESC desc = Direct3DHelper::Tex2D(format, width, height, 6, mipLevels, f);

	ComPtr<ID3D12Resource> texture;
	auto* device = App::GetRenderer().GetDevice();

	D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
	if ((flags & TEXTURE_FLAGS::INIT_TO_ZERO) == 0)
		heapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

	CheckHR(device->CreateCommittedResource(&defaultHeap,
		heapFlags,
		&desc,
		initialState,
		nullptr,
		IID_PPV_ARGS(texture.GetAddressOf())));

	return Texture(ZetaMove(name), ZetaMove(texture));
}

Texture GpuMemory::GetTexture2DFromDisk(const char* p) noexcept
{
	SmallVector<D3D12_SUBRESOURCE_DATA, App::ThreadAllocator, 10> subresources;
	std::unique_ptr<uint8_t[]> ddsData;		// must remain alive until CopyTextureRegion() has been called
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t mipCount;
	DXGI_FORMAT format;

	auto* device = App::GetRenderer().GetDevice();
	Direct3DHelper::LoadDDSFromFile(p, subresources, format, ddsData, width, height, depth, mipCount);

	// not allowed to be RT or Depth-Stencil
	Texture tex = GetTexture2D(p, width, height, format, D3D12_RESOURCE_STATE_COPY_DEST, 0, mipCount);

	const int idx = GetIndexForThread();

	m_threadContext[idx].ResUploader->UploadTexture(idx, *m_threadContext[idx].UploadHeap,
		tex.GetResource(),
		0,
		subresources.begin(),
		(uint32_t)subresources.size(),
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

	/*
		resourceUpload.Transition(
			*texture,
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	*/

	return tex;
}

Texture GpuMemory::GetTexture2DAndInit(const char* name, uint64_t width, uint32_t height, DXGI_FORMAT format,
	D3D12_RESOURCE_STATES postCopyState, uint8_t* pixels, uint32_t flags) noexcept
{
	Texture t = GetTexture2D(name, width, height, format, D3D12_RESOURCE_STATE_COPY_DEST, flags);
	const int idx = GetIndexForThread();

	m_threadContext[idx].ResUploader->UploadTexture(idx, *m_threadContext[idx].UploadHeap,
		t.GetResource(),
		pixels,
		postCopyState);

	return t;
}

int GpuMemory::GetIndexForThread() noexcept
{
	const uint32_t tid = std::bit_cast<THREAD_ID_TYPE, std::thread::id>(std::this_thread::get_id());

	for (int i = 0; i < MAX_NUM_THREADS; i++)
	{
		if (m_threadIDs[i] == tid)
			return i;
	}

	Check(false, "Should be unreachable.");

	return -1;
}
