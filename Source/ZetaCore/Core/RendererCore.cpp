#include "RendererCore.h"
#include "CommandList.h"
#include "Direct3DUtil.h"
#include "../Support/Task.h"
#include "../Support/Param.h"
#include "../App/Timer.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// RendererCore
//--------------------------------------------------------------------------------------

RendererCore::RendererCore()
    : m_cbvSrvUavDescHeapGpu(32),
    m_cbvSrvUavDescHeapCpu(32),
    m_rtvDescHeap(8),
    m_directQueue(D3D12_COMMAND_LIST_TYPE_DIRECT),
    m_computeQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE)
{}

RendererCore::~RendererCore()
{}

void RendererCore::Init(HWND hwnd, uint16_t renderWidth, uint16_t renderHeight, 
    uint16_t displayWidth, uint16_t displayHeight)
{
    m_hwnd = hwnd;

    m_deviceObjs.InitializeAdapter();
    m_deviceObjs.CreateDevice();
    InitStaticSamplers();

    CheckHR(m_deviceObjs.m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, 
        IID_PPV_ARGS(m_fence.GetAddressOf())));
    m_event = CreateEventA(nullptr, false, false, nullptr);
    CheckWin32(m_event);

    m_renderWidth = renderWidth;
    m_renderHeight = renderHeight;
    m_displayWidth = displayWidth;
    m_displayHeight = displayHeight;

    GpuMemory::Init();
    GpuMemory::BeginFrame();

    m_cbvSrvUavDescHeapGpu.Init(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        Constants::NUM_CBV_SRV_UAV_DESC_HEAP_GPU_DESCRIPTORS,
        true);
    m_cbvSrvUavDescHeapCpu.Init(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        Constants::NUM_CBV_SRV_UAV_DESC_HEAP_CPU_DESCRIPTORS,
        false);
    m_rtvDescHeap.Init(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        Constants::NUM_RTV_DESC_HEAP_DESCRIPTORS,
        false);
    //m_dsvDescHeap.Init(D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
    //    Constants::NUM_DSV_DESC_HEAP_DESCRIPTORS,
    //    false);

    m_directQueue.Init();
    m_computeQueue.Init();
    //m_copyQueue.Init();

    m_backbuffDescTable = m_rtvDescHeap.Allocate(Constants::NUM_BACK_BUFFERS);
    //m_depthBuffDescTable = m_dsvDescHeap.Allocate(1);

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

    m_gpuTimer.Init();

    ParamVariant p0;
    p0.InitBool("Renderer", "Display", "VSync", 
        fastdelegate::MakeDelegate(this, &RendererCore::SetVSync), m_vsyncInterval > 0);
    App::AddParam(p0);
}

