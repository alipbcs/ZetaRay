#include "../App/Timer.h"
#include "RendererCore.h"
#include "CommandList.h"
#include "../Support/Task.h"
#include "../App/Filesystem.h"
#include "../Utility/Utility.h"
#include <thread>
#include <algorithm>
#include <bit>
#include <xxHash/xxhash.h>

using namespace ZetaRay;
using namespace ZetaRay::App;
using namespace ZetaRay::Core;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::Core::Direct3DUtil;
using namespace ZetaRay::Core::GpuMemory;

namespace
{
    //--------------------------------------------------------------------------------------
    // ResourceUploadBatch
    //--------------------------------------------------------------------------------------

    struct ResourceUploadBatch
    {
        ResourceUploadBatch()
            : m_arena(4 * 1096),
            m_scratchResources(m_arena)
        {}
        ~ResourceUploadBatch() = default;

        ResourceUploadBatch(ResourceUploadBatch&& other) = delete;
        ResourceUploadBatch& operator=(ResourceUploadBatch&& other) = delete;

        void Begin()
        {
            Assert(!m_inBeginEndBlock, "Can't Begin: already in a Begin-End block.");
            m_inBeginEndBlock = true;
            m_hasWorkThisFrame = false;
        }

        // Works by:
        // 1. Allocates an intermediate upload heap buffer whose size is calculated by 
        //    GetCopyableFootprints()
        // 2. Maps the intermediate buffer
        // 3. Copies all subresources to upload heap buffer
        // 4. Records a CopyTextureRegion call to default heap texture for each subresource on 
        //    the command list
        void UploadTexture(UploadHeapArena& arena, ID3D12Resource* texture, 
            Span<D3D12_SUBRESOURCE_DATA> subResData,
            int firstSubresourceIndex = 0, 
            D3D12_RESOURCE_STATES postCopyState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
        {
            Assert(m_inBeginEndBlock, "Not in begin-end block.");
            Assert(texture, "Texture was NULL.");

            if (!m_directCmdList)
            {
                m_directCmdList = App::GetRenderer().GetGraphicsCmdList();
#ifndef NDEBUG
                m_directCmdList->SetName("ResourceUploadBatch");
#endif
            }

            constexpr int MAX_NUM_SUBRESOURCES = 13;
            Assert(MAX_NUM_SUBRESOURCES >= subResData.size(), 
                "MAX_NUM_SUBRESOURCES is too small.");

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT subresLayout[MAX_NUM_SUBRESOURCES];
            UINT subresNumRows[MAX_NUM_SUBRESOURCES];
            UINT64 subresRowSize[MAX_NUM_SUBRESOURCES];

            const auto destDesc = texture->GetDesc();
            Assert(destDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER, 
                "This function is for uploading textures.");

            // Required size of an intermediate buffer that is to be used to initialize 
            // this resource
            UINT64 totalSize;
            auto* device = App::GetRenderer().GetDevice();
            device->GetCopyableFootprints(&destDesc,   // resource description
                firstSubresourceIndex,                 // index of the first subresource
                (uint32_t)subResData.size(),           // number of subresources
                0,                                     // offset to the resource
                subresLayout,                          // description and placement of each subresource
                subresNumRows,                         // number of rows for each subresource
                subresRowSize,                         // unpadded size of a row of each subresource
                &totalSize);

            const auto uploadBuffer = arena.SubAllocate((uint32_t)totalSize);

            CopyTextureFromUploadBuffer(uploadBuffer.Res, uploadBuffer.Mapped, 
                uploadBuffer.Offset, texture, (uint32_t)subResData.size(), 
                firstSubresourceIndex, subResData, subresLayout, subresNumRows, 
                subresRowSize, postCopyState);

            m_hasWorkThisFrame = true;
        }

        void UploadTexture(ID3D12Resource* texture, Span<D3D12_SUBRESOURCE_DATA> subResData, 
            int firstSubresourceIndex = 0,
            D3D12_RESOURCE_STATES postCopyState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
        {
            Assert(m_inBeginEndBlock, "Not in begin-end block.");
            Assert(texture, "Texture was NULL.");

            if (!m_directCmdList)
            {
                m_directCmdList = App::GetRenderer().GetGraphicsCmdList();
#ifndef NDEBUG
                m_directCmdList->SetName("ResourceUploadBatch");
#endif
            }

            constexpr int MAX_NUM_SUBRESOURCES = 12;
            Assert(MAX_NUM_SUBRESOURCES >= subResData.size(), 
                "MAX_NUM_SUBRESOURCES is too small.");

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT subresLayout[MAX_NUM_SUBRESOURCES];
            UINT subresNumRows[MAX_NUM_SUBRESOURCES];
            UINT64 subresRowSize[MAX_NUM_SUBRESOURCES];

            const auto destDesc = texture->GetDesc();
            Assert(destDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER, 
                "This function is for uploading textures.");

            auto* device = App::GetRenderer().GetDevice();
            UINT64 totalSize;
            device->GetCopyableFootprints(&destDesc,    // resource description
                firstSubresourceIndex,                  // index of the first subresource
                (uint32_t)subResData.size(),            // number of subresources
                0,                                      // offset to the resource
                subresLayout,                           // description and placement of each subresource
                subresNumRows,                          // number of rows for each subresource
                subresRowSize,                          // unpadded size of a row of each subresource
                &totalSize);

            UploadHeapBuffer uploadBuffer = GpuMemory::GetUploadHeapBuffer((uint32_t)totalSize);

            CopyTextureFromUploadBuffer(uploadBuffer.Resource(), 
                uploadBuffer.MappedMemory(), uploadBuffer.Offset(), texture, 
                (uint32_t)subResData.size(), firstSubresourceIndex, subResData, 
                subresLayout, subresNumRows, subresRowSize, postCopyState);

            // Preserve the upload buffer for as long as GPU is using it 
            m_scratchResources.push_back(ZetaMove(uploadBuffer));

            m_hasWorkThisFrame = true;
        }

        void UploadBuffer(ID3D12Resource* buffer, void* data, uint32_t sizeInBytes, 
            uint32_t destOffset = 0, bool forceSeparate = false)
        {
            Assert(m_inBeginEndBlock, "Not in begin-end block.");
            Assert(buffer, "Buffer was NULL.");

            if (!m_directCmdList)
            {
                m_directCmdList = App::GetRenderer().GetGraphicsCmdList();
#ifndef NDEBUG
                m_directCmdList->SetName("ResourceUploadBatch");
#endif
            }

            // Note: GetCopyableFootprints() returns the padded size for a standalone 
            // resource, here we might be suballocating from a larger buffer.
            UploadHeapBuffer uploadBuffer = GpuMemory::GetUploadHeapBuffer(sizeInBytes, 4, forceSeparate);
            uploadBuffer.Copy(0, sizeInBytes, data);

            // Note: can't use CopyResource() since the UploadHeap might not have the 
            // exact same size as the destination resource due to subresource allocations.

            m_directCmdList->CopyBufferRegion(buffer,
                destOffset,
                uploadBuffer.Resource(),
                uploadBuffer.Offset(),
                sizeInBytes);

            // Preserve the upload buffer for as long as GPU is using it 
            m_scratchResources.push_back(ZetaMove(uploadBuffer));

            m_hasWorkThisFrame = true;
        }

        void UploadTexture(ID3D12Resource* dstResource, uint8_t* pixels, 
            D3D12_RESOURCE_STATES postCopyState)
        {
            Assert(m_inBeginEndBlock, "Not in begin-end block.");

            if (!m_directCmdList)
            {
                m_directCmdList = App::GetRenderer().GetGraphicsCmdList();
#ifndef NDEBUG
                m_directCmdList->SetName("ResourceUploadBatch");
#endif
            }

            const auto desc = dstResource->GetDesc();
            Assert(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D, 
                "This function is for uploading 2D textures.");

            const uint32_t numBytesPerPixel = (uint32_t)Direct3DUtil::BitsPerPixel(desc.Format) >> 3;
            const UINT rowSizeInBytes = (uint32_t)desc.Width * numBytesPerPixel;
            const UINT rowPitch = Math::AlignUp(rowSizeInBytes, 
                (uint32_t)D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            const UINT uploadSize = desc.Height * rowPitch;

            UploadHeapBuffer uploadBuffer = GpuMemory::GetUploadHeapBuffer(uploadSize);

            for (int y = 0; y < (int)desc.Height; y++)
                uploadBuffer.Copy(y * rowPitch, rowSizeInBytes, pixels + y * rowSizeInBytes);

            D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
            srcLocation.pResource = uploadBuffer.Resource();
            srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLocation.PlacedFootprint.Offset = uploadBuffer.Offset();
            srcLocation.PlacedFootprint.Footprint.Format = desc.Format;
            srcLocation.PlacedFootprint.Footprint.Width = (UINT)desc.Width;
            srcLocation.PlacedFootprint.Footprint.Height = (UINT)desc.Height;
            srcLocation.PlacedFootprint.Footprint.Depth = 1;
            srcLocation.PlacedFootprint.Footprint.RowPitch = rowPitch;

            D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
            dstLocation.pResource = dstResource;
            dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLocation.SubresourceIndex = 0;

            m_directCmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, 
                nullptr);

            if (postCopyState != D3D12_RESOURCE_STATE_COPY_DEST)
            {
                m_directCmdList->ResourceBarrier(dstResource, D3D12_RESOURCE_STATE_COPY_DEST, 
                    postCopyState);
            }

            // Preserve the upload buffer for as long as GPU is using it 
            m_scratchResources.push_back(ZetaMove(uploadBuffer));

            m_hasWorkThisFrame = true;
        }

        // Submits all the uploads
        // No more uploads can happen after this call until Begin is called again.
        uint64_t End()
        {
            Assert(m_inBeginEndBlock, "Not in begin-end block.");
            uint64_t ret = 0;

            if (m_hasWorkThisFrame)
            {
                ret = App::GetRenderer().ExecuteCmdList(m_directCmdList);
                m_directCmdList = nullptr;
            }

            m_inBeginEndBlock = false;

            return ret;
        }

        void Recycle()
        {
            //m_scratchResources.free_memory();
            //m_arena.Reset();
            m_scratchResources.clear();
        }

    private:
        void CopyTextureFromUploadBuffer(ID3D12Resource* uploadBuffer, void* mapped, 
            uint32_t uploadBuffOffsetInBytes, ID3D12Resource* texture, int numSubresources, 
            int firstSubresourceIndex, Span<D3D12_SUBRESOURCE_DATA> subResData, 
            Span<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> subresLayout, Span<UINT> subresNumRows, 
            Span<UINT64> subresRowSize, D3D12_RESOURCE_STATES postCopyState)
        {
            // Notes:
            // 
            // 1.
            //   subresRowSize[i]: #bytes to copy for each row (unpadded)
            //   subresLayout[i].Footprint.RowPitch: padded size in bytes of each row
            //
            // 2. As buffers have a 64 KB alignment, GetCopyableFootprints() returns the padded 
            //    size. Using subresRowSize[0] as the copy size could lead to access violations 
            //    since size of the data pointed to by subresData is probably smaller.

            // For each subresource in destination
            for (int i = 0; i < numSubresources; i++)
            {
                size_t destOffset = subresLayout[i].Offset;
                const size_t destSubresSlicePitch = subresLayout[i].Footprint.RowPitch * subresNumRows[i];

                // For each slice of that subresource
                for (int slice = 0; slice < (int)subresLayout[i].Footprint.Depth; slice++)
                {
                    const uintptr_t sourceSubres = reinterpret_cast<uintptr_t>(
                        subResData[i].pData) + subResData[i].SlicePitch * slice;

                    // For each row of that subresource slice
                    for (int row = 0; row < (int)subresNumRows[i]; row++)
                    {
                        const uintptr_t dest = reinterpret_cast<uintptr_t>(mapped) +
                            uploadBuffOffsetInBytes +
                            destOffset +
                            row * subresLayout[i].Footprint.RowPitch;

                        const uintptr_t src = sourceSubres + subResData[i].RowPitch * row;

                        memcpy(reinterpret_cast<void*>(dest),
                            reinterpret_cast<void*>(src),
                            subresRowSize[i]);
                    }

                    destOffset += destSubresSlicePitch;
                }
            }

            for (int i = 0; i < numSubresources; i++)
            {
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.pResource = texture;
                dst.SubresourceIndex = firstSubresourceIndex + i;

                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = uploadBuffer;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = subresLayout[i];
                src.PlacedFootprint.Offset += uploadBuffOffsetInBytes;

                m_directCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }

            if (postCopyState != D3D12_RESOURCE_STATE_COPY_DEST)
            {
                m_directCmdList->ResourceBarrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, 
                    postCopyState);
            }
        }

        // Scratch resources need to stay alive while GPU is using them
        MemoryArena m_arena;
        SmallVector<UploadHeapBuffer, Support::ArenaAllocator> m_scratchResources;

        GraphicsCmdList* m_directCmdList = nullptr;
        bool m_inBeginEndBlock = false;
        bool m_hasWorkThisFrame = false;
    };

