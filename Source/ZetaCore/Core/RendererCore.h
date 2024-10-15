#pragma once

#include "Config.h"
#include "DescriptorHeap.h"
#include "GpuTimer.h"

namespace ZetaRay::Support
{
    struct TaskSet;
    struct ParamVariant;
}

namespace ZetaRay::Core
{
    struct CommandQueue;
    class CommandList;
    class GraphicsCmdList;
    class ComputeCmdList;
    class CopyCmdList;
    class SharedShaderResources;

    class RendererCore
    {
    public:
        RendererCore();
        ~RendererCore();

        RendererCore(const RendererCore&) = delete;
        RendererCore& operator=(const RendererCore&) = delete;

        void Init(HWND hwnd, uint16_t renderWidth, uint16_t renderHeight, uint16_t displayWidth, uint16_t displayHeight);
        void Shutdown();
        void OnWindowSizeChanged(HWND hwnd, uint16_t renderWidth, uint16_t renderHeight, uint16_t displayWidth, uint16_t displayHeight);
        void WaitForSwapChainWaitableObject();
        void BeginFrame();
        void SubmitResourceCopies();
        void EndFrame(Support::TaskSet& endFrameTS);

        ZetaInline ID3D12Device10* GetDevice() { return m_deviceObjs.m_device.Get(); };
        ZetaInline const char* GetDeviceDescription() { return m_deviceObjs.m_deviceName; }
        ZetaInline IDXGIAdapter3* GetAdapter() { return m_deviceObjs.m_dxgiAdapter.Get(); }
        DXGI_OUTPUT_DESC GetOutputMonitorDesc() const;
        uint64_t GetCommandQueueTimeStampFrequency(D3D12_COMMAND_LIST_TYPE t) const;

        ZetaInline uint16_t GetRenderWidth() const { return m_renderWidth; }
        ZetaInline uint16_t GetRenderHeight() const { return m_renderHeight; }
        ZetaInline uint16_t GetDisplayWidth() const { return m_displayWidth; }
        ZetaInline uint16_t GetDisplayHeight() const { return m_displayHeight; }
        ZetaInline float GetAspectRatio() const { return (float)m_renderWidth / m_renderHeight; }
        ZetaInline int GetCurrentBackBufferIndex() const { return m_currBackBuffIdx; }
        ZetaInline const GpuMemory::Texture& GetCurrentBackBuffer() { return m_backBuffers[m_currBackBuffIdx]; }
        ZetaInline D3D12_CPU_DESCRIPTOR_HANDLE GetCurrBackBufferRTV() const { return m_backbuffDescTable.CPUHandle(m_currBackBuffIdx); }

        ZetaInline SharedShaderResources& GetSharedShaderResources() { return *m_sharedShaderRes; }
        ZetaInline DescriptorHeap& GetGpuDescriptorHeap() { return m_cbvSrvUavDescHeapGpu; };
        ZetaInline ID3D12DescriptorHeap* GetSamplerDescriptorHeap() { return m_samplerDescHeap.Get(); };
        ZetaInline DescriptorHeap& GetCbvSrvUavDescriptorHeapCpu() { return m_cbvSrvUavDescHeapCpu; };
        ZetaInline DescriptorHeap& GetRtvDescriptorHeap() { return m_rtvDescHeap; };
        //ZetaInline DescriptorHeap& GetDsvDescriptorHeap() { return m_dsvDescHeap; };
        ZetaInline GpuTimer& GetGpuTimer() { return m_gpuTimer; }

        GraphicsCmdList* GetGraphicsCmdList();
        ComputeCmdList* GetComputeCmdList();
        //CopyCmdList* GetCopyCmdList();
        void ReleaseCmdList(CommandList* ctx);
        uint64_t ExecuteCmdList(CommandList* ctx);

        void SignalDirectQueue(ID3D12Fence* f, uint64_t v);
        void SignalComputeQueue(ID3D12Fence* f, uint64_t v);
        // void SignalCopyQueue(ID3D12Fence* f, uint64_t v);

        bool IsDirectQueueFenceComplete(uint64_t fenceValue);
        bool IsComputeQueueFenceComplete(uint64_t fenceValue);

