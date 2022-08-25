#include "Renderer.h"
#include "CommandQueue.h" 
#include "CommandList.h"
#include "SharedShaderResources.h"
#include "Direct3DHelpers.h"
#include "../SupportSystem/Task.h"
#include "../SupportSystem/Param.h"
#include "../Win32/App.h"
#include "../Win32/Timer.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// Renderer
//--------------------------------------------------------------------------------------

Renderer::~Renderer() noexcept
{
}

void Renderer::Init(HWND hwnd, int renderWidth, int renderHeight, int displayWidth, int displayHeight) noexcept
{
	m_hwnd = hwnd;

	m_deviceObjs.InitializeAdapter();
	m_deviceObjs.CreateDevice();
	InitStaticSamplers();

	CheckHR(m_deviceObjs.m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));
	m_event = CreateEventA(nullptr, false, false, "CommandQueue");
	AssertWin32(m_event);

	m_renderWidth = renderWidth;
	m_renderHeight = renderHeight;
	m_displayWidth = displayWidth;
	m_displayHeight = displayHeight;

	// memory
	m_gpuMemory.Init();
	m_gpuMemory.BeginFrame();

	// descriptor heaps
	m_cbvSrvUavDescHeapGpu.Init(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		RendererConstants::NUM_CBV_SRV_UAV_DESC_HEAP_GPU_DESCRIPTORS,
		true);

	m_cbvSrvUavDescHeapCpu.Init(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		RendererConstants::NUM_CBV_SRV_UAV_DESC_HEAP_CPU_DESCRIPTORS,
		false);

	m_rtvDescHeap.Init(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
		RendererConstants::NUM_RTV_DESC_HEAP_DESCRIPTORS,
		false);

	m_dsvDescHeap.Init(D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
		RendererConstants::NUM_DSV_DESC_HEAP_DESCRIPTORS,
		false);

	// command queues
	m_directQueue.reset(new(std::nothrow) CommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT, "Direct Command-Queue"));
	m_computeQueue.reset(new(std::nothrow) CommandQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE, "Compute Command-Queue"));
	//m_copyQueue.reset(new CommandQueue(D3D12_COMMAND_LIST_TYPE_COPY, L"Copy Command-Queue"));

	m_sharedShaderRes.reset(new(std::nothrow) SharedShaderResources);

	// swapchain & depth descriptor tables
	m_backbuffDescTable = m_rtvDescHeap.Allocate(RendererConstants::NUM_BACK_BUFFERS);
	m_depthBuffDescTable = m_dsvDescHeap.Allocate(1);

	ResizeBackBuffers(hwnd);

	m_renderViewport.TopLeftX = 0.0f;
	m_renderViewport.TopLeftY = 0.0f;
	m_renderViewport.Width = (float)m_renderWidth;
	m_renderViewport.Height = (float)m_renderHeight;
	m_renderViewport.MinDepth = D3D12_MIN_DEPTH;
	m_renderViewport.MaxDepth = D3D12_MAX_DEPTH;

	m_renderScissor.left = 0;
	m_renderScissor.top = 0;
	m_renderScissor.right = m_renderWidth;
	m_renderScissor.bottom = m_renderHeight;

	if (m_vsyncInterval == 0 && m_deviceObjs.m_tearingSupport)
	{
		m_presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
		//CheckHR(m_deviceObjs.m_dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
	}

	// gpu timer
	m_gpuTimer.Init();

	ParamVariant p0;
	p0.InitBool("Renderer", "Settings", "VSync", fastdelegate::MakeDelegate(this, &Renderer::SetVSync),
		m_vsyncInterval > 0);
	App::AddParam(p0);
}

int Renderer::CurrOutIdx() noexcept
{
	return App::GetTimer().GetTotalFrameCount() & 0x1;
}

