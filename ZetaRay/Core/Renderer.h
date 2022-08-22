#pragma once

#include "Constants.h"
#include "DescriptorHeap.h"
#include "GpuMemory.h"
#include "GpuTimer.h"

namespace ZetaRay
{
	struct CommandQueue;
	class CommandList;
	class GraphicsCmdList;
	class ComputeCmdList;
	class CopyCmdList;
	class SharedShaderResources;
	struct TaskSet;
	struct ParamVariant;

	class Renderer
	{
	public:
		Renderer() noexcept = default;
		~Renderer() noexcept;

		Renderer(const Renderer&) = delete;
		Renderer& operator=(const Renderer&) = delete;

		void Init(HWND hwnd, int renderWidth, int renderHeight, int displayWidth, int displayHeight) noexcept;
		void Shutdown() noexcept;
		void OnWindowSizeChanged(HWND hwnd, int renderWidth, int renderHeight, int displayWidth, int displayHeight) noexcept;
		void BeginFrame() noexcept;
		void SubmitResourceCopies() noexcept;
		void EndFrame(TaskSet& endFrameTS) noexcept;

		ID3D12Device8* GetDevice() { return m_deviceObjs.m_device.Get(); };
		const char* GetDeviceDescription() { return m_deviceObjs.m_deviceName; }
		IDXGIAdapter3* GetAdapter() { return m_deviceObjs.m_dxgiAdapter.Get(); }
		DXGI_OUTPUT_DESC GetOutputMonitorDesc() noexcept;
		uint64_t GetCommandQueueTimeStampFrequency(D3D12_COMMAND_LIST_TYPE t) noexcept;

		int GetRenderWidth() { return m_renderWidth; }
		int GetRenderHeight() { return m_renderHeight; }
		int GetDisplayWidth() { return m_displayWidth; }
		int GetDisplayHeight() { return m_displayHeight; }
		float GetAspectRatio() { return (float)m_renderWidth / m_renderHeight; }
		Texture& GetCurrentBackBuffer() { return m_backBuffers[m_currBackBuffIdx]; }

		GpuMemory& GetGpuMemory() { return m_gpuMemory; }
		SharedShaderResources& GetSharedShaderResources() { return *m_sharedShaderRes; }
		DescriptorHeap& GetCbvSrvUavDescriptorHeapGpu() { return m_cbvSrvUavDescHeapGpu; };
		DescriptorHeap& GetCbvSrvUavDescriptorHeapCpu() { return m_cbvSrvUavDescHeapCpu; };
		DescriptorHeap& GetRtvDescriptorHeap() { return m_rtvDescHeap; };
		DescriptorHeap& GetDsvDescriptorHeap() { return m_dsvDescHeap; };
		GpuTimer& GetGpuTimer() { return m_gpuTimer; }

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

		D3D12_VIEWPORT GetDisplayViewport() { return m_displayViewport; }
		D3D12_RECT GetDisplayScissor() { return m_displayScissor; }
		D3D12_VIEWPORT GetRenderViewport() { return m_renderViewport; }
		D3D12_RECT GetRenderScissor() { return m_renderScissor; }
		const Texture& GetCurrBackBuffer() { return m_backBuffers[m_currBackBuffIdx]; }
		D3D12_CPU_DESCRIPTOR_HANDLE GetCurrBackBufferRTV() { return m_backbuffDescTable.CPUHandle(m_currBackBuffIdx); }

		bool IsTearingSupported() noexcept { return m_vsyncInterval == 0 && m_deviceObjs.m_tearingSupport; };
		int GetVSyncInterval() { return m_vsyncInterval; }

		const D3D12_STATIC_SAMPLER_DESC* GetStaticSamplers() { return m_staticSamplers; };
		int CurrOutIdx() noexcept;

	private:
		void ResizeBackBuffers(HWND hwnd) noexcept;
		void InitStaticSamplers() noexcept;
		void SetVSync(const ParamVariant& p) noexcept;

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
		Texture m_backBuffers[RendererConstants::NUM_BACK_BUFFERS];
		int m_currBackBuffIdx = 0;
		int m_displayWidth;
		int m_displayHeight;
		int m_renderWidth;
		int m_renderHeight;
		UINT m_presentFlags = 0;
		int m_vsyncInterval = 1;

		D3D12_VIEWPORT m_displayViewport;
		D3D12_RECT m_displayScissor;
		D3D12_VIEWPORT m_renderViewport;
		D3D12_RECT m_renderScissor;

		D3D12_STATIC_SAMPLER_DESC m_staticSamplers[RendererConstants::NUM_STATIC_SAMPLERS];

		ComPtr<ID3D12Fence> m_fence;
		uint64_t m_fenceVals[RendererConstants::NUM_BACK_BUFFERS] = { 0, 0, 0 };
		uint64_t m_currFenceVal = 1;
		HANDLE m_event;

		GpuTimer m_gpuTimer;
	};
}