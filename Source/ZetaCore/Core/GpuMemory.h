#pragma once

#include "Direct3DUtil.h"
#include "../Utility/Span.h"
#include "../Support/OffsetAllocator.h"

namespace ZetaRay::App::Filesystem
{
	struct Path;
}

namespace ZetaRay::Core::GpuMemory
{
	static constexpr uint64_t INVALID_ID = uint64_t(-1);

	//
	// Types
	//
	
	struct UploadHeapBuffer
	{
		UploadHeapBuffer() = default;
		UploadHeapBuffer(ID3D12Resource* r, void* mapped, const Support::OffsetAllocator::Allocation& alloc = Support::OffsetAllocator::Allocation::Empty());
		~UploadHeapBuffer();
		UploadHeapBuffer(UploadHeapBuffer&& other);
		UploadHeapBuffer& operator=(UploadHeapBuffer&& other);

		void Reset();
		ZetaInline bool IsInitialized() const { return m_resource != nullptr; }
		ZetaInline ID3D12Resource* Resource() { return m_resource; }
		ZetaInline D3D12_GPU_VIRTUAL_ADDRESS GpuVA() const { return m_resource->GetGPUVirtualAddress() + m_allocation.Offset; }
		ZetaInline void* MappedMemeory() { return m_mappedMemory; }
		ZetaInline const Support::OffsetAllocator::Allocation& Allocation() const { return m_allocation; }
		ZetaInline uint32_t Offset() const { return m_allocation.Offset; }
		void Copy(uint32_t offset, uint32_t numBytesToCopy, void* data);

	private:
		ID3D12Resource* m_resource = nullptr;
		void* m_mappedMemory = nullptr;
		Support::OffsetAllocator::Allocation m_allocation = Support::OffsetAllocator::Allocation::Empty();
	};

	struct UploadHeapArena
	{
		struct Allocation
		{
			ID3D12Resource* Res;
			void* Mapped;
			uint32_t Offset = 0;
		};

		struct Block
		{
			ID3D12Resource* Res;
			uint32_t Offset;
			void* Mapped;
		};

		explicit UploadHeapArena(uint32_t sizeInBytes);
		~UploadHeapArena();
		UploadHeapArena(UploadHeapArena&& rhs);

		Util::Span<Block> Blocks() { return m_blocks; }
		Allocation SubAllocate(uint32_t size, uint32_t alignment = 1);

	private:
		Util::SmallVector<Block, Support::SystemAllocator, 4> m_blocks;
		uint32_t m_size;
	};

	struct ReadbackHeapBuffer
	{
		ReadbackHeapBuffer() = default;
		explicit ReadbackHeapBuffer(ComPtr<ID3D12Resource>&& r);
		~ReadbackHeapBuffer();
		ReadbackHeapBuffer(ReadbackHeapBuffer&&);
		ReadbackHeapBuffer& operator=(ReadbackHeapBuffer&&);

		ZetaInline bool IsInitialized() { return m_resource != nullptr; }
		void Reset();

		ZetaInline D3D12_GPU_VIRTUAL_ADDRESS GpuVA() const
		{
			Assert(m_resource, "ReadbackHeapBuffer hasn't been initialized.");
			return m_resource->GetGPUVirtualAddress();
		}
		ZetaInline ID3D12Resource* Resource()
		{
			Assert(m_resource, "ReadbackHeapBuffer hasn't been initialized.");
			return m_resource.Get();
		}
		ZetaInline D3D12_RESOURCE_DESC Desc() const
		{
			Assert(m_resource, "ReadbackHeapBuffer hasn't been initialized.");
			return m_resource->GetDesc();
		}

		// From MS Docs:
		// "Resources on D3D12_HEAP_TYPE_READBACK heaps do not support persistent map. Map and Unmap must 
		// be called between CPU and GPU accesses to the same memory address on some system architectures, 
		// when the page caching behavior is write-back."
		void Map();
		void Unmap();
		ZetaInline bool IsMapped() const { return m_mappedMemory != nullptr; }
		ZetaInline void* MappedMemory()
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
		DefaultHeapBuffer() = default;
		DefaultHeapBuffer(const char* p, ID3D12Resource* r);
		~DefaultHeapBuffer();
		DefaultHeapBuffer(DefaultHeapBuffer&&);
		DefaultHeapBuffer& operator=(DefaultHeapBuffer&&);

		void Reset();
		ZetaInline bool IsInitialized() const { return m_resource != nullptr; }
		ZetaInline D3D12_GPU_VIRTUAL_ADDRESS GpuVA() const
		{
			Assert(m_resource, "Buffer hasn't been initialized.");
			return m_resource->GetGPUVirtualAddress();
		}
		ZetaInline ID3D12Resource* Resource()
		{
			Assert(m_resource, "Buffer hasn't been initialized.");
			return m_resource;
		}
		ZetaInline D3D12_RESOURCE_DESC Desc() const
		{
			Assert(m_resource, "Buffer hasn't been initialized.");
			return m_resource->GetDesc();
		}
		ZetaInline uint64_t ID() const
		{
			Assert(m_resource, "Buffer hasn't been initialized.");
			return m_ID;
		}