void Renderer::ResizeBackBuffers(HWND hwnd) noexcept
{
	// if the backbuffers already exist, resize them
	if (m_backBuffers[0].GetResource())
	{
		for (int i = 0; i < RendererConstants::NUM_BACK_BUFFERS; i++)
		{
			m_backBuffers[i].Reset(false);
		}

		m_deviceObjs.ResizeSwapChain(m_displayWidth, m_displayHeight, RendererConstants::MAX_SWAPCHAIN_FRAME_LATENCY);
	}
	else
		m_deviceObjs.CreateSwapChain(m_directQueue->GetCommandQueue(), 
			hwnd, 
			m_displayWidth, m_displayHeight,
			RendererConstants::NUM_BACK_BUFFERS,
			Direct3DHelper::NoSRGB(RendererConstants::BACK_BUFFER_FORMAT),
			RendererConstants::MAX_SWAPCHAIN_FRAME_LATENCY);

	m_currBackBuffIdx = m_deviceObjs.m_dxgiSwapChain->GetCurrentBackBufferIndex();

	// obtain the buffers
	for (int i = 0; i < RendererConstants::NUM_BACK_BUFFERS; i++)
	{
		ComPtr<ID3D12Resource> backbuff;
		CheckHR(m_deviceObjs.m_dxgiSwapChain->GetBuffer(i, IID_PPV_ARGS(backbuff.GetAddressOf())));

		StackStr(buff, n, "Backbuffer_%d", i);
		m_backBuffers[i] = ZetaMove(Texture(buff, ZetaMove(backbuff)));
	}

	// create RTVs
	for (int i = 0; i < RendererConstants::NUM_BACK_BUFFERS; i++)
	{
		D3D12_RENDER_TARGET_VIEW_DESC desc{};
		desc.Format = RendererConstants::BACK_BUFFER_FORMAT;
		desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

		m_deviceObjs.m_device->CreateRenderTargetView(m_backBuffers[i].GetResource(), &desc, m_backbuffDescTable.CPUHandle(i));
	}

	m_displayViewport.TopLeftX = 0.0f;
	m_displayViewport.TopLeftY = 0.0f;
	m_displayViewport.Width = (float)m_displayWidth;
	m_displayViewport.Height = (float)m_displayHeight;
	m_displayViewport.MinDepth = D3D12_MIN_DEPTH;
	m_displayViewport.MaxDepth = D3D12_MAX_DEPTH;

	m_displayScissor.left = 0;
	m_displayScissor.top = 0;
	m_displayScissor.right = m_displayWidth;
	m_displayScissor.bottom = m_displayHeight;
}

void Renderer::Shutdown() noexcept
{
	if (!m_deviceObjs.m_tearingSupport)
	{
		// Ref: https://docs.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi
		// "You may not release a swap chain in full-screen mode because doing so may create thread contention"
		m_deviceObjs.m_dxgiSwapChain->SetFullscreenState(false, nullptr);
	}

	m_backbuffDescTable.Reset();
	m_depthBuffDescTable.Reset();
	m_cbvSrvUavDescHeapGpu.Shutdown();
	m_cbvSrvUavDescHeapCpu.Shutdown();
	m_dsvDescHeap.Shutdown();
	m_rtvDescHeap.Shutdown();
	m_gpuMemory.Shutdown();
	m_gpuTimer.Shutdown();

	for (int i = 0; i < RendererConstants::NUM_BACK_BUFFERS; i++)
	{
		m_backBuffers[i].Reset();
	}

	FlushAllCommandQueues();

	m_directQueue.reset();
	m_computeQueue.reset();
}

void Renderer::OnWindowSizeChanged(HWND hwnd, int renderWidth, int renderHeight, int displayWidth, int displayHeight) noexcept
{
	FlushAllCommandQueues();

	const bool resizeNeeded = displayWidth != m_displayWidth || displayHeight != m_displayHeight;

	m_renderWidth = renderWidth;
	m_renderHeight = renderHeight;
	m_displayWidth = displayWidth;
	m_displayHeight = displayHeight;

	if (resizeNeeded)
	{
		ResizeBackBuffers(hwnd);

		BOOL fullscreenState;
		CheckHR(m_deviceObjs.m_dxgiSwapChain->GetFullscreenState(&fullscreenState, nullptr));

		// DXGI_PRESENT_ALLOW_TEARING cannot be enabled in full-screen
		if (fullscreenState)
			m_presentFlags &= ~DXGI_PRESENT_ALLOW_TEARING;
	}

	m_renderViewport.TopLeftX = 0.0f;
	m_renderViewport.TopLeftY = 0.0f;
	m_renderViewport.Width = (float)m_renderWidth;
	m_renderViewport.Height = (float)m_renderHeight;
	m_renderViewport.MinDepth = D3D12_MIN_DEPTH;
	m_renderViewport.MaxDepth = D3D12_MAX_DEPTH;

	m_renderScissor.left = 0;
	m_renderScissor.top = 0;
	m_renderScissor.right = m_renderWidth;
	m_renderScissor.bottom = m_renderHeight;
}

