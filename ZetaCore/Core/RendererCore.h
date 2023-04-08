#pragma once

#include "Constants.h"
#include "DescriptorHeap.h"
#include "GpuMemory.h"
#include "GpuTimer.h"
#include "../Utility/Span.h"

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
		RendererCore() noexcept;
		~RendererCore() noexcept;

		RendererCore(const RendererCore&) = delete;
		RendererCore& operator=(const RendererCore&) = delete;

		void Init(HWND hwnd, int renderWidth, int renderHeight, int displayWidth, int displayHeight) noexcept;
		void Shutdown() noexcept;
		void OnWindowSizeChanged(HWND hwnd, int renderWidth, int renderHeight, int displayWidth, int displayHeight) noexcept;
		void WaitForSwapChainWaitableObject() noexcept;
		void BeginFrame() noexcept;
		void SubmitResourceCopies() noexcept;
		void EndFrame(Support::TaskSet& endFrameTS) noexcept;

		ZetaInline ID3D12Device8* GetDevice() { return m_deviceObjs.m_device.Get(); };
		ZetaInline const char* GetDeviceDescription() { return m_deviceObjs.m_deviceName; }
		ZetaInline IDXGIAdapter3* GetAdapter() { return m_deviceObjs.m_dxgiAdapter.Get(); }
		DXGI_OUTPUT_DESC GetOutputMonitorDesc() const noexcept;
		uint64_t GetCommandQueueTimeStampFrequency(D3D12_COMMAND_LIST_TYPE t) const noexcept;

		ZetaInline int GetRenderWidth() const { return m_renderWidth; }
		ZetaInline int GetRenderHeight() const { return m_renderHeight; }
		ZetaInline int GetDisplayWidth() const { return m_displayWidth; }
		ZetaInline int GetDisplayHeight() const { return m_displayHeight; }
		ZetaInline float GetAspectRatio() const { return (float)m_renderWidth / m_renderHeight; }
		ZetaInline int GetCurrentBackBufferIndex() const { return m_currBackBuffIdx; }
		ZetaInline Texture& GetCurrentBackBuffer() { return m_backBuffers[m_currBackBuffIdx]; }

		ZetaInline GpuMemory& GetGpuMemory() { return m_gpuMemory; }
		ZetaInline SharedShaderResources& GetSharedShaderResources() { return *m_sharedShaderRes; }
		ZetaInline DescriptorHeap& GetCbvSrvUavDescriptorHeapGpu() { return m_cbvSrvUavDescHeapGpu; };
		ZetaInline DescriptorHeap& GetCbvSrvUavDescriptorHeapCpu() { return m_cbvSrvUavDescHeapCpu; };
		ZetaInline DescriptorHeap& GetRtvDescriptorHeap() { return m_rtvDescHeap; };
		ZetaInline DescriptorHeap& GetDsvDescriptorHeap() { return m_dsvDescHeap; };
		ZetaInline GpuTimer& GetGpuTimer() { return m_gpuTimer; }

		GraphicsCmdList* GetGraphicsCmdList() noexcept;
		ComputeCmdList* GetComputeCmdList() noexcept;
		//CopyCmdList* GetCopyCmdList() noexcept;
		void ReleaseCmdList(CommandList* ctx) noexcept;
		uint64_t ExecuteCmdList(CommandList* ctx) noexcept;

		void SignalDirectQueue(ID3D12Fence* f, uint64_t v) noexcept;
		void SignalComputeQueue(ID3D12Fence* f, uint64_t v) noexcept;
