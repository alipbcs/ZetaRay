#pragma once

#include "../App/ZetaRay.h"
#include "../Win32/Win32.h"
#include <D3D12/1.610.3/d3d12.h>
#include <D3D12/1.610.3/dxgiformat.h>
#include <dxgi1_6.h>

#ifdef _DEBUG
#include <dxgidebug.h>
#endif

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#ifdef _DEBUG 
#ifndef CheckHR
#define CheckHR(x)											                	            \
    {																    	                \
	    HRESULT hr_ = (x);											    	                \
	    if (FAILED(hr_))												    	            \
	    {															           	            \
            char buff_[64];                                                                 \
            stbsp_snprintf(buff_, 256, "HRESULT: %x\n%s: %d", hr_, __FILE__,  __LINE__);    \
			MessageBoxA(nullptr, buff_, "Fatal Error", MB_ICONERROR | MB_OK);               \
		    __debugbreak();												                    \
	    }																                    \
    }
#endif
#else
#ifndef CheckHR
#define CheckHR(x)                                                                          \
    {                                                                                       \
	    HRESULT hr_ = (x);																	\
	    if (FAILED(hr_))																	\
	    {													                                \
            char buff_[64];                                                                 \
            stbsp_snprintf(buff_, 256, "HRESULT: %x\n%s: %d", hr_, __FILE__,  __LINE__);    \
			MessageBoxA(nullptr, buff_, "Fatal Error", MB_ICONERROR | MB_OK);               \
            exit(EXIT_FAILURE);                                                             \
	    }                                                                                   \
    }
#endif // DEBUGBREAK
#endif

#ifdef _DEBUG 
#ifndef SET_D3D_OBJ_NAME
#define SET_D3D_OBJ_NAME(pObj, str)																	\
	{																								\
		HRESULT hr = pObj->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(str), str);		\
		if(FAILED(hr))																				\
		{																							\
			char msg[128];	        																\
			FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM,												\
				nullptr,																			\
				hr,																					\
				MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),											\
				msg,																				\
				sizeof(msg),																		\
				nullptr);																			\
																									\
			char buff[256];																			\
            stbsp_snprintf(buff, 256, "HRESULT: %s \n%s: %d", msg, __FILE__,  __LINE__);			\
			MessageBoxA(nullptr, buff, "Fatal Error", MB_ICONERROR | MB_OK);						\
		    __debugbreak();																			\
		}																							\
	}
#endif
#else
#ifndef SET_D3D_OBJ_NAME
#define SET_D3D_OBJ_NAME(pObj, str)
#endif
#endif

namespace ZetaRay::Core
{
	class DeviceObjects
	{
	public:
		void InitializeAdapter() noexcept;
		void CreateDevice() noexcept;
		void CreateSwapChain(ID3D12CommandQueue* directQueue, HWND hwnd, int w, int h, int numBuffers,
			DXGI_FORMAT format, int maxLatency) noexcept;
		void ResizeSwapChain(int w, int h, int maxLatency) noexcept;

		ComPtr<IDXGIFactory6> m_dxgiFactory;
		ComPtr<IDXGIAdapter3> m_dxgiAdapter;
		ComPtr<ID3D12Device8> m_device;
		ComPtr<IDXGISwapChain3> m_dxgiSwapChain;

		bool m_tearingSupport = false;
		//UINT m_swapChainFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
		UINT m_swapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
		HANDLE m_frameLatencyWaitableObj;

		char m_deviceName[64] = { '\0' };
	};
}