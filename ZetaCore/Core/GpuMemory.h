#pragma once

#include "Device.h"
#include "../Utility/Error.h"
#include "../Utility/SmallVector.h"
#include "../App/App.h"
#include <memory>

namespace ZetaRay::Core
{
	namespace Internal
	{
		struct LinearAllocatorPage;
		struct UploadHeapManager;
		struct DefaultHeapManager;
		struct ResourceUploadBatch;

		struct PageHandle
		{
			void Reset()
			{
				PoolIdx = -1;
				ThreadIdx = -1;
			}

			int PoolIdx = -1;
			int ThreadIdx = -1;
		};
	}

	// "Resources in this heap must be created with D3D12_RESOURCE_STATE_GENERIC_READ and cannot be changed away from this."
	struct UploadHeapBuffer
	{
		friend class GpuMemory;

		UploadHeapBuffer() noexcept = default;
		UploadHeapBuffer(Internal::PageHandle page,
			size_t offsetFromResource,
			D3D12_GPU_VIRTUAL_ADDRESS gpuAddress,
			ID3D12Resource* resource,
			void* memory,
			size_t size) noexcept;

		~UploadHeapBuffer() noexcept;

		UploadHeapBuffer(UploadHeapBuffer&& other) noexcept;
		UploadHeapBuffer& operator=(UploadHeapBuffer&&) noexcept;

		bool IsInitialized() noexcept { return Resource != nullptr; }
		ZetaInline D3D12_GPU_VIRTUAL_ADDRESS GetGpuVA() const { return GpuAddress; }
		ZetaInline ID3D12Resource* GetResource() { return Resource; }
		ZetaInline size_t GetSize() const { return Size; }
		ZetaInline size_t GetOffset() const { return OffsetFromResource; }

		void Reset() noexcept;
		void Copy(size_t offset, size_t numBytesToCopy, void* data) noexcept;

	private:
		D3D12_GPU_VIRTUAL_ADDRESS GpuAddress = {};
		ID3D12Resource* Resource = nullptr;
		void* MappedMemory = nullptr;
		size_t Size = 0;
		size_t OffsetFromResource = 0;
		Internal::PageHandle PageHandle;
	};

	// "Resources in this heap must be created with D3D12_RESOURCE_STATE_COPY_DEST and cannot be changed away from this."
	struct ReadbackHeapBuffer
	{
		ReadbackHeapBuffer() noexcept = default;
		explicit ReadbackHeapBuffer(ComPtr<ID3D12Resource>&& r) noexcept;
		~ReadbackHeapBuffer() noexcept;

		ReadbackHeapBuffer(ReadbackHeapBuffer&&) noexcept;
		ReadbackHeapBuffer& operator=(ReadbackHeapBuffer&&) noexcept;

		void Reset() noexcept;

		ZetaInline D3D12_GPU_VIRTUAL_ADDRESS GetGpuVA() const { return m_resource->GetGPUVirtualAddress(); }
		ZetaInline ID3D12Resource* GetResource() { return m_resource.Get(); }
		ZetaInline size_t Size() const { return m_resource->GetDesc().Width; }
		
		// reminder: source and destination resources for CopyResource() can't be mapped
		// From MS Docs:
		// "Resources on D3D12_HEAP_TYPE_READBACK heaps do not support persistent map. Map and Unmap must 
		// be called between CPU and GPU accesses to the same memory address on some system architectures, 
		// when the page caching behavior is write-back."
		void Map() noexcept;
		void Unmap() noexcept;
		ZetaInline void* GetMappedMemory() noexcept
		{ 
			Assert(m_mappedMemory, "Resource is not mapped.");
			return m_mappedMemory;
		}

	private:
		ComPtr<ID3D12Resource> m_resource;
		void* m_mappedMemory = nullptr;
	};

	struct DefaultHeapBuffer
	{
		friend class GpuMemory;

		DefaultHeapBuffer() noexcept = default;
		DefaultHeapBuffer(const char* p, ComPtr<ID3D12Resource>&& r) noexcept;
		~DefaultHeapBuffer() noexcept;

		DefaultHeapBuffer(DefaultHeapBuffer&&) noexcept;
		DefaultHeapBuffer& operator=(DefaultHeapBuffer&&) noexcept;

		void Reset() noexcept;
		ZetaInline bool IsInitialized() const noexcept { return m_resource != nullptr; }
		ZetaInline D3D12_GPU_VIRTUAL_ADDRESS GetGpuVA() const { return m_resource->GetGPUVirtualAddress(); }
		ZetaInline ID3D12Resource* GetResource() { return m_resource.Get(); }
		ZetaInline D3D12_RESOURCE_DESC GetDesc() const { return m_resource->GetDesc(); }
		ZetaInline uint64_t GetPathID() const { return m_pathID; }

		void GetAllocationInfo(size_t* size = nullptr, size_t* alignment = nullptr) const
		{
			if (!size && !alignment)
				return;

			if (size)
				*size = m_resource->GetDesc().Width;

			if (alignment)
				*alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
		}

	private:
		uint64_t m_pathID = uint64_t(-1);
		ComPtr<ID3D12Resource> m_resource;
	};

