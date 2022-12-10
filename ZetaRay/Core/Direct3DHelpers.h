// Ref: DirectXTK12, available from (under MIT License):
// https://github.com/microsoft/DirectXTK12

#pragma once

#include "Device.h"
#include "../App/App.h"
#include <memory>

namespace ZetaRay::App
{
    struct PoolAllocator;
}

namespace ZetaRay::Util
{
    template<typename T, typename Allocator>
    class Vector;
}

namespace ZetaRay::Core
{
    struct Texture;
    struct DefaultHeapBuffer;
}

namespace ZetaRay::Core::Direct3DHelper
{
    struct DDS_HEADER;

    inline D3D12_HEAP_PROPERTIES UploadHeapProp() noexcept
    {
        D3D12_HEAP_PROPERTIES uploadHeap{
            .Type = D3D12_HEAP_TYPE_UPLOAD,
            .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
            .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
            .CreationNodeMask = 1,
            .VisibleNodeMask = 1 };

        return uploadHeap;
    }

    inline D3D12_HEAP_PROPERTIES DefaultHeapProp() noexcept
    {
        D3D12_HEAP_PROPERTIES defaultHeap{
            .Type = D3D12_HEAP_TYPE_DEFAULT,
            .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
            .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
            .CreationNodeMask = 1,
            .VisibleNodeMask = 1 };

        return defaultHeap;
    }

    inline D3D12_HEAP_PROPERTIES ReadbackHeapProp() noexcept
    {
        D3D12_HEAP_PROPERTIES defaultHeap{
            .Type = D3D12_HEAP_TYPE_READBACK,
            .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
            .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
            .CreationNodeMask = 1,
            .VisibleNodeMask = 1 };

        return defaultHeap;
    }

    inline D3D12_RESOURCE_DESC BufferResourceDesc(UINT64 w,
        D3D12_RESOURCE_FLAGS f = D3D12_RESOURCE_FLAG_NONE) noexcept
    {
        D3D12_RESOURCE_DESC bufferDesc;

        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Alignment = 0;
        bufferDesc.Width = w;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.SampleDesc.Quality = 0;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = f;

        return bufferDesc;
    }

    inline D3D12_RESOURCE_DESC Tex1D(DXGI_FORMAT format, UINT64 width,
        int arraySize = 1, int mipLevels = 1,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
        D3D12_TEXTURE_LAYOUT layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        UINT64 alignment = 0) noexcept
    {
        D3D12_RESOURCE_DESC tex1DDesc;

        tex1DDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        tex1DDesc.Format = format;
        tex1DDesc.Alignment = alignment;
        tex1DDesc.Width = width;
        tex1DDesc.Height = 1;
        tex1DDesc.MipLevels = mipLevels;
        tex1DDesc.DepthOrArraySize = arraySize;
        tex1DDesc.Flags = flags;
        tex1DDesc.Layout = layout;
        tex1DDesc.SampleDesc.Count = 1;
        tex1DDesc.SampleDesc.Quality = 0;

        return tex1DDesc;
    }

    inline D3D12_RESOURCE_DESC Tex2D(DXGI_FORMAT format, UINT64 width, UINT height,
        int arraySize = 1, int mipLevels = 1,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
        D3D12_TEXTURE_LAYOUT layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        UINT64 alignment = 0) noexcept
    {
        D3D12_RESOURCE_DESC tex2DDesc;

        tex2DDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex2DDesc.Format = format;
        tex2DDesc.Alignment = alignment;
        tex2DDesc.Width = width;
        tex2DDesc.Height = height;
        tex2DDesc.MipLevels = mipLevels;
        tex2DDesc.DepthOrArraySize = arraySize;
        tex2DDesc.Flags = flags;
        tex2DDesc.Layout = layout;
        tex2DDesc.SampleDesc.Count = 1;
        tex2DDesc.SampleDesc.Quality = 0;

        return tex2DDesc;
    }

    inline D3D12_RESOURCE_DESC Tex3D(DXGI_FORMAT format, UINT64 width, UINT height,
        int depth = 1, int mipLevels = 1,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
        D3D12_TEXTURE_LAYOUT layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        UINT64 alignment = 0) noexcept
    {
        D3D12_RESOURCE_DESC tex2DDesc;

        tex2DDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        tex2DDesc.Format = format;
        tex2DDesc.Alignment = alignment;
        tex2DDesc.Width = width;
        tex2DDesc.Height = height;
        tex2DDesc.MipLevels = mipLevels;
        tex2DDesc.DepthOrArraySize = depth;
        tex2DDesc.Flags = flags;
        tex2DDesc.Layout = layout;
        tex2DDesc.SampleDesc.Count = 1;
        tex2DDesc.SampleDesc.Quality = 0;

        return tex2DDesc;
    }

