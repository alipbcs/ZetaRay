#include "Device.h"
#include "Config.h"
#include "../App/Common.h"

// PIX crashes & NSight doesn't work when the debug layer is enabled
//#define DIREC3D_DEBUG_LAYER

// To ensure stable GPU frequency for performance testing. Requires developer mode to be enabled.
//#define STABLE_GPU_POWER_STATE

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::App;

void DeviceObjects::InitializeAdapter()
{
#if !defined(NDEBUG) && defined(DIREC3D_DEBUG_LAYER)
    {
        ComPtr<ID3D12Debug> debugController;
        CheckHR(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
        debugController->EnableDebugLayer();

        ComPtr<IDXGIInfoQueue> dxgiInfoQueue;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiInfoQueue))))
        {
            IDXGIFactory2* dxgiFactory2;
            CheckHR(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&dxgiFactory2)));
            CheckHR(dxgiFactory2->QueryInterface(IID_PPV_ARGS(m_dxgiFactory.GetAddressOf())));
            dxgiFactory2->Release();
        }

        //ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> pDredSettings;
        //CheckHR(D3D12GetDebugInterface(IID_PPV_ARGS(&pDredSettings)));

        // Turn on AutoBreadcrumbs and Page Fault reporting
        //pDredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        //pDredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
#else
    CheckHR(CreateDXGIFactory1(IID_PPV_ARGS(&m_dxgiFactory)));
#endif

    IDXGIAdapter* dxgiAdapter;
    CheckHR(m_dxgiFactory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, 
        IID_PPV_ARGS(&dxgiAdapter)));
    CheckHR(dxgiAdapter->QueryInterface(IID_PPV_ARGS(m_dxgiAdapter.GetAddressOf())));
    dxgiAdapter->Release();

    DXGI_ADAPTER_DESC2 desc;
    CheckHR(m_dxgiAdapter->GetDesc2(&desc));

    Common::WideToCharStr(desc.Description, m_deviceName);
}

void DeviceObjects::CreateDevice(bool checkFeatureSupport)
{
    ID3D12Device* device;

    CheckHR(D3D12CreateDevice(m_dxgiAdapter.Get(), D3D_FEATURE_LEVEL_12_2, 
        IID_PPV_ARGS(&device)));
    CheckHR(device->QueryInterface(IID_PPV_ARGS(m_device.GetAddressOf())));
    device->Release();

#if !defined(NDEBUG) && defined(DIREC3D_DEBUG_LAYER)
    ID3D12InfoQueue* infoQueue;
    CheckHR(m_device->QueryInterface(&infoQueue));

    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
    //infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);

    D3D12_MESSAGE_ID filteredMsgs[] = 
    { 
        D3D12_MESSAGE_ID_LOADPIPELINE_NAMENOTFOUND,
        //D3D12_MESSAGE_ID_LOADPIPELINE_INVALIDDESC,
        D3D12_MESSAGE_ID_CREATEPIPELINELIBRARY_DRIVERVERSIONMISMATCH,
        //D3D12_MESSAGE_ID_CREATEPIPELINELIBRARY_INVALIDLIBRARYBLOB,
        //D3D12_MESSAGE_ID_RESOLVE_QUERY_INVALID_QUERY_STATE,
        //D3D12_MESSAGE_ID_EXECUTECOMMANDLISTS_GPU_WRITTEN_READBACK_RESOURCE_MAPPED
    };

    D3D12_INFO_QUEUE_FILTER filter{};
    filter.DenyList.NumIDs = sizeof(filteredMsgs);
    filter.DenyList.pIDList = filteredMsgs;
    infoQueue->AddStorageFilterEntries(&filter);

    infoQueue->Release();

#endif

    if (!checkFeatureSupport)
        return;

#ifdef STABLE_GPU_POWER_STATE
    CheckHR(device->SetStablePowerState(true));
#endif

    // Hardware-accelerated RT
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 feature;
    CheckHR(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, 
        &feature, sizeof(feature)));
    Check(feature.RaytracingTier == D3D12_RAYTRACING_TIER_1_1, 
        "Raytracing Tier 1.1 is not supported.");

    // Shader model 6.6
    D3D12_FEATURE_DATA_SHADER_MODEL sm;
    sm.HighestShaderModel = D3D_SHADER_MODEL::D3D_SHADER_MODEL_6_6;
    CheckHR(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, 
        sizeof(sm)));
    Check(sm.HighestShaderModel == D3D_SHADER_MODEL::D3D_SHADER_MODEL_6_6, 
        "Shader Model 6.6 is not supported.");

    // Tearing
    CheckHR(m_dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, 
        &m_tearingSupport, sizeof(DXGI_FEATURE)));
    if (m_tearingSupport)
        m_swapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    // fp16
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4;
    CheckHR(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, 
        &options4, sizeof(options4)));
    Check(options4.Native16BitShaderOpsSupported, "Native fp16 is not supported.");

    // Wave intrinsics
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1;
    CheckHR(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, 
        &options1, sizeof(options1)));
    Check(options1.WaveOps, "Wave intrinsics are not supported.");
    Check(options1.WaveLaneCountMin >= 32, "Wave lane count of at least 32 is required.");

    // Enhanced barriers
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    CheckHR(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, 
        &options12, sizeof(options12)));
    Check(options12.EnhancedBarriersSupported, "Enhanced barriers are not supported.");

    // RGBE support
    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport{};
    formatSupport.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    CheckHR(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, 
        &formatSupport, sizeof(formatSupport)));

    if (formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW
        & D3D12_FORMAT_SUPPORT1_SHADER_LOAD)
        m_rgbeSupport = true;
}

void DeviceObjects::CreateSwapChain(ID3D12CommandQueue* directQueue, HWND hwnd, int w, 
    int h, int numBuffers, DXGI_FORMAT format, int maxLatency)
{
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = w;
    desc.Height = h;
    desc.Format = format;
    desc.Stereo = false;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = numBuffers;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = m_swapChainFlags;

    IDXGISwapChain1* swapChain;
    CheckHR(m_dxgiFactory->CreateSwapChainForHwnd(directQueue, hwnd, &desc, 
        nullptr, nullptr, &swapChain));
    CheckHR(swapChain->QueryInterface(IID_PPV_ARGS(m_dxgiSwapChain.GetAddressOf())));
    swapChain->Release();

    CheckHR(m_dxgiSwapChain->SetMaximumFrameLatency(maxLatency));
    m_frameLatencyWaitableObj = m_dxgiSwapChain->GetFrameLatencyWaitableObject();
}

void DeviceObjects::ResizeSwapChain(int w, int h, int maxLatency)
{
    CheckHR(m_dxgiSwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, m_swapChainFlags));
    //CheckHR(m_dxgiSwapChain->SetMaximumFrameLatency(maxLatency));
}