    //--------------------------------------------------------------------------------------
    // GpuMemoryImplData
    //--------------------------------------------------------------------------------------

    struct GpuMemoryImplData
    {
        // Requests are attempted to be suballocated from a shared upload heap of the following 
        // size. If unsuccessful, a new upload heap is created.
        static constexpr uint32_t UPLOAD_HEAP_SIZE = uint32_t(9 * 1024 * 1024);
        static constexpr uint32_t MAX_NUM_UPLOAD_HEAP_ALLOCS = 128;

        struct PendingResource
        {
            ZetaInline bool IsUploadHeapBuffer() const
            {
                return !Allocation.IsEmpty();
            }

            ID3D12Pageable* Res;
            uint64_t ReleaseFence;
            void* MappedMemory = nullptr;
            OffsetAllocator::Allocation Allocation = OffsetAllocator::Allocation::Empty();
        };

        OffsetAllocator m_uploadHeapAllocator;
        ComPtr<ID3D12Resource> m_uploadHeap;
        void* m_uploadHeapMapped;
        SRWLOCK m_uploadHeapLock = SRWLOCK_INIT;

        Util::SmallVector<PendingResource> m_toRelease;
        SRWLOCK m_pendingResourceLock = SRWLOCK_INIT;

        ComPtr<ID3D12Fence> m_fenceDirect;
        ComPtr<ID3D12Fence> m_fenceCompute;
        uint64_t m_nextFenceVal = 1;    // no need to be atomic