void Renderer::BeginFrame() noexcept
{
	if (App::GetTimer().GetTotalFrameCount() > 0)
	{
		m_gpuMemory.BeginFrame();
	}
	
	m_gpuTimer.BeginFrame();
}

void Renderer::SubmitResourceCopies() noexcept
{
	m_gpuMemory.SubmitResourceCopies();

	App::AddFrameStat("Renderer", "RTV Desc. Heap", m_rtvDescHeap.GetNumFreeSlots(), m_rtvDescHeap.GetHeapSize());
	App::AddFrameStat("Renderer", "Gpu Desc. Heap", m_cbvSrvUavDescHeapGpu.GetNumFreeSlots(), m_cbvSrvUavDescHeapGpu.GetHeapSize());
}

void Renderer::EndFrame(TaskSet& endFrameTS) noexcept
{
	auto h0 = endFrameTS.EmplaceTask("Present", [this]()
		{
			const int currBackBuffIdx = m_currBackBuffIdx;

			auto hr = m_deviceObjs.m_dxgiSwapChain->Present(m_vsyncInterval, m_presentFlags);
			if (FAILED(hr))
			{
				if (hr == DXGI_ERROR_DEVICE_REMOVED)
				{
					//ComPtr<ID3D12DeviceRemovedExtendedData1> pDred;
					//CheckHR(m_deviceObjs.m_device->QueryInterface(IID_PPV_ARGS(&pDred)));

					//D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 DredAutoBreadcrumbsOutput;
					//D3D12_DRED_PAGE_FAULT_OUTPUT DredPageFaultOutput;
					//CheckHR(pDred->GetAutoBreadcrumbsOutput1(&DredAutoBreadcrumbsOutput));
					//CheckHR(pDred->GetPageFaultAllocationOutput(&DredPageFaultOutput));
				}

				__debugbreak();
			}

			// Schedule a Signal command in the queue.
			// Set the fence value for the next frame.
			m_fenceVals[currBackBuffIdx] = m_currFenceVal;
			CheckHR(m_directQueue->GetCommandQueue()->Signal(m_fence.Get(), m_currFenceVal++));

			// Update the back buffer index.
			const int nextBackBuffidx = m_deviceObjs.m_dxgiSwapChain->GetCurrentBackBufferIndex();
			const uint64_t completed = m_fence->GetCompletedValue();

			if (completed < m_fenceVals[nextBackBuffidx])
			{
				CheckHR(m_fence->SetEventOnCompletion(m_fenceVals[nextBackBuffidx], m_event));
				//uint64_t f = App::GetTimer().GetTotalFrameCount();
				//printf("Frame %llu, CPU waiting for GPU...\n", f);
				WaitForSingleObject(m_event, INFINITE);
			}

			m_currBackBuffIdx = nextBackBuffidx;
		});

	auto h1 = endFrameTS.EmplaceTask("RecycleGpuMem", [this]()
		{
			m_gpuMemory.Recycle();
		});

	auto h2 = endFrameTS.EmplaceTask("RecycleDescHeaps", [this]()
		{
			m_cbvSrvUavDescHeapGpu.Recycle();
			m_cbvSrvUavDescHeapCpu.Recycle();
			m_rtvDescHeap.Recycle();
			m_dsvDescHeap.Recycle();
		});
}

DXGI_OUTPUT_DESC Renderer::GetOutputMonitorDesc() noexcept
{
	ComPtr<IDXGIOutput> pOutput;
	CheckHR(m_deviceObjs.m_dxgiSwapChain->GetContainingOutput(&pOutput));

	DXGI_OUTPUT_DESC desc;
	CheckHR(pOutput->GetDesc(&desc));

	return desc;
}