        // Waits (CPU side) for the fence on Direct Queue to reach the specified value (blocking)
        void WaitForDirectQueueFenceCPU(uint64_t fenceValue);
        void WaitForDirectQueueFenceCPU2(uint64_t fenceValue, HANDLE e);

        // Waits (CPU side) for the fence on Direct Queue to reach the specified value (blocking)
        void WaitForComputeQueueFenceCPU(uint64_t fenceValue);

        // Issues a GPU-side wait on the Compute Queue for the fence on the Direct Queue. Corresponding fence
        // can only be signalled through ExecuteCmdList() calls.
        void WaitForDirectQueueOnComputeQueue(uint64_t v);

        // Issues a GPU-side wait on the Direct Queue for the Fence on the Compute Queue. Corresponding fence
        // can only be signalled through ExecuteCmdList() calls.
        void WaitForComputeQueueOnDirectQueue(uint64_t v);

        // Issue a GPU-side wait on the Direct/Compute Queue for the Fence on the copy queue. That fence
        // can only signalled through ExecuteCmdList() calls.
        //void WaitForCopyQueueOnDirectQueue(uint64_t v);
        //void WaitForCopyQueueOnComputeQueue(uint64_t v);
        void FlushAllCommandQueues();

        ZetaInline D3D12_VIEWPORT GetDisplayViewport() const { return m_displayViewport; }
        ZetaInline D3D12_RECT GetDisplayScissor() const { return m_displayScissor; }
        ZetaInline D3D12_VIEWPORT GetRenderViewport() const { return m_renderViewport; }
        ZetaInline D3D12_RECT GetRenderScissor() const { return m_renderScissor; }

        ZetaInline bool IsRGBESupported() const { return m_deviceObjs.m_rgbeSupport; };
        ZetaInline bool IsTearingSupported() const { return m_vsyncInterval == 0 && m_deviceObjs.m_tearingSupport; };
        ZetaInline int GetVSyncInterval() const { return m_vsyncInterval; }

        ZetaInline Util::Span<D3D12_STATIC_SAMPLER_DESC> GetStaticSamplers() { return m_staticSamplers; };
        ZetaInline int GlobalIdxForDoubleBufferedResources() const { return m_globalDoubleBuffIdx; }

    private:
        void ResizeBackBuffers(HWND hwnd);
        void InitStaticSamplers();
        void SetVSync(const Support::ParamVariant& p);

        DeviceObjects m_deviceObjs;

        std::unique_ptr<SharedShaderResources> m_sharedShaderRes;
        DescriptorHeap m_cbvSrvUavDescHeapGpu;
        DescriptorHeap m_cbvSrvUavDescHeapCpu;
        DescriptorHeap m_rtvDescHeap;
        ComPtr<ID3D12DescriptorHeap> m_samplerDescHeap;
        //DescriptorHeap m_dsvDescHeap;
        std::unique_ptr<CommandQueue> m_directQueue;
        std::unique_ptr<CommandQueue> m_computeQueue;
        //std::unique_ptr<CommandQueue> m_copyQueue;

        DescriptorTable m_backbuffDescTable;
        DescriptorTable m_depthBuffDescTable;

        HWND m_hwnd;
        GpuMemory::Texture m_backBuffers[Constants::NUM_BACK_BUFFERS];
        uint16_t m_currBackBuffIdx = 0;
        uint16_t m_displayWidth;
        uint16_t m_displayHeight;
        uint16_t m_renderWidth;
        uint16_t m_renderHeight;
        UINT m_presentFlags = 0;
        uint16_t m_vsyncInterval = 1;
        uint16_t m_globalDoubleBuffIdx = 0;

        D3D12_VIEWPORT m_displayViewport;
        D3D12_RECT m_displayScissor;
        D3D12_VIEWPORT m_renderViewport;
        D3D12_RECT m_renderScissor;

        D3D12_STATIC_SAMPLER_DESC m_staticSamplers[9];

        ComPtr<ID3D12Fence> m_fence;
        uint64_t m_fenceVals[Constants::NUM_BACK_BUFFERS] = { 0 };
        uint64_t m_nextFenceVal = 1;
        HANDLE m_event;

        GpuTimer m_gpuTimer;
    };
}