        alignas(32) ZETA_THREAD_ID_TYPE m_threadIDs [ZETA_MAX_NUM_THREADS];
        ResourceUploadBatch m_uploaders[ZETA_MAX_NUM_THREADS];
    };

    ZetaInline int GetThreadIndex(Span<uint32_t> threadIDs)
    {
        const uint32_t tid = std::bit_cast<ZETA_THREAD_ID_TYPE, std::thread::id>(
            std::this_thread::get_id());

        int ret = -1;
        __m256i vKey = _mm256_set1_epi32(tid);

        for (int i = 0; i < ZETA_MAX_NUM_THREADS; i += 8)
        {
            __m256i vIDs = _mm256_load_si256((__m256i*)(threadIDs.data() + i));
            __m256i vRes = _mm256_cmpeq_epi32(vIDs, vKey);
            int mask = _mm256_movemask_ps(_mm256_castsi256_ps(vRes));

            if (mask != 0)
            {
                ret = i + _tzcnt_u32(mask);
                break;
            }
        }

        Assert(ret != -1, "Thread index was not found.");

        return ret;
    }

    GpuMemoryImplData* g_data = nullptr;
}

//--------------------------------------------------------------------------------------
// UploadHeapBuffer
//--------------------------------------------------------------------------------------

UploadHeapBuffer::UploadHeapBuffer(ID3D12Resource* r, void* mapped, 
    const Support::OffsetAllocator::Allocation& alloc)
    : m_resource(r),
    m_mappedMemory(mapped),
    m_allocation(alloc)
{}

UploadHeapBuffer::~UploadHeapBuffer()
{
    Reset();
}

UploadHeapBuffer::UploadHeapBuffer(UploadHeapBuffer&& other)
    : m_resource(other.m_resource),
    m_mappedMemory(other.m_mappedMemory),
    m_allocation(other.m_allocation)
{
    other.m_resource = nullptr;
    other.m_mappedMemory = nullptr;
    other.m_allocation = OffsetAllocator::Allocation::Empty();
}

UploadHeapBuffer& UploadHeapBuffer::operator=(UploadHeapBuffer&& other)
{
    if (this == &other)
        return *this;

    Reset();

    m_resource = other.m_resource;
    m_mappedMemory = other.m_mappedMemory;
    m_allocation = other.m_allocation;

    other.m_resource = nullptr;
    other.m_mappedMemory = nullptr;
    other.m_allocation = OffsetAllocator::Allocation::Empty();

    return *this;
}

void UploadHeapBuffer::Reset()
{
    if (m_resource)
        GpuMemory::ReleaseUploadHeapBuffer(*this);

    m_resource = nullptr;
    m_mappedMemory = nullptr;
    m_allocation = OffsetAllocator::Allocation::Empty();
}

void UploadHeapBuffer::Copy(uint32_t offset, uint32_t numBytesToCopy, void* data)
{
    Assert(offset + numBytesToCopy <= m_allocation.Size, 
        "Copy destination region was out-of-bound.");
    memcpy(reinterpret_cast<uint8_t*>(m_mappedMemory) + m_allocation.Offset + offset, 
        data, numBytesToCopy);
}

//--------------------------------------------------------------------------------------
// UploadHeapArena
//--------------------------------------------------------------------------------------

UploadHeapArena::UploadHeapArena(uint32_t sizeInBytes)
    : m_size(Math::AlignUp(sizeInBytes, (uint32_t)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT))
{}

UploadHeapArena::~UploadHeapArena()
{
    if (!m_blocks.empty())
        GpuMemory::ReleaseUploadHeapArena(*this);
}

UploadHeapArena::UploadHeapArena(UploadHeapArena&& other)
    : m_size(other.m_size)
{
    m_blocks.swap(other.m_blocks);
    other.m_size = 0;
}

UploadHeapArena::Allocation UploadHeapArena::SubAllocate(uint32_t size, uint32_t alignment)
{
    Check(size <= m_size, "allocations larger than %u MB are not supported.", m_size / (1024 * 1024));

    for (auto& block : m_blocks)
    {
        const uint32_t newOffset = (uint32_t)Math::AlignUp(block.Offset, alignment);

        if (newOffset + size <= m_size)
        {
            block.Offset = newOffset + size;

            return Allocation{ .Res = block.Res,
                .Mapped = block.Mapped,
                .Offset = newOffset };
        }
    }

    D3D12_HEAP_PROPERTIES uploadHeap = Direct3DUtil::UploadHeapProp();
    D3D12_RESOURCE_DESC bufferDesc = Direct3DUtil::BufferResourceDesc(m_size);

    auto* device = App::GetRenderer().GetDevice();

    ID3D12Resource* res;
    CheckHR(device->CreateCommittedResource(&uploadHeap,
        D3D12_HEAP_FLAG_CREATE_NOT_ZEROED,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&res)));

    SET_D3D_OBJ_NAME(res, "UploadHeapArena");

    // From MS docs:
    // "Resources on D3D12_HEAP_TYPE_UPLOAD heaps can be persistently mapped, meaning Map 
    // can be called once, immediately after resource creation. Unmap never needs to be called, 
    // but the address returned from Map must no longer be used after the last reference to the 
    // resource is released. When using persistent map, the application must ensure the CPU finishes 
    // writing data into memory before the GPU executes a command list that reads the memory."
    void* mapped;
    CheckHR(res->Map(0, nullptr, &mapped));

    Block newBlock{ .Res = res,
        .Offset = size,
        .Mapped = mapped };

    m_blocks.push_front(ZetaMove(newBlock));

    return Allocation{ .Res = res,
        .Mapped = mapped };
}

//--------------------------------------------------------------------------------------
// Buffer
//--------------------------------------------------------------------------------------

Buffer::Buffer(const char* p, ID3D12Resource* r, RESOURCE_HEAP_TYPE heapType)
    : m_resource(r),
    m_ID(XXH3_64_To_32(XXH3_64bits(p, strlen(p)))),
    m_heapType(heapType)
{
    SET_D3D_OBJ_NAME(m_resource, p);
}

Buffer::~Buffer()
{
    Reset();
}

Buffer::Buffer(Buffer&& other)
    : m_resource(other.m_resource),
    m_ID(other.m_ID),
    m_heapType(other.m_heapType)
{
    other.m_resource = nullptr;
    other.m_ID = INVALID_ID;
}

Buffer& Buffer::operator=(Buffer&& other)
{
    if (this == &other)
        return *this;

    Reset();

    m_resource = other.m_resource;
    m_ID = other.m_ID;

    other.m_resource = nullptr;
    other.m_ID = INVALID_ID;
    m_heapType = other.m_heapType;

    return *this;
}

void Buffer::Reset(bool waitForGpu)
{
    if (m_resource)
    {
        if (waitForGpu && (m_heapType == RESOURCE_HEAP_TYPE::COMMITTED))
            GpuMemory::ReleaseDefaultHeapBuffer(*this);
        else
        {
            auto newRefCount = m_resource->Release();
            Assert(newRefCount == 0, "unexpected ref count -- expected 0, actual %u.", 
                newRefCount);
        }
    }

    m_ID = INVALID_ID;
    m_resource = nullptr;
}

//--------------------------------------------------------------------------------------
// ReadbackHeapBuffer
//--------------------------------------------------------------------------------------

ReadbackHeapBuffer::ReadbackHeapBuffer(ID3D12Resource* r)
    : m_resource(r)
{}

