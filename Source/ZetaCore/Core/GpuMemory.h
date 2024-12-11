#pragma once

#include "Direct3DUtil.h"
#include "../Utility/Span.h"
#include "../Support/OffsetAllocator.h"

namespace ZetaRay::Core::GpuMemory
{
    //
    // Types
    //

    enum TEXTURE_FLAGS
    {
        NONE = 0,
        ALLOW_RENDER_TARGET = 1 << 0,
        ALLOW_DEPTH_STENCIL = 1 << 1,
        ALLOW_UNORDERED_ACCESS = 1 << 2,
        INIT_TO_ZERO = 1 << 3
    };

    enum class RESOURCE_HEAP_TYPE
    {
        COMMITTED,
        PLACED,
        COUNT
    };

    template<int N = 1>
    struct PlacedResourceList
    {
        void PushBuffer(uint32_t sizeInBytes, bool allowUAV, bool isRtAs)
        {
            D3D12_RESOURCE_FLAGS f = allowUAV ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : 
                D3D12_RESOURCE_FLAG_NONE;
            if (isRtAs)
                f |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;

            D3D12_RESOURCE_DESC1 desc = Direct3DUtil::BufferResourceDesc1(sizeInBytes, f);
            m_descs.push_back(desc);

            //m_size = Math::AlignUp(m_size, (uint64_t)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
            //m_size += sizeInBytes;
        }
        void PushTex2D(DXGI_FORMAT format, uint64_t width, uint32_t height, uint32_t flags = TEXTURE_FLAGS::NONE)
        {
            D3D12_RESOURCE_FLAGS f = D3D12_RESOURCE_FLAG_NONE;

            if (flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL)
                f |= (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL & ~D3D12_RESOURCE_FLAG_NONE);
            if (flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET)
                f |= (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET & ~D3D12_RESOURCE_FLAG_NONE);
            if (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)
                f |= (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS & ~D3D12_RESOURCE_FLAG_NONE);

            auto desc = Direct3DUtil::Tex2D1(format, width, height, 1, 1, f);
            m_descs.push_back(desc);
        }
        void End()
        {
            m_infos.resize(N);
            D3D12_RESOURCE_ALLOCATION_INFO info = Direct3DUtil::AllocationInfo(m_descs, m_infos);
            m_size = info.SizeInBytes;
        }
        ZetaInline uint64_t TotalSizeInBytes() const { return m_size; }
        ZetaInline Util::Span<D3D12_RESOURCE_ALLOCATION_INFO1> AllocInfos() const { return m_infos; }

    private:
        Util::SmallVector<D3D12_RESOURCE_DESC1, Support::SystemAllocator, N> m_descs;
        Util::SmallVector<D3D12_RESOURCE_ALLOCATION_INFO1, Support::SystemAllocator, N> m_infos;
        uint64_t m_size = 0;
    };

    struct UploadHeapBuffer
    {
        UploadHeapBuffer() = default;
        UploadHeapBuffer(ID3D12Resource* r, void* mapped, const Support::OffsetAllocator::Allocation& alloc = 
            Support::OffsetAllocator::Allocation::Empty());
        ~UploadHeapBuffer();
        UploadHeapBuffer(UploadHeapBuffer&& other);
        UploadHeapBuffer& operator=(UploadHeapBuffer&& other);

        void Reset();
        ZetaInline bool IsInitialized() const { return m_resource != nullptr; }
        ZetaInline ID3D12Resource* Resource() { return m_resource; }
        ZetaInline D3D12_GPU_VIRTUAL_ADDRESS GpuVA() const { return m_resource->GetGPUVirtualAddress() + m_allocation.Offset; }
        ZetaInline void* MappedMemory() { return m_mappedMemory; }
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
        explicit ReadbackHeapBuffer(ID3D12Resource* r);
        ~ReadbackHeapBuffer();
        ReadbackHeapBuffer(ReadbackHeapBuffer&&);
        ReadbackHeapBuffer& operator=(ReadbackHeapBuffer&&);

        ZetaInline bool IsInitialized() { return m_resource != nullptr; }
        void Reset(bool waitForGPU = true);
        ZetaInline D3D12_GPU_VIRTUAL_ADDRESS GpuVA() const
        {
            Assert(m_resource, "ReadbackHeapBuffer hasn't been initialized.");
            return m_resource->GetGPUVirtualAddress();
        }
        ZetaInline ID3D12Resource* Resource()
        {
            Assert(m_resource, "ReadbackHeapBuffer hasn't been initialized.");
            return m_resource;
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
            //Assert(m_mappedMemory, "Resource is not mapped.");
            return m_mappedMemory;
        }

