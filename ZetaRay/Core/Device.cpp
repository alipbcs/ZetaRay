#include "../Utility/Error.h"
#include "Device.h"
#include "Constants.h"

using namespace ZetaRay;

// PIX crashes when debug layer is enabled
#define ENABLE_DEBUG_LAYER

void DeviceObjects::InitializeAdapter() noexcept
{
#if defined(_DEBUG) && defined(ENABLE_DEBUG_LAYER)
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
	CheckHR(m_dxgiFactory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&dxgiAdapter)));
	CheckHR(dxgiAdapter->QueryInterface(IID_PPV_ARGS(m_dxgiAdapter.GetAddressOf())));
	dxgiAdapter->Release();

	DXGI_ADAPTER_DESC2 desc;
	CheckHR(m_dxgiAdapter->GetDesc2(&desc));

	int size = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
	Assert(size < sizeof(m_deviceName), "buffer overflow");
	WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, m_deviceName, size, nullptr, nullptr);
}

void DeviceObjects::CreateDevice() noexcept
{
	ID3D12Device* device;

	CheckHR(D3D12CreateDevice(m_dxgiAdapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&device)));
	CheckHR(device->QueryInterface(IID_PPV_ARGS(m_device.GetAddressOf())));
	device->Release();

#if defined(_DEBUG) && defined(ENABLE_DEBUG_LAYER)
	ID3D12InfoQueue* infoQueue;
	CheckHR(m_device->QueryInterface(&infoQueue));

	infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
	infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
	//infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);

	D3D12_MESSAGE_ID filteredMsgs[] = 
	{ 
		//D3D12_MESSAGE_ID_LOADPIPELINE_NAMENOTFOUND,
		//D3D12_MESSAGE_ID_LOADPIPELINE_INVALIDDESC,
		D3D12_MESSAGE_ID_CREATEPIPELINELIBRARY_DRIVERVERSIONMISMATCH,
		//D3D12_MESSAGE_ID_CREATEPIPELINELIBRARY_INVALIDLIBRARYBLOB,
		D3D12_MESSAGE_ID_RESOLVE_QUERY_INVALID_QUERY_STATE	// TODO remove
	};

	D3D12_INFO_QUEUE_FILTER filter{};
	filter.DenyList.NumIDs = sizeof(filteredMsgs);
	filter.DenyList.pIDList = filteredMsgs;
	infoQueue->AddStorageFilterEntries(&filter);

	infoQueue->Release();
#endif // _DEBUG

	// check Hardware-accelerated RT support
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 feature;
	CheckHR(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &feature, sizeof(feature)));
	Check(feature.RaytracingTier == D3D12_RAYTRACING_TIER_1_1, "RaytracingTier 1.1 is not supported");

	// check shader model 6.6 support
	D3D12_FEATURE_DATA_SHADER_MODEL sm;
	sm.HighestShaderModel = D3D_SHADER_MODEL::D3D_SHADER_MODEL_6_6;
	CheckHR(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)));
	Check(sm.HighestShaderModel == D3D_SHADER_MODEL::D3D_SHADER_MODEL_6_6, "Shader Model 6.6 is not supported");

	// check tearing support
	CheckHR(m_dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &m_tearingSupport, sizeof(DXGI_FEATURE)));
	if (m_tearingSupport)
		m_swapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

	// fp16
	D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4;
	CheckHR(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4)));
	Check(options4.Native16BitShaderOpsSupported, "Native fp16 is not supported");
}

void DeviceObjects::CreateSwapChain(ID3D12CommandQueue* directQueue, HWND hwnd, int w, int h, int numBuffers,
	DXGI_FORMAT format, int maxLatency) noexcept
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
	//desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
	desc.Flags = m_swapChainFlags;

	IDXGISwapChain1* swapChain;
	CheckHR(m_dxgiFactory->CreateSwapChainForHwnd(directQueue, hwnd, &desc, nullptr, nullptr, &swapChain));
	CheckHR(swapChain->QueryInterface(IID_PPV_ARGS(m_dxgiSwapChain.GetAddressOf())));
	swapChain->Release();

	//CheckHR(m_dxgiSwapChain->SetMaximumFrameLatency(maxLatency));
}

void DeviceObjects::ResizeSwapChain(int w, int h, int maxLatency) noexcept
{
	CheckHR(m_dxgiSwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, m_swapChainFlags));
	//CheckHR(m_dxgiSwapChain->SetMaximumFrameLatency(maxLatency));
}