ReadbackHeapBuffer::~ReadbackHeapBuffer()
{
    Reset();
}

ReadbackHeapBuffer::ReadbackHeapBuffer(ReadbackHeapBuffer&& other)
    : m_resource(std::exchange(other.m_resource, nullptr)),
    m_mappedMemory(std::exchange(other.m_mappedMemory, nullptr))
{}

ReadbackHeapBuffer& ReadbackHeapBuffer::operator=(ReadbackHeapBuffer&& other)
{
    if (this == &other)
        return *this;

    Reset();

    m_resource = other.m_resource;
    m_mappedMemory = other.m_mappedMemory;
    other.m_mappedMemory = nullptr;
    other.m_resource = nullptr;

    return *this;
}

void ReadbackHeapBuffer::Reset(bool waitForGpu)
{
    if (m_resource)
    {
        if (waitForGpu)
            GpuMemory::ReleaseReadbackHeapBuffer(*this);
        else
        {
            auto newRefCount = m_resource->Release();
            Assert(newRefCount == 0, "unexpected ref count -- expected 0, actual %u.", 
                newRefCount);
        }
    }

    m_resource = nullptr;
    m_mappedMemory = nullptr;
}

void ReadbackHeapBuffer::Map()
{
    if (m_mappedMemory)
        return;

    // Buffers have only one subresource
    CheckHR(m_resource->Map(0, nullptr, &m_mappedMemory));
}

void ReadbackHeapBuffer::Unmap()
{
    if (!m_mappedMemory)
        return;

    m_resource->Unmap(0, nullptr);
    m_mappedMemory = nullptr;
}

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------

Texture::Texture(const char* name, ID3D12Resource* res, RESOURCE_HEAP_TYPE heapType)
    : m_resource(res),
    m_ID(XXH3_64_To_32(XXH3_64bits(name, strlen(name)))),
    m_heapType(heapType)
{
    SET_D3D_OBJ_NAME(m_resource, name);
}

Texture::Texture(ID_TYPE id, ID3D12Resource* res, RESOURCE_HEAP_TYPE heapType)
    : m_resource(res),
    m_ID(id),
    m_heapType(heapType)
{
    Assert(id != INVALID_ID, "Invalid ID.");
}

Texture::~Texture()
{
    Reset();
}

Texture::Texture(Texture&& other)
    : m_resource(other.m_resource),
    m_ID(other.m_ID),
    m_heapType(other.m_heapType)
{
    other.m_resource = nullptr;
    other.m_ID = INVALID_ID;
}

Texture& Texture::operator=(Texture&& other)
{
    if (this == &other)
        return *this;

    Reset();

    m_resource = other.m_resource;
    m_ID = other.m_ID;
    m_heapType = other.m_heapType;

    other.m_resource = nullptr;
    other.m_ID = INVALID_ID;

    return *this;
}

void Texture::Reset(bool waitForGpu, bool checkRefCount)
{
    if (m_resource)
    {
        if (waitForGpu && (m_heapType == RESOURCE_HEAP_TYPE::COMMITTED))
            GpuMemory::ReleaseTexture(*this);
        else
        {
            auto newRefCount = m_resource->Release();
            Assert(!checkRefCount || newRefCount == 0, "unexpected ref count -- expected 0, actual %u.", 
                newRefCount);
        }
    }

    m_resource = nullptr;
    m_ID = INVALID_ID;
}

//--------------------------------------------------------------------------------------
// ResourceHeap
//--------------------------------------------------------------------------------------

ResourceHeap::ResourceHeap(ID3D12Heap* heap)
    : m_heap(heap)
{}

ResourceHeap::~ResourceHeap()
{
    Reset();
}

ResourceHeap::ResourceHeap(ResourceHeap&& other)
    : m_heap(other.m_heap)
{
    other.m_heap = nullptr;
}

ResourceHeap& ResourceHeap::operator=(ResourceHeap&& other)
{
    if (this == &other)
        return *this;

    Reset();

    m_heap = other.m_heap;
    other.m_heap = nullptr;

    return *this;
}

void ResourceHeap::Reset()
{
    if (m_heap)
        GpuMemory::ReleaseResourceHeap(*this);

    m_heap = nullptr;
}

//--------------------------------------------------------------------------------------
// GpuMemory
//--------------------------------------------------------------------------------------

void GpuMemory::Init()
{
    Assert(!g_data, "attempting to double initialize.");
    g_data = new GpuMemoryImplData;

    g_data->m_uploadHeapAllocator.Init(GpuMemoryImplData::UPLOAD_HEAP_SIZE, 
        GpuMemoryImplData::MAX_NUM_UPLOAD_HEAP_ALLOCS);

    D3D12_HEAP_PROPERTIES uploadHeap = Direct3DUtil::UploadHeapProp();
    D3D12_RESOURCE_DESC bufferDesc = Direct3DUtil::BufferResourceDesc(
        GpuMemoryImplData::UPLOAD_HEAP_SIZE);

    auto* device = App::GetRenderer().GetDevice();
    CheckHR(device->CreateCommittedResource(&uploadHeap,
        D3D12_HEAP_FLAG_CREATE_NOT_ZEROED,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(g_data->m_uploadHeap.GetAddressOf())));

    SET_D3D_OBJ_NAME(g_data->m_uploadHeap, "UploadHeap");
    CheckHR(g_data->m_uploadHeap->Map(0, nullptr, &g_data->m_uploadHeapMapped));

    CheckHR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(
        g_data->m_fenceDirect.GetAddressOf())));
    CheckHR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(
        g_data->m_fenceCompute.GetAddressOf())));

    auto workerThreadIDs = App::GetWorkerThreadIDs();

    for (int i = 0; i < workerThreadIDs.size(); i++)
        g_data->m_threadIDs[i] = workerThreadIDs[i];
}

void GpuMemory::BeginFrame()
{
    const int numThreads = App::GetNumWorkerThreads();

    for (int i = 0; i < numThreads; i++)
        g_data->m_uploaders[i].Begin();
}

void GpuMemory::SubmitResourceCopies()
{
    const int numThreads = App::GetNumWorkerThreads();
    uint64_t maxFenceVal = 0;

    for (int i = 0; i < numThreads; i++)
    {
        auto f = g_data->m_uploaders[i].End();
        maxFenceVal = Math::Max(maxFenceVal, f);
    }

    if (maxFenceVal != 0)
    {
        // Compute queue needs to wait for direct queue
        App::GetRenderer().WaitForDirectQueueOnComputeQueue(maxFenceVal);
    }
}