	private:
		uint64_t m_ID = INVALID_ID;
		ID3D12Resource* m_resource = nullptr;
	};

	struct Texture
	{
		Texture() = default;
		Texture(const char* p, ID3D12Resource* r);
		~Texture();
		Texture(Texture&&);
		Texture& operator=(Texture&&);

		void Reset(bool waitForGpu = true, bool checkRefCount = true);
		ZetaInline bool IsInitialized() const { return m_ID != -1; }
		ZetaInline D3D12_GPU_VIRTUAL_ADDRESS GpuVA() const
		{
			Assert(m_resource, "Texture hasn't been initialized.");
			return m_resource->GetGPUVirtualAddress();
		}
		ZetaInline ID3D12Resource* Resource()
		{
			Assert(m_resource, "Texture hasn't been initialized.");
			return m_resource;
		}
		ZetaInline uint64_t ID() const
		{
			Assert(m_resource, "Texture hasn't been initialized.");
			return m_ID;
		}
		D3D12_RESOURCE_DESC Desc() const
		{
			Assert(m_resource, "Texture hasn't been initialized.");
			return m_resource->GetDesc();
		}

	private:
		uint64_t m_ID = INVALID_ID;
		ID3D12Resource* m_resource = nullptr;
	};

	//
	// API
	//

	enum CREATE_TEXTURE_FLAGS
	{
		ALLOW_RENDER_TARGET = 1 << 0,
		ALLOW_DEPTH_STENCIL = 1 << 1,
		ALLOW_UNORDERED_ACCESS = 1 << 2,
		INIT_TO_ZERO = 1 << 3
	};

	void Init();
	void BeginFrame();
	void SubmitResourceCopies();
	void Recycle();
	void Shutdown();

	UploadHeapBuffer GetUploadHeapBuffer(uint32_t sizeInBytes, uint32_t alignment = 4, bool forceSeperate = false);
	void ReleaseUploadHeapBuffer(UploadHeapBuffer& buffer);
	void ReleaseUploadHeapArena(UploadHeapArena& arena);

	ReadbackHeapBuffer GetReadbackHeapBuffer(uint32_t sizeInBytes);

	DefaultHeapBuffer GetDefaultHeapBuffer(const char* name, uint32_t size,
		D3D12_RESOURCE_STATES initialState, bool allowUAV, bool initToZero = false);
	DefaultHeapBuffer GetDefaultHeapBuffer(const char* name, uint32_t size,
		bool isRtAs, bool allowUAV, bool initToZero = false);
	DefaultHeapBuffer GetDefaultHeapBufferAndInit(const char* name,
		uint32_t sizeInBytes,
		bool allowUAV, 
		void* data,
		bool forceSeperateUploadBuffer = false);
	void UploadToDefaultHeapBuffer(DefaultHeapBuffer& buffer, uint32_t sizeInBytes, void* data);
	void ReleaseDefaultHeapBuffer(DefaultHeapBuffer& buffer);
	void ReleaseTexture(Texture& textue);

	Texture GetTexture2D(const char* name, uint64_t width, uint32_t height, DXGI_FORMAT format,
		D3D12_RESOURCE_STATES initialState, uint32_t flags = 0, uint16_t mipLevels = 1,
		D3D12_CLEAR_VALUE* clearVal = nullptr);

	Texture GetTexture2D(const char* name, uint64_t width, uint32_t height, DXGI_FORMAT format,
		D3D12_BARRIER_LAYOUT initialLayout, uint32_t flags = 0, uint16_t mipLevels = 1,
		D3D12_CLEAR_VALUE* clearVal = nullptr);

	Texture GetTexture3D(const char* name, uint64_t width, uint32_t height, uint16_t depth,
		DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState,
		uint32_t flags = 0, uint16_t mipLevels = 1);

	Texture GetTextureCube(const char* name, uint64_t width, uint32_t height,
		DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState,
		uint32_t flags = 0, uint16_t mipLevels = 1);

	Core::Direct3DUtil::LOAD_DDS_RESULT GetTexture2DFromDisk(const App::Filesystem::Path& p, Texture& t);
	Core::Direct3DUtil::LOAD_DDS_RESULT GetTexture2DFromDisk(const App::Filesystem::Path& p, Texture& t, UploadHeapArena& arena);
	Core::Direct3DUtil::LOAD_DDS_RESULT GetTexture3DFromDisk(const App::Filesystem::Path& p, Texture& t);
	Texture GetTexture2DAndInit(const char* p, uint64_t width, uint32_t height, DXGI_FORMAT format,
		D3D12_RESOURCE_STATES initialState, uint8_t* pixels, uint32_t flags = 0);
}