    inline D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* res,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after,
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE) noexcept
    {
        D3D12_RESOURCE_BARRIER barrier{};

        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = res;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = subresource;
        barrier.Flags = flags;

        return barrier;
    }

    inline D3D12_RESOURCE_BARRIER UAVBarrier(ID3D12Resource* res) noexcept
    {
        D3D12_RESOURCE_BARRIER barrier{};

        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.UAV.pResource = res;

        return barrier;
    }

    // Return the BPP for a particular format
    size_t BitsPerPixel(DXGI_FORMAT fmt) noexcept;

    // Get surface information for a particular format
    HRESULT GetSurfaceInfo(size_t width,
        size_t height,
        DXGI_FORMAT fmt,
        size_t* outNumBytes,
        size_t* outRowBytes,
        size_t* outNumRows) noexcept;

    // Returns required size of a buffer to be used for data upload
    inline UINT64 GetRequiredIntermediateSize(ID3D12Resource* pDestinationResource, UINT FirstSubresource,
        UINT NumSubresources) noexcept
    {
        auto Desc = pDestinationResource->GetDesc();
        UINT64 RequiredSize = 0;

        ID3D12Device* pDevice = nullptr;
        pDestinationResource->GetDevice(IID_ID3D12Device, reinterpret_cast<void**>(&pDevice));
        pDevice->GetCopyableFootprints(&Desc, FirstSubresource, NumSubresources, 0, nullptr, nullptr, nullptr, &RequiredSize);
        pDevice->Release();

        return RequiredSize;
    }

    inline D3D12_BLEND_DESC DefaultBlendDesc() noexcept
    {
        D3D12_BLEND_DESC desc{};
        desc.AlphaToCoverageEnable = false;
        desc.IndependentBlendEnable = false;

        D3D12_RENDER_TARGET_BLEND_DESC rtBlendDesc{};
        rtBlendDesc.BlendEnable = false;
        rtBlendDesc.LogicOpEnable = false;
        rtBlendDesc.SrcBlend = D3D12_BLEND_ONE;
        rtBlendDesc.DestBlend = D3D12_BLEND_ZERO;
        rtBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
        rtBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        for (int i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
            desc.RenderTarget[i] = rtBlendDesc;

        return desc;
    }

    inline D3D12_RASTERIZER_DESC DefaultRasterizerDesc() noexcept
    {
        D3D12_RASTERIZER_DESC desc{};

        desc.FillMode = D3D12_FILL_MODE_SOLID;
        desc.CullMode = D3D12_CULL_MODE_BACK;
        desc.FrontCounterClockwise = false;
        desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        desc.DepthClipEnable = true;
        desc.MultisampleEnable = false;
        desc.AntialiasedLineEnable = false;
        desc.ForcedSampleCount = 0;
        desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

        return desc;
    }

    inline D3D12_DEPTH_STENCIL_DESC DefaultDepthStencilDesc() noexcept
    {
        D3D12_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable = true;
        desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        desc.StencilEnable = false;
        desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

        D3D12_DEPTH_STENCILOP_DESC defaultStencilOp{};
        defaultStencilOp.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        defaultStencilOp.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        defaultStencilOp.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        defaultStencilOp.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

        desc.FrontFace = defaultStencilOp;
        desc.BackFace = defaultStencilOp;

        return desc;
    }

    inline constexpr DXGI_FORMAT NoSRGB(DXGI_FORMAT fmt) noexcept
    {
        switch (fmt)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8X8_UNORM;
        default:
            return fmt;
        }
    }

    void LoadDDSFromFile(const char* path,
        Util::Vector<D3D12_SUBRESOURCE_DATA, App::ThreadAllocator>& subresources,
        DXGI_FORMAT& format,
        std::unique_ptr<uint8_t[]>& ddsData,
        uint32_t& width,
        uint32_t& height,
        uint32_t& depth,
        uint32_t& mipCount) noexcept;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC GetPSODesc(const D3D12_INPUT_LAYOUT_DESC* inputLayout,
        int numRenderTargets,
        DXGI_FORMAT* rtvFormats,
        DXGI_FORMAT dsvFormat,
        D3D12_RASTERIZER_DESC* rasterizerDesc = nullptr,
        D3D12_BLEND_DESC* blendDesc = nullptr,
        D3D12_DEPTH_STENCIL_DESC* depthStencilDesc = nullptr,
        D3D12_PRIMITIVE_TOPOLOGY_TYPE primitiveTopology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) noexcept;

    uint64_t GetPSODescHash(D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) noexcept;

    void CreateGraphicsPSO(D3D12_GRAPHICS_PIPELINE_STATE_DESC& psDesc,
        ID3D12RootSignature* rootSignature,
        const D3D12_SHADER_BYTECODE* vertexShader,
        const D3D12_SHADER_BYTECODE* pixelShader,
        const D3D12_SHADER_BYTECODE* hullShader,
        const D3D12_SHADER_BYTECODE* domainShader,
        ID3D12PipelineState** pPipelineState) noexcept;

    void CreateBufferSRV(const DefaultHeapBuffer& buff, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, 
        UINT stride, UINT numElements) noexcept;
    void CreateBufferUAV(const DefaultHeapBuffer& buff, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, 
        UINT stride, UINT numElements) noexcept;
    void CreateTexture2DSRV(const Texture& t, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN,
        float minLODClamp = 0.0f, UINT mostDetailedMip = 0, UINT planeSlice = 0) noexcept;
    void CreateTexture3DSRV(const Texture& t, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN,
        float minLODClamp = 0.0f, UINT mostDetailedMip = 0, UINT planeSlice = 0) noexcept;
    void CreateTexture2DUAV(const Texture& t, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN, 
        UINT mipSlice = 0, UINT planeSlice = 0) noexcept;
    void CreateTexture3DUAV(const Texture& t, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN, 
        UINT mipSlice = 0, UINT numSlices = 0, UINT firstSliceIdx = 0) noexcept;

    void CreateRTV(const Texture& t, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN, 
        UINT mipSlice = 0, UINT planeSlice = 0) noexcept;
}