void GpuMemory::Recycle()
{
    auto& renderer = App::GetRenderer();
    renderer.SignalDirectQueue(g_data->m_fenceDirect.Get(), g_data->m_nextFenceVal);
    renderer.SignalComputeQueue(g_data->m_fenceCompute.Get(), g_data->m_nextFenceVal);

    for (int i = 0; i < App::GetNumWorkerThreads(); i++)
        g_data->m_uploaders[i].Recycle();

    const uint64_t completedFenceValDir = g_data->m_fenceDirect->GetCompletedValue();
    const uint64_t completedFenceValCompute = g_data->m_fenceCompute->GetCompletedValue();

    SmallVector<GpuMemoryImplData::PendingResource> toDelete;

    {
        const auto* first = std::partition(g_data->m_toRelease.begin(), g_data->m_toRelease.end(),
            [completedFenceValDir, completedFenceValCompute](const GpuMemoryImplData::PendingResource& res)
            {
                return res.ReleaseFence > completedFenceValDir || 
                    res.ReleaseFence > completedFenceValCompute;
            });

        const auto numToDelete = g_data->m_toRelease.end() - first;
        toDelete.append_range(first, g_data->m_toRelease.end(), true);
        g_data->m_toRelease.pop_back(numToDelete);
    }

    // No need to synchronize -- this happens at the end of each frame and resource deletion
    // won't happen until next frame's update, which happens strictly after recycling has finished

    {
        // Release the upload heap buffers here rather than on a background thread with the others
        // to avoid synchronization
        const auto* first = std::partition(toDelete.begin(), toDelete.end(),
            [](const GpuMemoryImplData::PendingResource& res)
            {
                return !res.IsUploadHeapBuffer();
            });

        const auto numUploadBuffers = toDelete.end() - first;

        for (auto it = first; it < toDelete.end(); it++)
        {
            Assert(!it->Allocation.IsEmpty(), "unexpected result.");
            Assert(it->MappedMemory == g_data->m_uploadHeapMapped, "unexpected result.");
            g_data->m_uploadHeapAllocator.Free(it->Allocation);
        }

        toDelete.pop_back(numUploadBuffers);
    }

    if (!toDelete.empty())
    {
        Task t("Releasing resources", TASK_PRIORITY::BACKGROUND, [ToDelete = ZetaMove(toDelete)]()
            {
                for (auto& r : ToDelete)
                {
                    Assert(r.Res, "unexpected - attempting to release null resource.");
                    Assert(!r.IsUploadHeapBuffer(), 
                        "unexpected - small upload heap buffers shouldn't be released on a background thread.");

                    if (r.MappedMemory)
                        static_cast<ID3D12Resource*>(r.Res)->Unmap(0, nullptr);

                    auto newRefCount = r.Res->Release();
                    Assert(newRefCount == 0, "unexpected ref count -- expected 0, actual %u.", 
                        newRefCount);
                }
            });

        App::SubmitBackground(ZetaMove(t));
    }

    g_data->m_nextFenceVal++;
}

void GpuMemory::Shutdown()
{
    Assert(g_data, "g_data shouldn't be null.");
    delete g_data;
    g_data = nullptr;
}

UploadHeapBuffer GpuMemory::GetUploadHeapBuffer(uint32_t sizeInBytes, uint32_t alignment, 
    bool forceSeparate)
{
    if (!forceSeparate && sizeInBytes <= GpuMemoryImplData::UPLOAD_HEAP_SIZE)
    {
        AcquireSRWLockExclusive(&g_data->m_uploadHeapLock);
        const auto alloc = g_data->m_uploadHeapAllocator.Allocate(sizeInBytes, alignment);
        ReleaseSRWLockExclusive(&g_data->m_uploadHeapLock);

        if (alloc.IsEmpty())
        {
            StackStr(msg, n, 
                "Failed to allocate %u MB from the shared upload heap - creating a separate allocation...",
                sizeInBytes / (1024 * 1024));
            App::Log(msg, LogMessage::MsgType::WARNING);

            return GetUploadHeapBuffer(sizeInBytes, alignment, true);
        }

        return UploadHeapBuffer(g_data->m_uploadHeap.Get(),
            g_data->m_uploadHeapMapped,
            alloc);
    }
    else
    {
        D3D12_HEAP_PROPERTIES uploadHeap = Direct3DUtil::UploadHeapProp();
        const uint32_t alignedSize = Math::AlignUp(sizeInBytes, 
            (uint32_t)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
        D3D12_RESOURCE_DESC bufferDesc = Direct3DUtil::BufferResourceDesc(alignedSize);

        auto* device = App::GetRenderer().GetDevice();
        ID3D12Resource* buffer;
        CheckHR(device->CreateCommittedResource(&uploadHeap,
            D3D12_HEAP_FLAG_CREATE_NOT_ZEROED,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&buffer)));

        SET_D3D_OBJ_NAME(buffer, "UploadHeap");

        void* mapped;
        CheckHR(buffer->Map(0, nullptr, &mapped));

        OffsetAllocator::Allocation alloc = OffsetAllocator::Allocation::Empty();
        alloc.Size = alignedSize;

        return UploadHeapBuffer(buffer, mapped, alloc);
    }
}

void GpuMemory::ReleaseUploadHeapBuffer(UploadHeapBuffer& buffer)
{
    Assert(g_data, "Releasing GPU resources when GPU memory system has shut down.");

    AcquireSRWLockExclusive(&g_data->m_pendingResourceLock);

    g_data->m_toRelease.emplace_back(
        GpuMemoryImplData::PendingResource{ .Res = buffer.Resource(),
            .ReleaseFence = g_data->m_nextFenceVal,
            .MappedMemory = buffer.MappedMemory(),
            .Allocation = buffer.Allocation() });

    ReleaseSRWLockExclusive(&g_data->m_pendingResourceLock);
}

void GpuMemory::ReleaseUploadHeapArena(UploadHeapArena& arena)
{
    Assert(g_data, "Releasing GPU resources when GPU memory system has shut down.");

    auto blocks = arena.Blocks();

    if (blocks.empty())
        return;

    AcquireSRWLockExclusive(&g_data->m_pendingResourceLock);

    for (auto& block : blocks)
    {
        Assert(block.Mapped, "Mapped memory can't be NULL.");

        g_data->m_toRelease.emplace_back(
            GpuMemoryImplData::PendingResource{ .Res = block.Res,
                .ReleaseFence = g_data->m_nextFenceVal,
                .MappedMemory = block.Mapped });
    }

    ReleaseSRWLockExclusive(&g_data->m_pendingResourceLock);
}

ReadbackHeapBuffer GpuMemory::GetReadbackHeapBuffer(uint32_t sizeInBytes)
{
    auto* device = App::GetRenderer().GetDevice();

    D3D12_HEAP_PROPERTIES readbackHeap = Direct3DUtil::ReadbackHeapProp();
    D3D12_RESOURCE_DESC desc = Direct3DUtil::BufferResourceDesc(sizeInBytes);

    ID3D12Resource* buffer;

    CheckHR(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer)));

#ifndef NDEBUG
    buffer->SetName(L"Readback");
#endif

    return ReadbackHeapBuffer(buffer);
}

void GpuMemory::ReleaseReadbackHeapBuffer(ReadbackHeapBuffer& buffer)
{
    Assert(g_data, "Releasing GPU resources when GPU memory system has shut down.");
    Assert(buffer.IsInitialized() || !buffer.IsMapped(), 
        "Non-null mapped memory for null resource.");

    AcquireSRWLockExclusive(&g_data->m_pendingResourceLock);

    g_data->m_toRelease.emplace_back(
        GpuMemoryImplData::PendingResource{ .Res = buffer.Resource(),
            .ReleaseFence = g_data->m_nextFenceVal,
            .MappedMemory = buffer.MappedMemory() });

    ReleaseSRWLockExclusive(&g_data->m_pendingResourceLock);
}