//		void SignalCopyQueue(ID3D12Fence* f, uint64_t v) noexcept;

		// Waits (CPU-side) for the fence on Direct Queue to reach the specified value on (blocking)
		void WaitForDirectQueueFenceCPU(uint64_t fenceValue) noexcept;

		// Waits (CPU-side) for the fence on Direct Queue to reach the specified value on (blocking)
		void WaitForComputeQueueFenceCPU(uint64_t fenceValue) noexcept;

		// Issues a GPU-side wait on the Compute Queue for the fence on the Direct Queue. Corresponding fence
		// can only be signalled through ExecuteCmdList() calls.
		void WaitForDirectQueueOnComputeQueue(uint64_t v) noexcept;

		// Issues a GPU-side wait on the Direct Queue for the Fence on the Compute Queue. Corresponding fence
		// can only be signalled through ExecuteCmdList() calls.
		void WaitForComputeQueueOnDirectQueue(uint64_t v) noexcept;

		// Issue a GPU-side wait on the Direct/Compute Queue for the Fence on the copy queue. That fence
		// can only signalled through ExecuteCmdList() calls.
		//void WaitForCopyQueueOnDirectQueue(uint64_t v) noexcept;
		//void WaitForCopyQueueOnComputeQueue(uint64_t v) noexcept;
		void FlushAllCommandQueues() noexcept;

		ZetaInline D3D12_VIEWPORT GetDisplayViewport() const { return m_displayViewport; }
		ZetaInline D3D12_RECT GetDisplayScissor() const { return m_displayScissor; }
		ZetaInline D3D12_VIEWPORT GetRenderViewport() const { return m_renderViewport; }
		ZetaInline D3D12_RECT GetRenderScissor() const { return m_renderScissor; }
		ZetaInline const Texture& GetCurrBackBuffer() const { return m_backBuffers[m_currBackBuffIdx]; }
		ZetaInline D3D12_CPU_DESCRIPTOR_HANDLE GetCurrBackBufferRTV() const { return m_backbuffDescTable.CPUHandle(m_currBackBuffIdx); }

		ZetaInline bool IsTearingSupported() const noexcept { return m_vsyncInterval == 0 && m_deviceObjs.m_tearingSupport; };
		ZetaInline int GetVSyncInterval() const { return m_vsyncInterval; }

		ZetaInline Util::Span<D3D12_STATIC_SAMPLER_DESC> GetStaticSamplers() { return m_staticSamplers; };
		ZetaInline int GlobaIdxForDoubleBufferedResources() const noexcept { return m_globalDoubleBuffIdx; }

	private:
		void ResizeBackBuffers(HWND hwnd) noexcept;
		void InitStaticSamplers() noexcept;
		void SetVSync(const Support::ParamVariant& p) noexcept;

		DeviceObjects m_deviceObjs;

		GpuMemory m_gpuMemory;
		std::unique_ptr<SharedShaderResources> m_sharedShaderRes;
		DescriptorHeap m_cbvSrvUavDescHeapGpu;
		DescriptorHeap m_cbvSrvUavDescHeapCpu;
		DescriptorHeap m_rtvDescHeap;
		DescriptorHeap m_dsvDescHeap;
		std::unique_ptr<CommandQueue> m_directQueue;
		std::unique_ptr<CommandQueue> m_computeQueue;
		//std::unique_ptr<CommandQueue> m_copyQueue;

		DescriptorTable m_backbuffDescTable;
		DescriptorTable m_depthBuffDescTable;

		HWND m_hwnd;
		Texture m_backBuffers[Constants::NUM_BACK_BUFFERS];
		int m_currBackBuffIdx = 0;
		int m_displayWidth;
		int m_displayHeight;
		int m_renderWidth;
		int m_renderHeight;
		UINT m_presentFlags = 0;
		int m_vsyncInterval = 1;
		int m_globalDoubleBuffIdx = 0;

		D3D12_VIEWPORT m_displayViewport;
		D3D12_RECT m_displayScissor;
		D3D12_VIEWPORT m_renderViewport;
		D3D12_RECT m_renderScissor;

		D3D12_STATIC_SAMPLER_DESC m_staticSamplers[8];

		ComPtr<ID3D12Fence> m_fence;
		uint64_t m_fenceVals[Constants::NUM_BACK_BUFFERS] = { 0 };
		uint64_t m_nextFenceVal = 1;
		HANDLE m_event;

		GpuTimer m_gpuTimer;
	};
}