void RendererCore::ResizeBackBuffers(HWND hwnd)
{
    // Ff back buffers already exist, resize them
    if (m_backBuffers[0].IsInitialized())
    {
        // GPU is flushed, no need to wait
        for (int i = 0; i < Constants::NUM_BACK_BUFFERS; i++)
        {
            // Don't check ref count for backbuffer COM object as it's 3 rather than 1:
            // "DXGI_SWAP_EFFECT_FLIP_DISCARD is valid for a swap chain with more than one back buffer; although 
            // applications have read and write access only to buffer 0"
            // Ref: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect
            m_backBuffers[i].Reset(false, false);
        }

        m_deviceObjs.ResizeSwapChain(m_displayWidth, m_displayHeight, 
            Constants::MAX_SWAPCHAIN_FRAME_LATENCY);
    }
    else
    {
        m_deviceObjs.CreateSwapChain(m_directQueue.GetCommandQueue(),
            hwnd,
            m_displayWidth, m_displayHeight,
            Constants::NUM_BACK_BUFFERS,
            Direct3DUtil::NoSRGB(Constants::BACK_BUFFER_FORMAT),
            Constants::MAX_SWAPCHAIN_FRAME_LATENCY);
    }

    m_currBackBuffIdx = (uint16_t)m_deviceObjs.m_dxgiSwapChain->GetCurrentBackBufferIndex();

    // Obtain the back buffers
    for (int i = 0; i < Constants::NUM_BACK_BUFFERS; i++)
    {
        ID3D12Resource* backbuff;
        CheckHR(m_deviceObjs.m_dxgiSwapChain->GetBuffer(i, IID_PPV_ARGS(&backbuff)));

        StackStr(buff, n, "Backbuffer_%d", i);
        m_backBuffers[i] = ZetaMove(Texture(buff, ZetaMove(backbuff), 
            RESOURCE_HEAP_TYPE::COMMITTED));
    }

    for (int i = 0; i < Constants::NUM_BACK_BUFFERS; i++)
    {
        D3D12_RENDER_TARGET_VIEW_DESC desc{};
        desc.Format = Constants::BACK_BUFFER_FORMAT;
        desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        m_deviceObjs.m_device->CreateRenderTargetView(m_backBuffers[i].Resource(), 
            &desc, m_backbuffDescTable.CPUHandle(i));
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

void RendererCore::Shutdown()
{
    if (!m_deviceObjs.m_tearingSupport)
    {
        // Ref: https://docs.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi
        // "You may not release a swap chain in full-screen mode because doing so may create thread contention"
        m_deviceObjs.m_dxgiSwapChain->SetFullscreenState(false, nullptr);
    }

    // GPU has been flushed, no need to sync
    for (int i = 0; i < Constants::NUM_BACK_BUFFERS; i++)
        m_backBuffers[i].Reset(false, false);

    // Make sure all GPU resources (texture, buffers, etc) are manually released,
    // as they normally call the GPU memory subsystem upon destruction, which
    // is deleted after this point.
    m_gpuTimer.Shutdown();

    GpuMemory::Shutdown();
}

void RendererCore::OnWindowSizeChanged(HWND hwnd, uint16_t renderWidth, uint16_t renderHeight, 
    uint16_t displayWidth, uint16_t displayHeight)
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

void RendererCore::WaitForSwapChainWaitableObject()
{
    // Blocks until eariliest queued present is completed
    WaitForSingleObject(m_deviceObjs.m_frameLatencyWaitableObj, 16);
}

void RendererCore::BeginFrame()
{
    if (App::GetTimer().GetTotalFrameCount() > 0)
        GpuMemory::BeginFrame();

    m_gpuTimer.BeginFrame();
}

void RendererCore::SubmitResourceCopies()
{
    GpuMemory::SubmitResourceCopies();

    App::AddFrameStat("Renderer", "RTV Desc. Heap", 
        m_rtvDescHeap.GetHeapSize() - m_rtvDescHeap.GetNumFreeDescriptors(), 
        m_rtvDescHeap.GetHeapSize());
    App::AddFrameStat("Renderer", "Gpu Desc. Heap", 
        m_cbvSrvUavDescHeapGpu.GetHeapSize() - m_cbvSrvUavDescHeapGpu.GetNumFreeDescriptors(), 
        m_cbvSrvUavDescHeapGpu.GetHeapSize());
}

void RendererCore::EndFrame(TaskSet& endFrameTS)
{
    auto h0 = endFrameTS.EmplaceTask("Present", [this]()
        {
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

                CheckHR(hr);
            }

            // Schedule a Signal command in the queue.
            // Set the fence value for the next frame.
            m_fenceVals[m_currBackBuffIdx] = m_nextFenceVal;
            CheckHR(m_directQueue.GetCommandQueue()->Signal(m_fence.Get(), m_nextFenceVal++));

            // Update the back buffer index.
            const uint16_t nextBackBuffidx = (uint16_t)m_deviceObjs.m_dxgiSwapChain->GetCurrentBackBufferIndex();
            const uint64_t completed = m_fence->GetCompletedValue();

            if (completed < m_fenceVals[nextBackBuffidx])
            {
                CheckHR(m_fence->SetEventOnCompletion(m_fenceVals[nextBackBuffidx], m_event));
                //uint64_t f = App::GetTimer().GetTotalFrameCount();
                //printf("Frame %llu, CPU waiting for GPU...\n", f);
                WaitForSingleObject(m_event, INFINITE);
            }

            m_currBackBuffIdx = nextBackBuffidx;
            m_globalDoubleBuffIdx = (m_globalDoubleBuffIdx + 1) & 0x1;
        });

    auto h1 = endFrameTS.EmplaceTask("RecycleGpuMem", []()
        {
            GpuMemory::Recycle();
        });

    auto h2 = endFrameTS.EmplaceTask("RecycleDescHeaps", [this]()
        {
            m_cbvSrvUavDescHeapGpu.Recycle();
            m_cbvSrvUavDescHeapCpu.Recycle();
            m_rtvDescHeap.Recycle();
            //m_dsvDescHeap.Recycle();
        });
}

DXGI_OUTPUT_DESC RendererCore::GetOutputMonitorDesc() const
{
    ComPtr<IDXGIOutput> pOutput;
    CheckHR(m_deviceObjs.m_dxgiSwapChain->GetContainingOutput(&pOutput));

    DXGI_OUTPUT_DESC desc;
    CheckHR(pOutput->GetDesc(&desc));

    return desc;
}

uint64_t RendererCore::GetCommandQueueTimeStampFrequency(D3D12_COMMAND_LIST_TYPE t) const
{
    uint64_t freq = UINT64_MAX;

    switch (t)
    {
    case D3D12_COMMAND_LIST_TYPE_DIRECT:
        CheckHR(m_directQueue.m_cmdQueue->GetTimestampFrequency(&freq));
        break;
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
        CheckHR(m_computeQueue.m_cmdQueue->GetTimestampFrequency(&freq));
        break;
    default:
        break;
    }

    return freq;
}

GraphicsCmdList* RendererCore::GetGraphicsCmdList()
{
    CommandList* ctx = m_directQueue.GetCommandList();
    Assert(ctx->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast.");

    return static_cast<GraphicsCmdList*>(ctx);
}

ComputeCmdList* RendererCore::GetComputeCmdList()
{
    CommandList* ctx = m_computeQueue.GetCommandList();
    Assert(ctx->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast.");

    return static_cast<ComputeCmdList*>(ctx);
}

// There is another "CopyContext" defined in WinBase.h!
//CopyCmdList* RendererCore::GetCopyCmdList()
//{
//    auto* ctx = m_copyQueue->GetCommandList();
//    Assert(ctx->CommandListType() == D3D12_COMMAND_LIST_TYPE_COPY, "Invalid downcast.");
//
//    return static_cast<ZetaRay::CopyCmdList*>(ctx);
//}

void RendererCore::ReleaseCmdList(CommandList* ctx)
{
    if (ctx->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
        m_directQueue.ReleaseCommandList(ctx);    
    else if (ctx->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE)
        m_computeQueue.ReleaseCommandList(ctx);
}

uint64_t RendererCore::ExecuteCmdList(CommandList* ctx)
{
    if (ctx->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
        return m_directQueue.ExecuteCommandList(ctx);
    else if (ctx->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE)
        return m_computeQueue.ExecuteCommandList(ctx);

    return UINT64_MAX;
}

void RendererCore::SignalDirectQueue(ID3D12Fence* f, uint64_t v)
{
    auto* cmdQueue = m_directQueue.GetCommandQueue();
    Assert(cmdQueue, "cmdQueue was NULL");
    cmdQueue->Signal(f, v);
}

void RendererCore::SignalComputeQueue(ID3D12Fence* f, uint64_t v)
{
    m_computeQueue.GetCommandQueue()->Signal(f, v);
}

bool RendererCore::IsDirectQueueFenceComplete(uint64_t fenceValue)
{
    return m_directQueue.IsFenceComplete(fenceValue);
}

bool RendererCore::IsComputeQueueFenceComplete(uint64_t fenceValue)
{
    return m_computeQueue.IsFenceComplete(fenceValue);
}

void RendererCore::WaitForDirectQueueFenceCPU(uint64_t fenceValue)
{
    m_directQueue.WaitForFenceCPU(fenceValue);
}

void RendererCore::WaitForDirectQueueFenceCPU2(uint64_t fenceValue, HANDLE e)
{
    if (m_directQueue.IsFenceComplete(fenceValue))
        return;

    CheckHR(m_directQueue.m_fence->SetEventOnCompletion(fenceValue, e));
    WaitForSingleObject(e, INFINITE);
}

void RendererCore::WaitForComputeQueueFenceCPU(uint64_t fenceValue)
{
    m_computeQueue.WaitForFenceCPU(fenceValue);
}

void RendererCore::WaitForDirectQueueOnComputeQueue(uint64_t v)
{
    // MS Docs:
    // "Queues a GPU-side wait, and returns immediately. A GPU-side wait is where 
    // the GPU waits until the specified fence reaches or exceeds the specified value."
    //
    // command queue waits (during that time no work is executed) until the fence 
    // reaches the requested value
    CheckHR(m_computeQueue.m_cmdQueue->Wait(m_directQueue.m_fence.Get(), v));
}

void RendererCore::WaitForComputeQueueOnDirectQueue(uint64_t v)
{
    CheckHR(m_directQueue.m_cmdQueue->Wait(m_computeQueue.m_fence.Get(), v));
}

void RendererCore::FlushAllCommandQueues()
{
    m_directQueue.WaitForIdle();
    m_computeQueue.WaitForIdle();
}

void RendererCore::InitStaticSamplers()
{
    D3D12_STATIC_SAMPLER_DESC mip0;
    mip0.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    mip0.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    mip0.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    mip0.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    mip0.MipLODBias = 0;
    mip0.MaxAnisotropy = 0;
    mip0.ComparisonFunc = D3D12_COMPARISON_FUNC_NONE;
    mip0.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    mip0.MinLOD = 0.0f;
    mip0.MaxLOD = 0.0f;
    mip0.ShaderRegister = 0;
    mip0.RegisterSpace = 0;
    mip0.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC pointWrap;
    pointWrap.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    pointWrap.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    pointWrap.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    pointWrap.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    pointWrap.MipLODBias = 0;
    pointWrap.MaxAnisotropy = 0;
    pointWrap.ComparisonFunc = D3D12_COMPARISON_FUNC_NONE;
    pointWrap.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    pointWrap.MinLOD = 0.0f;
    pointWrap.MaxLOD = D3D12_FLOAT32_MAX;
    pointWrap.ShaderRegister = 1;
    pointWrap.RegisterSpace = 0;
    pointWrap.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC pointClamp = pointWrap;
    pointClamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pointClamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pointClamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pointClamp.ShaderRegister = 2;

    D3D12_STATIC_SAMPLER_DESC linearWrap;
    linearWrap.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearWrap.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    linearWrap.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    linearWrap.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    linearWrap.MipLODBias = 0;
    linearWrap.MaxAnisotropy = 0;
    linearWrap.ComparisonFunc = D3D12_COMPARISON_FUNC_NONE;
    linearWrap.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    linearWrap.MinLOD = 0.0f;
    linearWrap.MaxLOD = D3D12_FLOAT32_MAX;
    linearWrap.ShaderRegister = 3;
    linearWrap.RegisterSpace = 0;
    linearWrap.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC linearClamp = linearWrap;
    linearClamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClamp.ShaderRegister = 4;

    D3D12_STATIC_SAMPLER_DESC anisotropicWrap;
    anisotropicWrap.Filter = D3D12_FILTER_ANISOTROPIC;
    anisotropicWrap.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    anisotropicWrap.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    anisotropicWrap.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    anisotropicWrap.MipLODBias = 0;
    anisotropicWrap.MaxAnisotropy = 16;
    anisotropicWrap.ComparisonFunc = D3D12_COMPARISON_FUNC_NONE;
    anisotropicWrap.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    anisotropicWrap.MinLOD = 0.0f;
    anisotropicWrap.MaxLOD = D3D12_FLOAT32_MAX;
    anisotropicWrap.ShaderRegister = 5;
    anisotropicWrap.RegisterSpace = 0;
    anisotropicWrap.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC anisotropicWrap2x = anisotropicWrap;
    anisotropicWrap2x.MaxAnisotropy = 2;
    anisotropicWrap2x.ShaderRegister = 6;

    D3D12_STATIC_SAMPLER_DESC anisotropicWrap4x = anisotropicWrap;
    anisotropicWrap4x.MaxAnisotropy = 4;
    anisotropicWrap4x.ShaderRegister = 7;

    D3D12_STATIC_SAMPLER_DESC imguiSampler;
    imguiSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    imguiSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    imguiSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    imguiSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    imguiSampler.MipLODBias = 0.0f;
    imguiSampler.MaxAnisotropy = 0;
    imguiSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NONE;
    imguiSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    imguiSampler.MinLOD = 0.0f;
    imguiSampler.MaxLOD = 0.0f;
    imguiSampler.ShaderRegister = 8;
    imguiSampler.RegisterSpace = 0;
    imguiSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    m_staticSamplers[0] = mip0;
    m_staticSamplers[1] = pointWrap;
    m_staticSamplers[2] = pointClamp;
    m_staticSamplers[3] = linearWrap;
    m_staticSamplers[4] = linearClamp;
    m_staticSamplers[5] = anisotropicWrap;
    m_staticSamplers[6] = anisotropicWrap2x;
    m_staticSamplers[7] = anisotropicWrap4x;
    m_staticSamplers[8] = imguiSampler;

    D3D12_DESCRIPTOR_HEAP_DESC desc;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    desc.NumDescriptors = ZetaArrayLen(m_staticSamplers);
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NodeMask = 0;

    auto* device = App::GetRenderer().GetDevice();
    CheckHR(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_samplerDescHeap.GetAddressOf())));

    const auto descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    const auto baseCpuHandle = m_samplerDescHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SAMPLER_DESC samplerDescs[ZetaArrayLen(m_staticSamplers)];
    for (int i = 0; i < ZetaArrayLen(m_staticSamplers); i++)
    {
        samplerDescs[i].Filter = m_staticSamplers[i].Filter;
        samplerDescs[i].AddressU = m_staticSamplers[i].AddressU;
        samplerDescs[i].AddressV = m_staticSamplers[i].AddressV;
        samplerDescs[i].AddressW = m_staticSamplers[i].AddressW;
        samplerDescs[i].MipLODBias = m_staticSamplers[i].MipLODBias;
        samplerDescs[i].MaxAnisotropy = m_staticSamplers[i].MaxAnisotropy;
        samplerDescs[i].ComparisonFunc = m_staticSamplers[i].ComparisonFunc;
        samplerDescs[i].MinLOD = m_staticSamplers[i].MinLOD;
        samplerDescs[i].MaxLOD = m_staticSamplers[i].MaxLOD;
        samplerDescs[i].BorderColor[0] = 0;
        samplerDescs[i].BorderColor[1] = 0;
        samplerDescs[i].BorderColor[2] = 0;
        samplerDescs[i].BorderColor[3] = 0;
        
        D3D12_CPU_DESCRIPTOR_HANDLE handle{
            .ptr = baseCpuHandle.ptr + i * descSize
        };
        device->CreateSampler(&samplerDescs[i], handle);
    }
}

void RendererCore::SetVSync(const ParamVariant& p)
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