Buffer GpuMemory::GetDefaultHeapBuffer(const char* name, uint32_t sizeInBytes,
    D3D12_RESOURCE_STATES initState, bool allowUAV, bool initToZero)
{
    const D3D12_RESOURCE_FLAGS f = allowUAV ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : 
        D3D12_RESOURCE_FLAG_NONE;

    const D3D12_HEAP_PROPERTIES heapDesc = Direct3DUtil::DefaultHeapProp();
    const D3D12_RESOURCE_DESC bufferDesc = Direct3DUtil::BufferResourceDesc(sizeInBytes, f);

    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
    if (!initToZero)
        heapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

    auto* device = App::GetRenderer().GetDevice();
    ID3D12Resource* r;
    CheckHR(device->CreateCommittedResource(&heapDesc,
        heapFlags,
        &bufferDesc,
        initState,
        nullptr,
        IID_PPV_ARGS(&r)));

    return Buffer(name, r, RESOURCE_HEAP_TYPE::COMMITTED);
}

Buffer GpuMemory::GetDefaultHeapBuffer(const char* name, uint32_t sizeInBytes,
    bool isRtAs, bool allowUAV, bool initToZero)
{
    D3D12_RESOURCE_FLAGS f = allowUAV ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : 
        D3D12_RESOURCE_FLAG_NONE;
    if (isRtAs)
        f |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;

    const D3D12_HEAP_PROPERTIES heapDesc = Direct3DUtil::DefaultHeapProp();
    const D3D12_RESOURCE_DESC1 bufferDesc = Direct3DUtil::BufferResourceDesc1(sizeInBytes, f);

    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
    if (!initToZero)
        heapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

    auto* device = App::GetRenderer().GetDevice();
    ID3D12Resource* r;
    CheckHR(device->CreateCommittedResource3(&heapDesc,
        heapFlags,
        &bufferDesc,
        D3D12_BARRIER_LAYOUT_UNDEFINED,
        nullptr,
        nullptr,
        0,
        nullptr,
        IID_PPV_ARGS(&r)));

    return Buffer(name, r, RESOURCE_HEAP_TYPE::COMMITTED);
}

Buffer GpuMemory::GetPlacedHeapBuffer(const char* name, uint32_t sizeInBytes, ID3D12Heap* heap,
    uint64_t offsetInBytes, bool allowUAV, bool isRtAs)
{
    D3D12_RESOURCE_FLAGS f = allowUAV ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : 
        D3D12_RESOURCE_FLAG_NONE;
    if (isRtAs)
        f |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
    const D3D12_RESOURCE_DESC1 bufferDesc = Direct3DUtil::BufferResourceDesc1(sizeInBytes, f);

    auto* device = App::GetRenderer().GetDevice();
    ID3D12Resource* r;
    CheckHR(device->CreatePlacedResource2(heap,
        offsetInBytes,
        &bufferDesc,
        D3D12_BARRIER_LAYOUT_UNDEFINED,
        nullptr,
        0,
        nullptr,
        IID_PPV_ARGS(&r)));

    return Buffer(name, r, RESOURCE_HEAP_TYPE::PLACED);
}

Buffer GpuMemory::GetDefaultHeapBufferAndInit(const char* name, uint32_t sizeInBytes, 
    bool allowUAV, MemoryRegion initData, bool forceSeparateUploadBuffer)
{
    Assert(initData.SizeInBytes <= sizeInBytes, "Size of initialization data exceeded resource size.");

    auto buffer = GpuMemory::GetDefaultHeapBuffer(name,
        sizeInBytes,
        D3D12_RESOURCE_STATE_COMMON,
        allowUAV,
        false);

    const int idx = GetThreadIndex(g_data->m_threadIDs);
    g_data->m_uploaders[idx].UploadBuffer(buffer.Resource(), initData.Data, 
        (uint32)initData.SizeInBytes, 0, forceSeparateUploadBuffer);

    return buffer;
}

Buffer GpuMemory::GetPlacedHeapBufferAndInit(const char* name, uint32_t sizeInBytes, 
    ID3D12Heap* heap, uint64_t offsetInBytes, bool allowUAV, MemoryRegion initData, 
    bool forceSeparateUploadBuffer)
{
    Assert(initData.SizeInBytes <= sizeInBytes, "Size of initialization data exceeded resource size.");

    auto buffer = GpuMemory::GetPlacedHeapBuffer(name,
        sizeInBytes,
        heap,
        offsetInBytes,
        allowUAV,
        false);

    const int idx = GetThreadIndex(g_data->m_threadIDs);
    g_data->m_uploaders[idx].UploadBuffer(buffer.Resource(), initData.Data,
        (uint32)initData.SizeInBytes, 0, forceSeparateUploadBuffer);

    return buffer;
}

void GpuMemory::UploadToDefaultHeapBuffer(Buffer& buffer, uint32_t sizeInBytes, 
    MemoryRegion sourceData, uint32_t destOffsetInBytes)
{
    Assert(sourceData.SizeInBytes >= sizeInBytes, "Out-of-bound memory access of source data.");

    const int idx = GetThreadIndex(g_data->m_threadIDs);
    g_data->m_uploaders[idx].UploadBuffer(buffer.Resource(), sourceData.Data, sizeInBytes,
        destOffsetInBytes);
}

ResourceHeap GpuMemory::GetResourceHeap(uint64_t sizeInBytes, uint64_t alignment, 
    bool createZeroed)
{
    D3D12_HEAP_DESC heapDesc;
    heapDesc.SizeInBytes = Math::AlignUp(sizeInBytes, alignment);
    heapDesc.Alignment = alignment;
    heapDesc.Properties = Direct3DUtil::DefaultHeapProp();
    heapDesc.Flags = !createZeroed ? D3D12_HEAP_FLAG_CREATE_NOT_ZEROED : D3D12_HEAP_FLAG_NONE;

    ID3D12Heap* heap;
    auto* device = App::GetRenderer().GetDevice();
    CheckHR(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)));

    return ResourceHeap(heap);
}

void GpuMemory::ReleaseDefaultHeapBuffer(Buffer& buffer)
{
    Assert(g_data, "Releasing GPU resources when GPU memory system has shut down.");

    AcquireSRWLockExclusive(&g_data->m_pendingResourceLock);

    g_data->m_toRelease.emplace_back(
        GpuMemoryImplData::PendingResource{ .Res = buffer.Resource(),
            .ReleaseFence = g_data->m_nextFenceVal });

    ReleaseSRWLockExclusive(&g_data->m_pendingResourceLock);
}

void GpuMemory::ReleaseTexture(Texture& texture)
{
    Assert(g_data, "Releasing GPU resources when GPU memory system has shut down.");

    AcquireSRWLockExclusive(&g_data->m_pendingResourceLock);

    g_data->m_toRelease.emplace_back(
        GpuMemoryImplData::PendingResource{ .Res = texture.Resource(),
            .ReleaseFence = g_data->m_nextFenceVal });

    ReleaseSRWLockExclusive(&g_data->m_pendingResourceLock);
}

void GpuMemory::ReleaseResourceHeap(ResourceHeap& heap)
{
    Assert(g_data, "Releasing GPU resources when GPU memory system has shut down.");

    AcquireSRWLockExclusive(&g_data->m_pendingResourceLock);

    g_data->m_toRelease.emplace_back(
        GpuMemoryImplData::PendingResource{ .Res = heap.Heap(),
            .ReleaseFence = g_data->m_nextFenceVal });

    ReleaseSRWLockExclusive(&g_data->m_pendingResourceLock);
}