    private:
        ID3D12Resource* m_resource = nullptr;
        void* m_mappedMemory = nullptr;
    };

    struct Buffer
    {
        using ID_TYPE = uint32_t;
        static constexpr ID_TYPE INVALID_ID = UINT32_MAX;

        Buffer() = default;
        Buffer(const char* p, ID3D12Resource* r, RESOURCE_HEAP_TYPE heapType);
        ~Buffer();
        Buffer(Buffer&&);
        Buffer& operator=(Buffer&&);

        void Reset(bool waitForGpu = true);
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
        ZetaInline ID_TYPE ID() const
        {
            Assert(m_resource, "Buffer hasn't been initialized.");
            return m_ID;
        }

    private:
        ID3D12Resource* m_resource = nullptr;
        ID_TYPE m_ID = INVALID_ID;
        RESOURCE_HEAP_TYPE m_heapType;
    };

    struct Texture
    {
        using ID_TYPE = uint32_t;
        static constexpr ID_TYPE INVALID_ID = UINT32_MAX;

        Texture() = default;
        Texture(const char* name, ID3D12Resource* res, RESOURCE_HEAP_TYPE heapType);
        Texture(ID_TYPE id, ID3D12Resource* res, RESOURCE_HEAP_TYPE heapType,
            const char* dbgName = nullptr);
        ~Texture();
        Texture(Texture&&);
        Texture& operator=(Texture&&);

        void Reset(bool waitForGpu = true, bool checkRefCount = true);
        ZetaInline bool IsInitialized() const { return m_ID != INVALID_ID; }
        ZetaInline ID3D12Resource* Resource()
        {
            Assert(m_resource, "Texture hasn't been initialized.");
            return m_resource;
        }
        ZetaInline ID_TYPE ID() const { return m_ID; }
        ZetaInline D3D12_RESOURCE_DESC Desc() const
        {
            Assert(m_resource, "Texture hasn't been initialized.");
            return m_resource->GetDesc();
        }
        ZetaInline RESOURCE_HEAP_TYPE HeapType() const
        {
            Assert(m_resource, "Texture hasn't been initialized.");
            return m_heapType;
        }
        // Note: No GpuVa() method - from MS docs: "ID3D12Resource::GetGPUVirtualAddress() 
        // is only useful for buffer resources, it will return zero for all texture resources."

    private:
        ID3D12Resource* m_resource = nullptr;
        ID_TYPE m_ID = INVALID_ID;
        RESOURCE_HEAP_TYPE m_heapType;
    };

    struct ResourceHeap
    {
        ResourceHeap() = default;
        explicit ResourceHeap(ID3D12Heap* heap);
        ~ResourceHeap();
        ResourceHeap(ResourceHeap&&);
        ResourceHeap& operator=(ResourceHeap&&);

        void Reset();
        ZetaInline bool IsInitialized() const { return m_heap; }
        ZetaInline ID3D12Heap* Heap()
        {
            Assert(m_heap, "Heap hasn't been initialized.");
            return m_heap;
        }

    private:
        ID3D12Heap* m_heap = nullptr;
    };

    //
    // API
    //

    void Init();
    void BeginFrame();
    void SubmitResourceCopies();
    void Recycle();
    // Assumes GPU synchronization has been performed
    void Shutdown();

    UploadHeapBuffer GetUploadHeapBuffer(uint32_t sizeInBytes, uint32_t alignment = 4, 
        bool forceSeparate = false);
    void ReleaseUploadHeapBuffer(UploadHeapBuffer& buffer);
    void ReleaseUploadHeapArena(UploadHeapArena& arena);

    ReadbackHeapBuffer GetReadbackHeapBuffer(uint32_t sizeInBytes);
    void ReleaseReadbackHeapBuffer(ReadbackHeapBuffer& buffer);

    Buffer GetDefaultHeapBuffer(const char* name, uint32_t sizeInBytes,
        D3D12_RESOURCE_STATES initialState, bool allowUAV, bool initToZero = false);
    Buffer GetDefaultHeapBuffer(const char* name, uint32_t sizeInBytes,
        bool isRtAs, bool allowUAV, bool initToZero = false);
    Buffer GetPlacedHeapBuffer(const char* name, uint32_t sizeInBytes,
        ID3D12Heap* heap, uint64_t offsetInBytes, bool allowUAV, bool isRtAs);
    Buffer GetDefaultHeapBufferAndInit(const char* name,
        uint32_t sizeInBytes,
        bool allowUAV, 
        Util::MemoryRegion initData,
        bool forceSeparateUploadBuffer = false);    
    Buffer GetPlacedHeapBufferAndInit(const char* name,
        uint32_t sizeInBytes,
        ID3D12Heap* heap,
        uint64_t offsetInBytes,
        bool allowUAV,
        Util::MemoryRegion initData,
        bool forceSeparateUploadBuffer = false);
    void UploadToDefaultHeapBuffer(Buffer& buffer, uint32_t sizeInBytes, 
        Util::MemoryRegion sourceData, uint32_t destOffsetInBytes = 0);
    ResourceHeap GetResourceHeap(uint64_t sizeInBytes, 
        uint64_t alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
        bool createZeroed = false);
    void ReleaseDefaultHeapBuffer(Buffer& buffer);
    void ReleaseTexture(Texture& textue);
    void ReleaseResourceHeap(ResourceHeap& heap);