	struct Texture
	{
		friend class GpuMemory;

		Texture() noexcept = default;
		Texture(const char* p, ComPtr<ID3D12Resource>&& r) noexcept;
		~Texture() noexcept;

		Texture(Texture&&) noexcept;
		Texture& operator=(Texture&&) noexcept;

		ZetaInline bool IsInitialized() const { return m_pathID != -1; }
		void Reset(bool guardDestruction = true) noexcept;

		ZetaInline D3D12_GPU_VIRTUAL_ADDRESS GetGpuVA() const { return m_resource->GetGPUVirtualAddress(); }
		ZetaInline ID3D12Resource* GetResource() { return m_resource.Get(); }
		ZetaInline uint64_t GetPathID() const { return m_pathID; }
		void GetAllocationInfo(size_t* size = nullptr, size_t* alignment = nullptr) noexcept;

	private:
		uint64_t m_pathID = uint64_t(-1);
		ComPtr<ID3D12Resource> m_resource;
	};

	enum TEXTURE_FLAGS
	{
		ALLOW_RENDER_TARGET = 1 << 0,
		ALLOW_DEPTH_STENCIL = 1 << 1,
		ALLOW_UNORDERED_ACCESS = 1 << 2,
		INIT_TO_ZERO = 1 << 3
	};

	class GpuMemory
	{
	public:
		GpuMemory() noexcept;
		~GpuMemory() noexcept;

		GpuMemory(GpuMemory&&) = delete;
		GpuMemory& operator=(GpuMemory&&) = delete;

		void Init() noexcept;
		void BeginFrame() noexcept;
		void SubmitResourceCopies() noexcept;
		void Recycle() noexcept;
		void Shutdown() noexcept;

		UploadHeapBuffer GetUploadHeapBuffer(size_t sizeInBytes, size_t alignment = 16) noexcept;
		void ReleaseUploadHeapBuffer(UploadHeapBuffer& buff) noexcept;

		ReadbackHeapBuffer GetReadbackHeapBuffer(size_t sizeInBytes) noexcept;

		DefaultHeapBuffer GetDefaultHeapBuffer(const char* n, size_t size,
			D3D12_RESOURCE_STATES initState, bool allowUAV, bool initToZero = false) noexcept;
		DefaultHeapBuffer GetDefaultHeapBufferAndInit(const char* n,
			size_t sizeInBytes,
			D3D12_RESOURCE_STATES postCopyState,
			bool allowUAV, 
			void* data) noexcept;
		void UploadToDefaultHeapBuffer(const DefaultHeapBuffer& buff, size_t sizeInBytes, void* data) noexcept;
		void ReleaseDefaultHeapBuffer(DefaultHeapBuffer&& buff) noexcept;
		void ReleaseTexture(Texture&& t) noexcept;

		//Texture GetTexture1D(const char* n, uint64_t width, DXGI_FORMAT format,
		//	D3D12_RESOURCE_STATES initialState, uint32_t flags = 0, uint16_t mipLevels = 1) noexcept;

		Texture GetTexture2D(const char* n, uint64_t width, uint32_t height, DXGI_FORMAT format,
			D3D12_RESOURCE_STATES initialState, uint32_t flags = 0, uint16_t mipLevels = 1,
			D3D12_CLEAR_VALUE* clearVal = nullptr) noexcept;

		Texture GetTexture3D(const char* n, uint64_t width, uint32_t height, uint16_t depth,
			DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState,
			uint32_t flags = 0, uint16_t mipLevels = 1) noexcept;

		Texture GetTextureCube(const char* n, uint64_t width, uint32_t height,
			DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState,
			uint32_t flags = 0, uint16_t mipLevels = 1) noexcept;

		Texture GetTexture2DFromDisk(const char* p) noexcept;
		Texture GetTexture3DFromDisk(const char* p) noexcept;
		Texture GetTexture2DAndInit(const char* p, uint64_t width, uint32_t height, DXGI_FORMAT format,
			D3D12_RESOURCE_STATES initialState, uint8_t* pixels, uint32_t flags = 0) noexcept;

	private:
		int GetIndexForThread() noexcept;

		static constexpr D3D12_RESOURCE_FLAGS VALID_BUFFER_FLAGS =
			D3D12_RESOURCE_FLAG_NONE |
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
			D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

		struct PendingTexture
		{
			ComPtr<ID3D12Resource> Res;
			uint64_t ReleaseFence;
		};

		struct ThreadContext
		{
			std::unique_ptr<Internal::UploadHeapManager> UploadHeap;
			std::unique_ptr<Internal::DefaultHeapManager> DefaultHeap;
			std::unique_ptr<Internal::ResourceUploadBatch> ResUploader;

			Util::SmallVector<PendingTexture> ToReleaseTextures;
		};

		ThreadContext m_threadContext[MAX_NUM_THREADS];
		uint32_t alignas(64) m_threadIDs[MAX_NUM_THREADS];

		ComPtr<ID3D12Fence> m_fenceDirect;
		ComPtr<ID3D12Fence> m_fenceCompute;
		uint64_t m_nextFenceVal = 1;	// no need to be atomic
	};
}