Texture GpuMemory::GetTexture2D(const char* name, uint64_t width, uint32_t height, DXGI_FORMAT format,
    D3D12_RESOURCE_STATES initialState, uint32_t flags, uint16_t mipLevels, D3D12_CLEAR_VALUE* clearVal)
{
    Texture::ID_TYPE id = XXH3_64_To_32(XXH3_64bits(name, strlen(name)));
    return GetTexture2D(id, width, height, format, initialState, flags, mipLevels, clearVal);
}

Texture GpuMemory::GetTexture2D(Texture::ID_TYPE id, uint64_t width, uint32_t height, DXGI_FORMAT format,
    D3D12_RESOURCE_STATES initialState, uint32_t flags, uint16_t mipLevels, D3D12_CLEAR_VALUE* clearVal)
{
    Assert(width < D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid width.");
    Assert(height < D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid height.");
    Assert(mipLevels <= D3D12_REQ_MIP_LEVELS, "Invalid number of mip levels.");
    Assert(((flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET) & (flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL)) == 0,
        "Texture can't be used as both Render Target and Depth Stencil.");
    Assert(((flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL) & (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)) == 0,
        "A Depth-Stencil texture can't be used for unordered access.");

    D3D12_RESOURCE_FLAGS resFlags = D3D12_RESOURCE_FLAG_NONE;

    if (flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL & ~D3D12_RESOURCE_FLAG_NONE);
    if (flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET & ~D3D12_RESOURCE_FLAG_NONE);
    if (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS & ~D3D12_RESOURCE_FLAG_NONE);

    D3D12_HEAP_PROPERTIES defaultHeap = Direct3DUtil::DefaultHeapProp();
    D3D12_RESOURCE_DESC desc = Direct3DUtil::Tex2D(format, width, height, 1, mipLevels, resFlags);

    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
    if ((flags & TEXTURE_FLAGS::INIT_TO_ZERO) == 0)
        heapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

    auto* device = App::GetRenderer().GetDevice();
    ID3D12Resource* texture;
    CheckHR(device->CreateCommittedResource(&defaultHeap,
        heapFlags,
        &desc,
        initialState,
        clearVal,
        IID_PPV_ARGS(&texture)));

    return Texture(id, texture, RESOURCE_HEAP_TYPE::COMMITTED);
}

Texture GpuMemory::GetPlacedTexture2D(const char* name, uint64_t width, uint32_t height, 
    DXGI_FORMAT format, ID3D12Heap* heap, uint64_t offsetInBytes, D3D12_RESOURCE_STATES initialState, 
    uint32_t flags, uint16_t mipLevels, D3D12_CLEAR_VALUE* clearVal)
{
    Assert(width < D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid width.");
    Assert(height < D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid height.");
    Assert(mipLevels <= D3D12_REQ_MIP_LEVELS, "Invalid number of mip levels.");
    Assert(((flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET) & (flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL)) == 0,
        "Texture can't be used as both Render Target and Depth Stencil.");
    Assert(((flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL) & (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)) == 0,
        "A Depth-Stencil texture can't be used for unordered access.");

    D3D12_RESOURCE_FLAGS resFlags = D3D12_RESOURCE_FLAG_NONE;

    if (flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL & ~D3D12_RESOURCE_FLAG_NONE);
    if (flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET & ~D3D12_RESOURCE_FLAG_NONE);
    if (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS & ~D3D12_RESOURCE_FLAG_NONE);

    D3D12_RESOURCE_DESC desc = Direct3DUtil::Tex2D(format, width, height, 1, mipLevels, resFlags);
    ID3D12Resource* texture;
    auto* device = App::GetRenderer().GetDevice();
    CheckHR(device->CreatePlacedResource(heap,
        offsetInBytes,
        &desc,
        initialState,
        clearVal,
        IID_PPV_ARGS(&texture)));

    return Texture(name, texture, RESOURCE_HEAP_TYPE::PLACED);
}

Texture GpuMemory::GetPlacedTexture2D(const char* name, uint64_t width, uint32_t height,
    DXGI_FORMAT format, ID3D12Heap* heap, uint64_t offsetInBytes, D3D12_BARRIER_LAYOUT initialLayout,
    uint32_t flags, uint16_t mipLevels, D3D12_CLEAR_VALUE* clearVal)
{
    Assert(width < D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid width.");
    Assert(height < D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid height.");
    Assert(mipLevels <= D3D12_REQ_MIP_LEVELS, "Invalid number of mip levels.");
    Assert(((flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET) & (flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL)) == 0,
        "Texture can't be used as both Render Target and Depth Stencil.");
    Assert(((flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL) & (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)) == 0,
        "A Depth-Stencil texture can't be used for unordered access.");

    D3D12_RESOURCE_FLAGS resFlags = D3D12_RESOURCE_FLAG_NONE;

    if (flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL & ~D3D12_RESOURCE_FLAG_NONE);
    if (flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET & ~D3D12_RESOURCE_FLAG_NONE);
    if (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS & ~D3D12_RESOURCE_FLAG_NONE);

    D3D12_RESOURCE_DESC1 desc = Direct3DUtil::Tex2D1(format, width, height, 1, mipLevels, resFlags);
    ID3D12Resource* texture;
    auto* device = App::GetRenderer().GetDevice();
    CheckHR(device->CreatePlacedResource2(heap,
        offsetInBytes,
        &desc,
        initialLayout,
        clearVal,
        0,
        nullptr,
        IID_PPV_ARGS(&texture)));

    return Texture(name, texture, RESOURCE_HEAP_TYPE::PLACED);
}

Texture GpuMemory::GetTexture2D(const char* name, uint64_t width, uint32_t height, DXGI_FORMAT format,
    D3D12_BARRIER_LAYOUT initialLayout, uint32_t flags, uint16_t mipLevels, D3D12_CLEAR_VALUE* clearVal)
{
    Texture::ID_TYPE id = XXH3_64_To_32(XXH3_64bits(name, strlen(name)));
    return GetTexture2D(id, width, height, format, initialLayout, flags, mipLevels, clearVal);
}

Texture GpuMemory::GetTexture2D(Texture::ID_TYPE id, uint64_t width, uint32_t height, DXGI_FORMAT format,
    D3D12_BARRIER_LAYOUT initialLayout, uint32_t flags, uint16_t mipLevels, D3D12_CLEAR_VALUE* clearVal)
{
    Assert(width < D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid width.");
    Assert(height < D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid height.");
    Assert(mipLevels <= D3D12_REQ_MIP_LEVELS, "Invalid number of mip levels.");
    Assert(((flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET) & (flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL)) == 0,
        "Texture can't be used as both Render Target and Depth Stencil.");
    Assert(((flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL) & (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)) == 0,
        "A Depth-Stencil texture can't be used for unordered access.");

    D3D12_RESOURCE_FLAGS resFlags = D3D12_RESOURCE_FLAG_NONE;

    if (flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL & ~D3D12_RESOURCE_FLAG_NONE);
    if (flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET & ~D3D12_RESOURCE_FLAG_NONE);
    if (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS & ~D3D12_RESOURCE_FLAG_NONE);

    D3D12_HEAP_PROPERTIES defaultHeap = Direct3DUtil::DefaultHeapProp();
    D3D12_RESOURCE_DESC1 desc = Direct3DUtil::Tex2D1(format, width, height, 1, mipLevels, resFlags);

    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
    if ((flags & TEXTURE_FLAGS::INIT_TO_ZERO) == 0)
        heapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

    ID3D12Resource* texture;
    auto* device = App::GetRenderer().GetDevice();
    CheckHR(device->CreateCommittedResource3(&defaultHeap,
        heapFlags,
        &desc,
        initialLayout,
        clearVal,
        nullptr,
        0,
        nullptr,
        IID_PPV_ARGS(&texture)));

    return Texture(id, texture, RESOURCE_HEAP_TYPE::COMMITTED);
}

Texture GpuMemory::GetTexture3D(const char* name, uint64_t width, uint32_t height, uint16_t depth,
    DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState, uint32_t flags, uint16_t mipLevels)
{
    Assert(width < D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION, "Invalid width.");
    Assert(height < D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION, "Invalid height.");
    Assert(depth < D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION, "Invalid depth.");
    Assert(mipLevels <= D3D12_REQ_MIP_LEVELS, "Invalid number of mip levels.");
    Assert(!(flags & TEXTURE_FLAGS::ALLOW_DEPTH_STENCIL), "3D Texture can't be used as Depth Stencil.");

    D3D12_RESOURCE_FLAGS resFlags = D3D12_RESOURCE_FLAG_NONE;

    if (flags & TEXTURE_FLAGS::ALLOW_RENDER_TARGET)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET & ~D3D12_RESOURCE_FLAG_NONE);
    if (flags & TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS)
        resFlags |= (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS & ~D3D12_RESOURCE_FLAG_NONE);

    D3D12_HEAP_PROPERTIES defaultHeap = Direct3DUtil::DefaultHeapProp();
    D3D12_RESOURCE_DESC desc = Direct3DUtil::Tex3D(format, width, height, depth, mipLevels, resFlags);

    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
    if ((flags & TEXTURE_FLAGS::INIT_TO_ZERO) == 0)
        heapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

    ID3D12Resource* texture;
    auto* device = App::GetRenderer().GetDevice();
    CheckHR(device->CreateCommittedResource(&defaultHeap,
        heapFlags,
        &desc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&texture)));

    return Texture(name, texture, RESOURCE_HEAP_TYPE::COMMITTED);
}

LOAD_DDS_RESULT GpuMemory::GetTexture2DFromDisk(const App::Filesystem::Path& texPath, Texture::ID_TYPE ID,
    Texture& tex)
{
    D3D12_SUBRESOURCE_DATA subresources[DDS_Data::MAX_NUM_SUBRESOURCES];
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint16_t mipCount;
    uint32_t numSubresources;
    DXGI_FORMAT format;
    MemoryArena ma;
    const auto errCode = Direct3DUtil::LoadDDSFromFile(texPath.GetView().data(), subresources, 
        format, ArenaAllocator(ma), width, height, depth, mipCount, numSubresources);

    if (errCode != LOAD_DDS_RESULT::SUCCESS)
        return errCode;

    tex = GetTexture2D(ID, width, height, format, D3D12_RESOURCE_STATE_COPY_DEST, 0, mipCount);

    const int idx = GetThreadIndex(g_data->m_threadIDs);
    g_data->m_uploaders[idx].UploadTexture(tex.Resource(), Span(subresources, numSubresources));

    return LOAD_DDS_RESULT::SUCCESS;
}

LOAD_DDS_RESULT GpuMemory::GetTexture2DFromDisk(const App::Filesystem::Path& texPath, 
    Texture::ID_TYPE ID, Texture& tex, UploadHeapArena& heapArena, ArenaAllocator allocator)
{
    D3D12_SUBRESOURCE_DATA subresources[DDS_Data::MAX_NUM_SUBRESOURCES];
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint16_t mipCount;
    uint32_t numSubresources;
    DXGI_FORMAT format;
    const auto errCode = Direct3DUtil::LoadDDSFromFile(texPath.GetView().data(), subresources, 
        format, allocator, width, height, depth, mipCount, numSubresources);

    if (errCode != LOAD_DDS_RESULT::SUCCESS)
        return errCode;

    tex = GetTexture2D(ID, width, height, format, D3D12_RESOURCE_STATE_COPY_DEST, 0, mipCount);

    const int idx = GetThreadIndex(g_data->m_threadIDs);
    g_data->m_uploaders[idx].UploadTexture(heapArena, tex.Resource(), 
        Span(subresources, numSubresources));

    return LOAD_DDS_RESULT::SUCCESS;
}

LOAD_DDS_RESULT GpuMemory::GetDDSDataFromDisk(const App::Filesystem::Path& texPath,
    DDS_Data& dds, UploadHeapArena& heapArena, Support::ArenaAllocator allocator)
{
    return Direct3DUtil::LoadDDSFromFile(texPath.GetView().data(), dds.subresources, 
        dds.format, allocator, dds.width, dds.height, dds.depth, dds.mipCount, 
        dds.numSubresources);
}

LOAD_DDS_RESULT GpuMemory::GetTexture3DFromDisk(const App::Filesystem::Path& texPath, Texture& tex)
{
    // TODO MAX_NUM_SUBRESOURCES is not enough for 3D textures with mipmaps, though 
    // currently not needed
    D3D12_SUBRESOURCE_DATA subresources[DDS_Data::MAX_NUM_SUBRESOURCES];
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint16_t mipCount;
    uint32_t numSubresources;
    DXGI_FORMAT format;
    MemoryArena ma;
    const auto errCode = Direct3DUtil::LoadDDSFromFile(texPath.GetView().data(), subresources, 
        format, ArenaAllocator(ma), width, height, depth, mipCount, numSubresources);

    if (errCode != LOAD_DDS_RESULT::SUCCESS)
        return errCode;

    tex = GetTexture3D(texPath.Get(), width, height, (uint16_t)depth, format, 
        D3D12_RESOURCE_STATE_COPY_DEST, 0, mipCount);

    const int idx = GetThreadIndex(g_data->m_threadIDs);
    g_data->m_uploaders[idx].UploadTexture(tex.Resource(), Span(subresources, numSubresources),
        0, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    return LOAD_DDS_RESULT::SUCCESS;
}

Texture GpuMemory::GetPlacedTexture2DAndInit(Texture::ID_TYPE ID, const D3D12_RESOURCE_DESC1& desc,
    ID3D12Heap* heap, uint64_t offsetInBytes, UploadHeapArena& heapArena,
    Span<D3D12_SUBRESOURCE_DATA> subresources)
{
    ID3D12Resource* texture;
    auto* device = App::GetRenderer().GetDevice();
    CheckHR(device->CreatePlacedResource1(heap,
        offsetInBytes,
        &desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&texture)));

    const int idx = GetThreadIndex(g_data->m_threadIDs);
    g_data->m_uploaders[idx].UploadTexture(heapArena, texture, subresources);

    return Texture(ID, texture, RESOURCE_HEAP_TYPE::PLACED);
}

Texture GpuMemory::GetTexture2DAndInit(const char* name, uint64_t width, uint32_t height, 
    DXGI_FORMAT format, D3D12_RESOURCE_STATES postCopyState, uint8_t* pixels, uint32_t flags)
{
    Texture t = GetTexture2D(name, width, height, format, D3D12_RESOURCE_STATE_COPY_DEST, flags);

    const int idx = GetThreadIndex(g_data->m_threadIDs);
    g_data->m_uploaders[idx].UploadTexture(t.Resource(), pixels, postCopyState);

    return t;
}