    Texture GetTexture2D(const char* name, uint64_t width, uint32_t height, DXGI_FORMAT format,
        D3D12_RESOURCE_STATES initialState, uint32_t flags = 0, uint16_t mipLevels = 1,
        D3D12_CLEAR_VALUE* clearVal = nullptr);
    Texture GetTexture2D(Texture::ID_TYPE id, uint64_t width, uint32_t height, DXGI_FORMAT format,
        D3D12_RESOURCE_STATES initialState, uint32_t flags = 0, uint16_t mipLevels = 1,
        D3D12_CLEAR_VALUE* clearVal = nullptr, const char* dbgName = nullptr);
    Texture GetPlacedTexture2D(const char* name, uint64_t width, uint32_t height, DXGI_FORMAT format,
        ID3D12Heap* heap, uint64_t offsetInBytes, D3D12_RESOURCE_STATES initialState, uint32_t flags = 0,
        uint16_t mipLevels = 1, D3D12_CLEAR_VALUE* clearVal = nullptr);
    Texture GetPlacedTexture2D(const char* name, uint64_t width, uint32_t height, DXGI_FORMAT format,
        ID3D12Heap* heap, uint64_t offsetInBytes, D3D12_BARRIER_LAYOUT initialLayout, uint32_t flags = 0,
        uint16_t mipLevels = 1, D3D12_CLEAR_VALUE* clearVal = nullptr);
    Texture GetTexture2D(const char* name, uint64_t width, uint32_t height, DXGI_FORMAT format,
        D3D12_BARRIER_LAYOUT initialLayout, uint32_t flags = 0, uint16_t mipLevels = 1,
        D3D12_CLEAR_VALUE* clearVal = nullptr);
    Texture GetTexture2D(Texture::ID_TYPE id, uint64_t width, uint32_t height, DXGI_FORMAT format,
        D3D12_BARRIER_LAYOUT initialLayout, uint32_t flags = 0, uint16_t mipLevels = 1,
        D3D12_CLEAR_VALUE* clearVal = nullptr, const char* dbgName = nullptr);
    Texture GetTexture3D(const char* name, uint64_t width, uint32_t height, uint16_t depth,
        DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState,
        uint32_t flags = 0, uint16_t mipLevels = 1);

    struct DDS_Data
    {
        // Up to 4k for 2D textures
        static constexpr int MAX_NUM_SUBRESOURCES = 13;

        D3D12_SUBRESOURCE_DATA subresources[MAX_NUM_SUBRESOURCES];
        Texture::ID_TYPE ID;
        uint32_t width;
        uint32_t height;
        uint32_t depth;
        uint32_t numSubresources;
        DXGI_FORMAT format;
        uint16_t mipCount;
    };

    Core::Direct3DUtil::LOAD_DDS_RESULT GetTexture2DFromDisk(const char* texPath,
        Texture::ID_TYPE ID, Texture& tex);
    Core::Direct3DUtil::LOAD_DDS_RESULT GetTexture2DFromDisk(const char* texPath,
        Texture::ID_TYPE ID, Texture& tex, UploadHeapArena& heapArena, Support::ArenaAllocator allocator);
    Core::Direct3DUtil::LOAD_DDS_RESULT GetDDSDataFromDisk(const char* texPath,
        DDS_Data& dds, UploadHeapArena& heapArena, Support::ArenaAllocator allocator);
    Core::Direct3DUtil::LOAD_DDS_RESULT GetTexture3DFromDisk(const char* texPath,
        Texture& tex);
    Texture GetTexture2DAndInit(const char* name, uint64_t width, uint32_t height, DXGI_FORMAT format,
        D3D12_RESOURCE_STATES initialState, uint8_t* pixels, uint32_t flags = 0);
    Texture GetPlacedTexture2DAndInit(Texture::ID_TYPE ID, const D3D12_RESOURCE_DESC1& desc,
        ID3D12Heap* heap, uint64_t offsetInBytes, UploadHeapArena& heapArena,
        Util::Span<D3D12_SUBRESOURCE_DATA> subresources, const char* dbgName = nullptr);
}