uint64_t Renderer::GetCommandQueueTimeStampFrequency(D3D12_COMMAND_LIST_TYPE t) noexcept
{
	uint64_t freq = -1;

	if (t == D3D12_COMMAND_LIST_TYPE_DIRECT)
	{
		CheckHR(m_directQueue->m_cmdQueue->GetTimestampFrequency(&freq));
	}
	else if (t == D3D12_COMMAND_LIST_TYPE_COMPUTE)
	{
		CheckHR(m_computeQueue->m_cmdQueue->GetTimestampFrequency(&freq));
	}
//	else if (t == D3D12_COMMAND_LIST_TYPE_COPY)
//		m_computeQueue->m_cmdQueue->GetTimestampFrequency(&freq);

	return freq;
}

GraphicsCmdList* Renderer::GetGraphicsCmdList() noexcept
{
	CommandList* ctx = m_directQueue->GetCommandList();
	Assert(ctx->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast.");

	return static_cast<GraphicsCmdList*>(ctx);
}

ComputeCmdList* Renderer::GetComputeCmdList() noexcept
{
	CommandList* ctx = m_computeQueue->GetCommandList();
	Assert(ctx->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast.");

	return static_cast<ComputeCmdList*>(ctx);
}

// There is another "CopyContext" defined in WinBase.h!
//CopyCmdList* Renderer::GetCopyCmdList() noexcept
//{
//	auto* ctx = m_copyQueue->GetCommandList();
//	Assert(ctx->CommandListType() == D3D12_COMMAND_LIST_TYPE_COPY, "Invalid downcast.");
//
//	return static_cast<ZetaRay::CopyCmdList*>(ctx);
//}

void Renderer::ReleaseCmdList(CommandList* ctx) noexcept
{
	if (ctx->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
		m_directQueue->ReleaseCommandList(ctx);	
	else if (ctx->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE)
		m_computeQueue->ReleaseCommandList(ctx);
}

uint64_t Renderer::ExecuteCmdList(CommandList* ctx) noexcept
{
	if (ctx->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
	{
		return m_directQueue->ExecuteCommandList(ctx);
	}
	else if (ctx->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE)
	{
		return m_computeQueue->ExecuteCommandList(ctx);
	}

	return -1;
}

void Renderer::SignalDirectQueue(ID3D12Fence* f, uint64_t v) noexcept
{
	auto* cmdQueue = m_directQueue->GetCommandQueue();
	Assert(cmdQueue, "cmdQueue was NULL");
	cmdQueue->Signal(f, v);
}

void Renderer::SignalComputeQueue(ID3D12Fence* f, uint64_t v) noexcept
{
	m_computeQueue->GetCommandQueue()->Signal(f, v);
}

void Renderer::WaitForDirectQueueFenceCPU(uint64_t fenceValue) noexcept
{
	m_directQueue->WaitForFenceCPU(fenceValue);
}

void Renderer::WaitForComputeQueueFenceCPU(uint64_t fenceValue) noexcept
{
	m_computeQueue->WaitForFenceCPU(fenceValue);
}

void Renderer::WaitForDirectQueueOnComputeQueue(uint64_t v) noexcept
{
	// MS Docs:
	// "Queues a GPU-side wait, and returns immediately. A GPU-side wait is where 
	// the GPU waits until the specified fence reaches or exceeds the specified value."
	//
	// command queue waits (during that time no work is executed) until the fence 
	// reaches the requested value
	CheckHR(m_computeQueue->m_cmdQueue->Wait(m_directQueue->m_fence.Get(), v));
}

void Renderer::WaitForComputeQueueOnDirectQueue(uint64_t v) noexcept
{
	CheckHR(m_directQueue->m_cmdQueue->Wait(m_computeQueue->m_fence.Get(), v));
}

void Renderer::FlushAllCommandQueues() noexcept
{
	m_directQueue->WaitForIdle();
	m_computeQueue->WaitForIdle();
}

void Renderer::InitStaticSamplers() noexcept
{
	D3D12_STATIC_SAMPLER_DESC pointWrap{};
	pointWrap.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	pointWrap.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	pointWrap.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	pointWrap.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	pointWrap.MipLODBias = 0;
	pointWrap.MaxAnisotropy = 16;
	pointWrap.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	pointWrap.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	pointWrap.MinLOD = 0.0f;
	pointWrap.MaxLOD = D3D12_FLOAT32_MAX;
	pointWrap.ShaderRegister = 0;
	pointWrap.RegisterSpace = 0;
	pointWrap.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_STATIC_SAMPLER_DESC pointClamp{};
	pointClamp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	pointClamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	pointClamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	pointClamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	pointClamp.MipLODBias = 0;
	pointClamp.MaxAnisotropy = 16;
	pointClamp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	pointClamp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	pointClamp.MinLOD = 0.0f;
	pointClamp.MaxLOD = D3D12_FLOAT32_MAX;
	pointClamp.ShaderRegister = 1;
	pointClamp.RegisterSpace = 0;
	pointClamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_STATIC_SAMPLER_DESC linearWrap{};
	linearWrap.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	linearWrap.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	linearWrap.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	linearWrap.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	linearWrap.MipLODBias = 0;
	linearWrap.MaxAnisotropy = 16;
	linearWrap.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	linearWrap.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	linearWrap.MinLOD = 0.0f;
	linearWrap.MaxLOD = D3D12_FLOAT32_MAX;
	linearWrap.ShaderRegister = 2;
	linearWrap.RegisterSpace = 0;
	linearWrap.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_STATIC_SAMPLER_DESC linearClamp{};
	linearClamp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	linearClamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	linearClamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	linearClamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	linearClamp.MipLODBias = 0;
	linearClamp.MaxAnisotropy = 16;
	linearClamp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	linearClamp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	linearClamp.MinLOD = 0.0f;
	linearClamp.MaxLOD = D3D12_FLOAT32_MAX;
	linearClamp.ShaderRegister = 3;
	linearClamp.RegisterSpace = 0;
	linearClamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_STATIC_SAMPLER_DESC anisotropicWrap{};
	anisotropicWrap.Filter = D3D12_FILTER_ANISOTROPIC;
	anisotropicWrap.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	anisotropicWrap.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	anisotropicWrap.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	anisotropicWrap.MipLODBias = 0;
	anisotropicWrap.MaxAnisotropy = 16;
	anisotropicWrap.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	anisotropicWrap.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	anisotropicWrap.MinLOD = 0.0f;
	anisotropicWrap.MaxLOD = D3D12_FLOAT32_MAX;
	anisotropicWrap.ShaderRegister = 4;
	anisotropicWrap.RegisterSpace = 0;
	anisotropicWrap.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_STATIC_SAMPLER_DESC anisotropicClamp{};
	anisotropicClamp.Filter = D3D12_FILTER_ANISOTROPIC;
	anisotropicClamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	anisotropicClamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	anisotropicClamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	anisotropicClamp.MipLODBias = 0;
	anisotropicClamp.MaxAnisotropy = 16;
	anisotropicClamp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	anisotropicClamp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	anisotropicClamp.MinLOD = 0.0f;
	anisotropicClamp.MaxLOD = D3D12_FLOAT32_MAX;
	anisotropicClamp.ShaderRegister = 5;
	anisotropicClamp.RegisterSpace = 0;
	anisotropicClamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_STATIC_SAMPLER_DESC imguiSampler = {};
	imguiSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	imguiSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	imguiSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	imguiSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	imguiSampler.MipLODBias = 0.0f;
	imguiSampler.MaxAnisotropy = 0;
	imguiSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	imguiSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	imguiSampler.MinLOD = 0.0f;
	imguiSampler.MaxLOD = 0.0f;
	imguiSampler.ShaderRegister = 6;
	imguiSampler.RegisterSpace = 0;
	imguiSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	m_staticSamplers[0] = pointWrap;
	m_staticSamplers[1] = pointClamp;
	m_staticSamplers[2] = linearWrap;
	m_staticSamplers[3] = linearClamp;
	m_staticSamplers[4] = anisotropicWrap;
	m_staticSamplers[5] = anisotropicClamp;
	m_staticSamplers[6] = imguiSampler;
}

void Renderer::SetVSync(const ParamVariant& p) noexcept
{
	m_vsyncInterval = p.GetBool() ? 1 : 0;

	if (m_vsyncInterval == 0 && m_deviceObjs.m_tearingSupport)
	{
		m_presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
		//CheckHR(m_deviceObjs.m_dxgiFactory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER));
	}
	else
		m_presentFlags = 0;
}