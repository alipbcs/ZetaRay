#include "App.h"
#include "../Utility/SynchronizedView.h"
#include "Timer.h"
#include "../SupportSystem/MemoryPool.h"
#include "../SupportSystem/Param.h"
#include "../SupportSystem/Stat.h"
#include "../Utility/HashTable.h"
#include "../Utility/Span.h"
#include "../Core/Renderer.h"
#include "../Core/CommandQueue.h"			// just for std::unique_ptr<>
#include "../Core/SharedShaderResources.h"	// just for std::unique_ptr<>
#include "../Scene/Scene.h"
#include "../SupportSystem/ThreadPool.h"
#include "../Utility/RNG.h"
#include "../../Assets/Fonts/SegoeUI.h"
#include <atomic>

//#define STB_SPRINTF_IMPLEMENTATION
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION 
#include <xxHash-0.8.0/xxhash.h>
#include <imgui/imgui.h>
#include <imgui/implot.h>
#include <imgui/imnodes.h>

namespace
{
	struct FrameTime
	{
		static constexpr int HIST_LEN = 60;
		double FrameTimeHist[HIST_LEN] = { 0.0 };
		int NextFramHistIdx = 0;
	};	
}

namespace ZetaRay
{
	struct AppData
	{
		//static const int INITIAL_WINDOW_WIDTH = 1024;
		//static const int INITIAL_WINDOW_HEIGHT = 576;
		static const int INITIAL_WINDOW_WIDTH = 1536;
		static const int INITIAL_WINDOW_HEIGHT = 864;
#if defined(_DEBUG)
		inline static const char* PSO_CACHE_DIR = "Assets\\PsoCache\\Debug";
		inline static const char* COMPILED_SHADER_DIR = "Assets\\CompiledShaders\\Debug";
#else
		inline static const char* PSO_CACHE_DIR = "Assets\\PsoCache\\Release";
		inline static const char* COMPILED_SHADER_DIR = "Assets\\CompiledShaders\\Release";
#endif // _DEBUG

		inline static const char* ASSET_DIR = "Assets";
		inline static const char* TOOLS_DIR = "Tools";
		inline static const char* DXC_PATH = "Tools\\dxc\\dxc.exe";
		inline static const char* RENDER_PASS_DIR = "ZetaRay\\RenderPass";
		static constexpr int NUM_BACKGROUND_THREADS = 2;
		static constexpr int MAX_NUM_TASKS_PER_FRAME = 256;

		int m_processorCoreCount = 0;
		HWND m_hwnd;
		RECT m_wndRectCache;
		int m_displayWidth;
		int m_displayHeight;
		bool m_isActive = true;
		int m_lastMousePosX = 0;
		int m_lastMousePosY = 0;
		bool m_inSizeMove = false;
		bool m_minimized = false;
		bool m_isFullScreen = false;
		ImGuiMouseCursor m_imguiCursor = ImGuiMouseCursor_COUNT;
		bool m_imguiMouseTracked = false;
		uint32_t m_dpi;
		float m_upscaleFactor = 1.0f;
		float m_cameraMoveSpeed = 0.1f;
		float m_cameraZoomSpeed = 0.005f;

		Win32::Timer m_timer;
		Renderer m_renderer;
		ThreadPool m_mainThreadPool;
		ThreadPool m_backgroundThreadPool;
		Scene m_scene;

		struct alignas(64) ThreadContext
		{
			MemoryPool MemPool;
			SRWLOCK Lock;
			RNG Rng;
		};

		ThreadContext m_threadContexts[MAX_NUM_THREADS];
		//std::thread::id m_threadIDs[MAX_NUM_THREADS];
		uint32_t m_threadIDs[MAX_NUM_THREADS];

		SmallVector<ParamVariant> m_params;
		SmallVector<ShaderReloadHandler> m_shaderReloadHandlers;
		SmallVector<Stat> m_frameStats;
		FrameTime m_frameTime;

		//std::shared_mutex m_stdOutMtx;
		SRWLOCK m_stdOutLock = SRWLOCK_INIT;
		SRWLOCK m_paramLock = SRWLOCK_INIT;
		SRWLOCK m_shaderReloadLock = SRWLOCK_INIT;
		SRWLOCK m_statsLock = SRWLOCK_INIT;

		struct alignas(64) TaskSignal
		{
			std::atomic_int32_t Indegree;
			std::atomic_bool BlockFlag;
		};

		TaskSignal m_registeredTasks[MAX_NUM_TASKS_PER_FRAME];
		std::atomic_int32_t m_currTaskSignalIdx = 0;

		bool m_isInitialized = false;
	};

	static ZetaRay::AppData* g_pApp = nullptr;
}

namespace ZetaRay::AppImpl
{
	void ImGuiUpdateMouseCursor() noexcept
	{
		ImGuiIO& io = ImGui::GetIO();
		if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
			return;

		ImGuiMouseCursor imgui_cursor = ImGui::GetMouseCursor();
		if (imgui_cursor == ImGuiMouseCursor_None || io.MouseDrawCursor)
		{
			// Hide OS mouse cursor if imgui is drawing it or if it wants no cursor
			SetCursor(NULL);
		}
		else
		{
			// Show OS mouse cursor
			LPTSTR win32_cursor = IDC_ARROW;
			switch (imgui_cursor)
			{
			case ImGuiMouseCursor_Arrow:        win32_cursor = IDC_ARROW; break;
			case ImGuiMouseCursor_TextInput:    win32_cursor = IDC_IBEAM; break;
			case ImGuiMouseCursor_ResizeAll:    win32_cursor = IDC_SIZEALL; break;
			case ImGuiMouseCursor_ResizeEW:     win32_cursor = IDC_SIZEWE; break;
			case ImGuiMouseCursor_ResizeNS:     win32_cursor = IDC_SIZENS; break;
			case ImGuiMouseCursor_ResizeNESW:   win32_cursor = IDC_SIZENESW; break;
			case ImGuiMouseCursor_ResizeNWSE:   win32_cursor = IDC_SIZENWSE; break;
			case ImGuiMouseCursor_Hand:         win32_cursor = IDC_HAND; break;
			case ImGuiMouseCursor_NotAllowed:   win32_cursor = IDC_NO; break;
			}

			SetCursor(::LoadCursor(NULL, win32_cursor));
		}
	}

	void ImGuiUpdateMouse() noexcept
	{
		ImGuiIO& io = ImGui::GetIO();

		const ImVec2 mouse_pos_prev = io.MousePos;
		io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);

		// Obtain focused and hovered window. We forward mouse input when focused or when hovered (and no other window is capturing)
		HWND focused_window = GetForegroundWindow();
		HWND hovered_window = g_pApp->m_hwnd;
		HWND mouse_window = NULL;
		if (hovered_window && (hovered_window == g_pApp->m_hwnd || IsChild(hovered_window, g_pApp->m_hwnd)))
			mouse_window = hovered_window;
		else if (focused_window && (focused_window == g_pApp->m_hwnd || IsChild(focused_window, g_pApp->m_hwnd)))
			mouse_window = focused_window;
		if (mouse_window == NULL)
			return;

		// Set OS mouse position from Dear ImGui if requested (rarely used, only when ImGuiConfigFlags_NavEnableSetMousePos is enabled by user)
		if (io.WantSetMousePos)
		{
			POINT pos = { (int)mouse_pos_prev.x, (int)mouse_pos_prev.y };
			if (ClientToScreen(g_pApp->m_hwnd, &pos))
				SetCursorPos(pos.x, pos.y);
		}

		// Set Dear ImGui mouse position from OS position
		POINT pos;
		if (GetCursorPos(&pos) && ScreenToClient(mouse_window, &pos))
			io.MousePos = ImVec2((float)pos.x, (float)pos.y);

		// Update OS mouse cursor with the cursor requested by imgui
		ImGuiMouseCursor mouse_cursor = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
		if (g_pApp->m_imguiCursor != mouse_cursor)
		{
			g_pApp->m_imguiCursor = mouse_cursor;
			ImGuiUpdateMouseCursor();
		}
	}

	void InitImGui() noexcept
	{
		ImGui::CreateContext();
		ImPlot::CreateContext();
		ImNodes::CreateContext();

		ImGui::StyleColorsDark();

		ImGuiStyle& style = ImGui::GetStyle();
		ImVec4* colors = style.Colors;

		colors[ImGuiCol_WindowBg] = ImVec4(1.0f / 255, 1.0f / 255, 1.1f / 255, 0.6f);
		//colors[ImGuiCol_TitleBgActive] = (ImVec4)ImColor::HSV(5 / 7.0f, 0.7f, 0.5f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(245 / 255.0f, 20 / 255.0f, 20 / 255.0f, 1.0f);
		colors[ImGuiCol_TabActive] = ImVec4(8 / 255.0f, 47 / 255.0f, 144 / 255.0f, 1.0f);
		colors[ImGuiCol_Tab] = ImVec4(7 / 255.0f, 14 / 255.0f, 24 / 255.0f, 1.0f);
		colors[ImGuiCol_FrameBg] = ImVec4(6 / 255.0f, 14 / 255.0f, 6 / 255.0f, 1.0f);
		//colors[ImGuiCol_TitleBgActive] = ImVec4(1.0f, 64 / 255.0f, 150 / 255.0f, 0.98f);

		style.ScaleAllSizes((float)g_pApp->m_dpi / 96.0f);
		style.FramePadding = ImVec2(7.0f, 3.0f);
		style.GrabMinSize = 13.0f;
		style.FrameRounding = 12.0f;
		style.GrabRounding = style.FrameRounding;
		style.ItemSpacing = ImVec2(8.0f, 7.0f);

		ImGuiIO& io = ImGui::GetIO();
		//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
		io.Fonts->AddFontFromMemoryCompressedBase85TTF(SegoeUI_compressed_data_base85, 17.0f);
	}

	void UpdateStats() noexcept
	{
		g_pApp->m_frameStats.clear();

		const double frameTimeMs = g_pApp->m_timer.GetElapsedTime() * 1000.0;
		
		auto& frameStats = g_pApp->m_frameTime;
		frameStats.NextFramHistIdx = (frameStats.NextFramHistIdx < 59) ? frameStats.NextFramHistIdx + 1 : frameStats.NextFramHistIdx;
		Assert(frameStats.NextFramHistIdx >= 0 && frameStats.NextFramHistIdx < 60, "bug");

		// shift left
		double temp[FrameTime::HIST_LEN];
		memcpy(temp, frameStats.FrameTimeHist + 1, sizeof(double) * (FrameTime::HIST_LEN - 1));
		memcpy(frameStats.FrameTimeHist, temp, sizeof(double) * (FrameTime::HIST_LEN - 1));
		frameStats.FrameTimeHist[frameStats.NextFramHistIdx] = frameTimeMs;

		// compute moving average
		/*
		double movingAvg = 0.0f;
		for (int i = 0; i <= frameStats.NextFramHistIdx; i++)
			movingAvg += frameStats.FrameTimeHist[i];

		constexpr double oneDivSize = 1.0 / 60.0;
		movingAvg *= oneDivSize;
		*/

		DXGI_QUERY_VIDEO_MEMORY_INFO memoryInfo = {};
		CheckHR(g_pApp->m_renderer.GetAdapter()->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryInfo));

		//g_pApp->m_frameStats.emplace_back("Frame", "#Frames", g_pApp->m_timer.GetTotalFrameCount());
		g_pApp->m_frameStats.emplace_back("Frame", "FrameTime", (float)frameTimeMs);
		//g_pApp->m_frameStats.emplace_back("Frame", "FrameTime Avg.", (float) movingAvg);
		g_pApp->m_frameStats.emplace_back("Frame", "FPS", g_pApp->m_timer.GetFramesPerSecond());
		g_pApp->m_frameStats.emplace_back("GPU", "VRam Usage (MB)", memoryInfo.CurrentUsage >> 20);

		/*
		printf("----------------------\n");

		for (int i = 0; i < g_pApp->m_processorCoreCount; i++)
		{
			printf("Thread %d: %llu kb\n", i, g_pApp->m_threadContexts[i].MemPool.TotalSize() / 1024);
		}

		printf("----------------------\n\n");
		*/
	}

	void Update(TaskSet& sceneTS, TaskSet& sceneRendererTS) noexcept
	{
		UpdateStats();

		ImGuiUpdateMouse();
		ImGui::NewFrame();

		g_pApp->m_scene.Update(g_pApp->m_timer.GetElapsedTime(), sceneTS, sceneRendererTS);
	}

	void OnActivated() noexcept
	{
		g_pApp->m_timer.Resume();
		g_pApp->m_isActive = true;
	}

	void OnDeactivated() noexcept
	{
		g_pApp->m_timer.Pause();
		g_pApp->m_isActive = false;
	}

	void OnWindowSizeChanged() noexcept
	{
		if (g_pApp->m_timer.GetTotalFrameCount() > 0)
		{
			RECT rect;
			GetClientRect(g_pApp->m_hwnd, &rect);

			//int newWidth = (int)((rect.right - rect.left) * 96.0f / m_dpi);
			//int newHeight = (int)((rect.bottom - rect.top) * 96.0f / m_dpi);
			int newWidth = rect.right - rect.left;
			int newHeight = rect.bottom - rect.top;

			if (newWidth == g_pApp->m_displayWidth && newHeight == g_pApp->m_displayHeight)
				return;

			g_pApp->m_displayWidth = newWidth;
			g_pApp->m_displayHeight = newHeight;

			const float renderWidth = g_pApp->m_displayWidth / g_pApp->m_upscaleFactor;
			const float renderHeight = g_pApp->m_displayHeight / g_pApp->m_upscaleFactor;

			// following order is important
			g_pApp->m_renderer.OnWindowSizeChanged(g_pApp->m_hwnd, (int)renderWidth, (int)renderHeight, 
				g_pApp->m_displayWidth, g_pApp->m_displayHeight);
			g_pApp->m_scene.OnWindowSizeChanged();

			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2((float)g_pApp->m_displayWidth, (float)g_pApp->m_displayHeight);
		}
	}

	void OnToggleFullscreenWindow() noexcept
	{
		// switch from windowed to full-screen
		if (!g_pApp->m_isFullScreen)
		{
			GetWindowRect(g_pApp->m_hwnd, &g_pApp->m_wndRectCache);

			// Make the window borderless so that the client area can fill the screen.
			SetWindowLong(g_pApp->m_hwnd, GWL_STYLE, WS_OVERLAPPED & ~(WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU | WS_THICKFRAME));

			RECT fullscreenWindowRect;

			// Get the settings of the display on which the app's window is currently displayed
			DXGI_OUTPUT_DESC desc = g_pApp->m_renderer.GetOutputMonitorDesc();
			fullscreenWindowRect = desc.DesktopCoordinates;

			SetWindowPos(g_pApp->m_hwnd,
				HWND_NOTOPMOST,
				fullscreenWindowRect.left,
				fullscreenWindowRect.top,
				fullscreenWindowRect.right,
				fullscreenWindowRect.bottom,
				SWP_FRAMECHANGED | SWP_NOACTIVATE);

			ShowWindow(g_pApp->m_hwnd, SW_MAXIMIZE);
		}
		else
		{
			// Restore the window's attributes and size.
			SetWindowLong(g_pApp->m_hwnd, GWL_STYLE, WS_OVERLAPPED);

			SetWindowPos(
				g_pApp->m_hwnd,
				HWND_NOTOPMOST,
				g_pApp->m_wndRectCache.left,
				g_pApp->m_wndRectCache.top,
				g_pApp->m_wndRectCache.right - g_pApp->m_wndRectCache.left,
				g_pApp->m_wndRectCache.bottom - g_pApp->m_wndRectCache.top,
				SWP_FRAMECHANGED | SWP_NOACTIVATE);

			ShowWindow(g_pApp->m_hwnd, SW_NORMAL);
		}

		printf("g_pApp->m_isFullScreen was: %d\n", g_pApp->m_isFullScreen);
		g_pApp->m_isFullScreen = !g_pApp->m_isFullScreen;
	}

	void OnKeyboard(UINT message, WPARAM vkKey, LPARAM lParam) noexcept
	{
		if (ImGui::GetCurrentContext() == nullptr)
			return;

		ImGuiIO& io = ImGui::GetIO();

		const bool down = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);

		if (vkKey < 256)
			io.KeysDown[vkKey] = down;

		if (vkKey == VK_CONTROL)
		{
			io.KeysDown[VK_LCONTROL] = ((GetKeyState(VK_LCONTROL) & 0x8000) != 0);
			io.KeysDown[VK_RCONTROL] = ((GetKeyState(VK_RCONTROL) & 0x8000) != 0);
			io.KeyCtrl = io.KeysDown[VK_LCONTROL] || io.KeysDown[VK_RCONTROL];
		}
		if (vkKey == VK_SHIFT)
		{
			io.KeysDown[VK_LSHIFT] = ((GetKeyState(VK_LSHIFT) & 0x8000) != 0);
			io.KeysDown[VK_RSHIFT] = ((GetKeyState(VK_RSHIFT) & 0x8000) != 0);
			io.KeyShift = io.KeysDown[VK_LSHIFT] || io.KeysDown[VK_RSHIFT];
		}

		// following triggers an assertion in imgui.cpp when alt+enter is pressed
		//if (vkKey == VK_MENU)
		//{
		//	io.KeysDown[VK_LMENU] = ((GetKeyState(VK_LMENU) & 0x8000) != 0);
		//	io.KeysDown[VK_RMENU] = ((GetKeyState(VK_RMENU) & 0x8000) != 0);
		//	io.KeyAlt = io.KeysDown[VK_LMENU] || io.KeysDown[VK_RMENU];
		//}

		//io.AddInputCharacterUTF16((unsigned short)vkKey);

		if (!io.WantCaptureKeyboard)
		{
			switch (vkKey)
			{
				//A
			case 0x41:
				g_pApp->m_scene.GetCamera().MoveX(-g_pApp->m_cameraMoveSpeed);
				return;

				//D
			case 0x44:
				g_pApp->m_scene.GetCamera().MoveX(g_pApp->m_cameraMoveSpeed);
				return;

				//W
			case 0x57:
				g_pApp->m_scene.GetCamera().MoveZ(g_pApp->m_cameraMoveSpeed);
				return;

				//S
			case 0x53:
				g_pApp->m_scene.GetCamera().MoveZ(-g_pApp->m_cameraMoveSpeed);
				return;

				//Q
			case 0x51:
				g_pApp->m_scene.GetCamera().RotateY(-1.0f);
				return;

				//E
			case 0x45:
				g_pApp->m_scene.GetCamera().RotateY(1.0f);
				return;

				//Z
			case 0x5A:
				g_pApp->m_scene.GetCamera().RotateX(1.0f);
				return;

				//C
			case 0x43:
				g_pApp->m_scene.GetCamera().RotateX(-1.0f);
				return;
			}

			// ALT+ENTER:
			/*
			auto& renderer = g_pApp->m_renderer;
			bool listenToAltEnter = renderer.IsTearingSupported() && renderer.GetVSyncInterval() == 0;

			if (listenToAltEnter && (vkKey == VK_RETURN) && (lParam & (1 << 29)))
			{
				printf("lparam: %lld\n", lParam & 0xffff);
				//OnToggleFullscreenWindow();
				return;
			}
			*/
		}
	}

	void OnMouseDown(UINT message, WPARAM btnState, LPARAM lParam) noexcept
	{
		if (ImGui::GetCurrentContext() == nullptr)
			return;

		int button = 0;
		if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK)
		{
			button = 0;
		}
		else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK)
		{
			button = 1;
		}
		else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK)
		{
			button = 2;
		}

		if (!ImGui::IsAnyMouseDown() && GetCapture() == NULL)
			SetCapture(g_pApp->m_hwnd);

		ImGuiIO& io = ImGui::GetIO();
		//io.MouseDown[button] = true;
		io.AddMouseButtonEvent(button, true);

		if (!io.WantCaptureMouse)
		{
			int x = GET_X_LPARAM(lParam);
			int y = GET_Y_LPARAM(lParam);

			if (btnState == MK_LBUTTON)
			{
				SetCapture(g_pApp->m_hwnd);
				g_pApp->m_lastMousePosX = x;
				g_pApp->m_lastMousePosY = y;
			}
		}
	}

	void OnMouseUp(UINT message, WPARAM btnState, LPARAM lParam) noexcept
	{
		if (ImGui::GetCurrentContext() == nullptr)
			return;

		ImGuiIO& io = ImGui::GetIO();

		int button = 0;
		if (message == WM_LBUTTONUP)
		{
			button = 0;
		}
		else if (message == WM_RBUTTONUP)
		{
			button = 1;
		}
		else if (message == WM_MBUTTONUP)
		{
			button = 2;
		}

		io.MouseDown[button] = false;

		if (!ImGui::IsAnyMouseDown() && GetCapture() == g_pApp->m_hwnd)
			ReleaseCapture();

		io.AddMouseButtonEvent(button, false);

		if (!io.WantCaptureMouse)
		{
			if (message == WM_LBUTTONUP)
				ReleaseCapture();
		}
	}

	void OnMouseMove(UINT message, WPARAM btnState, LPARAM lParam) noexcept
	{
		if (ImGui::GetCurrentContext() == nullptr)
			return;

		ImGuiIO& io = ImGui::GetIO();

		// We need to call TrackMouseEvent in order to receive WM_MOUSELEAVE events
		if (!g_pApp->m_imguiMouseTracked)
		{
			TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, g_pApp->m_hwnd, 0 };
			TrackMouseEvent(&tme);
			g_pApp->m_imguiMouseTracked = true;
		}

		io.AddMousePosEvent((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));

		if (!io.WantCaptureMouse)
		{
			if (btnState == MK_LBUTTON)
			{
				int x = GET_X_LPARAM(lParam);
				int y = GET_Y_LPARAM(lParam);

				g_pApp->m_scene.GetCamera().RotateY(Math::DegreeToRadians((float)(x - g_pApp->m_lastMousePosX)));
				g_pApp->m_scene.GetCamera().RotateX(Math::DegreeToRadians((float)(y - g_pApp->m_lastMousePosY)));

				g_pApp->m_lastMousePosX = x;
				g_pApp->m_lastMousePosY = y;
			}
		}
	}

	void OnMouseWheel(UINT message, WPARAM btnState, LPARAM lParam) noexcept
	{
		if (ImGui::GetCurrentContext() == nullptr)
			return;

		ImGuiIO& io = ImGui::GetIO();

		//io.MouseWheel += (float)GET_WHEEL_DELTA_WPARAM(btnState) / (float)WHEEL_DELTA;
		io.AddMouseWheelEvent(0.0f, (float)GET_WHEEL_DELTA_WPARAM(btnState) / (float)WHEEL_DELTA);

		if (!io.WantCaptureMouse)
		{
			short zDelta = GET_WHEEL_DELTA_WPARAM(btnState);
			g_pApp->m_scene.GetCamera().MoveZ(g_pApp->m_cameraZoomSpeed * (float)zDelta);
		}
	}

	void OnDestroy() noexcept
	{
		ImGui::DestroyContext();
		ImPlot::DestroyContext();
		ImNodes::DestroyContext();

		App::FlushAllThreadPools();

		g_pApp->m_mainThreadPool.Shutdown();
		g_pApp->m_backgroundThreadPool.Shutdown();
		g_pApp->m_scene.Shutdown();
		g_pApp->m_renderer.Shutdown();
		g_pApp->m_params.clear();

		// TODO fix
		//for (int i = 0; i < m_processorCoreCount; i++)
		//	m_threadMemPools[i].Clear();

		delete g_pApp;
		g_pApp = nullptr;
	}

	LRESULT WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
	{
		switch (message)
		{
		case WM_ACTIVATEAPP:
			if (wParam)
				AppImpl::OnActivated();
			else
				AppImpl::OnDeactivated();

			return 0;

		case WM_ENTERSIZEMOVE:
			g_pApp->m_inSizeMove = true;
			AppImpl::OnDeactivated();

			return 0;

		case WM_EXITSIZEMOVE:
			g_pApp->m_inSizeMove = false;
			AppImpl::OnWindowSizeChanged();
			AppImpl::OnActivated();

			return 0;

		case WM_SIZE:
			if (!g_pApp->m_inSizeMove)
			{
				if (wParam == SIZE_MINIMIZED)
				{
					g_pApp->m_minimized = true;
					AppImpl::OnDeactivated();
				}
				else if (wParam == SIZE_RESTORED)
				{
					if (g_pApp->m_minimized)
						AppImpl::OnActivated();

					AppImpl::OnWindowSizeChanged();
				}
				else if (wParam == SIZE_MAXIMIZED)
				{
					AppImpl::OnWindowSizeChanged();
				}
			}

			return 0;

		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
			AppImpl::OnKeyboard(message, wParam, lParam);
			return 0;

		case WM_LBUTTONDOWN:
		case WM_MBUTTONDOWN:
		case WM_RBUTTONDOWN:
			AppImpl::OnMouseDown(message, wParam, lParam);
			return 0;

		case WM_LBUTTONUP:
		case WM_MBUTTONUP:
		case WM_RBUTTONUP:
			AppImpl::OnMouseUp(message, wParam, lParam);
			return 0;

		case WM_MOUSEMOVE:
			AppImpl::OnMouseMove(message, wParam, lParam);
			return 0;

		case WM_MOUSEWHEEL:
			AppImpl::OnMouseWheel(message, wParam, lParam);
			return 0;

		// TODO test
		case WM_DPICHANGED:
		{
			g_pApp->m_dpi = HIWORD(wParam);

			RECT* const prcNewWindow = (RECT*)lParam;
			SetWindowPos(hWnd, nullptr,
				prcNewWindow->left,
				prcNewWindow->top,
				prcNewWindow->right - prcNewWindow->left,
				prcNewWindow->bottom - prcNewWindow->top,
				SWP_NOZORDER | SWP_NOACTIVATE);

			// TODO font needs to be recreated

			return 0;
		}

		case WM_DESTROY:
			AppImpl::OnDestroy();
			PostQuitMessage(0);
			return 0;
		}

		return DefWindowProc(hWnd, message, wParam, lParam);
	}

	void CreateAppWindow(HINSTANCE instance) noexcept
	{
		const char* wndClassName = "MyWindowClass";

		WNDCLASSA wc{};

		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = WndProc;
		wc.cbClsExtra = 0;
		wc.cbWndExtra = 0;
		wc.hInstance = instance;
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.lpszClassName = wndClassName;

		RegisterClassA(&wc);

		g_pApp->m_hwnd = CreateWindowA(wndClassName,
			"ZetaRay",
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT,
			AppData::INITIAL_WINDOW_WIDTH, AppData::INITIAL_WINDOW_HEIGHT,
			nullptr, nullptr,
			instance,
			nullptr);

		AssertWin32(g_pApp->m_hwnd);

		SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		SetProcessDPIAware();
		g_pApp->m_dpi = GetDpiForWindow(g_pApp->m_hwnd);

		const int wndWidth = (int)((AppData::INITIAL_WINDOW_WIDTH * g_pApp->m_dpi) / 96.0f);
		const int wndHeight = (int)((AppData::INITIAL_WINDOW_HEIGHT * g_pApp->m_dpi) / 96.0f);

		SetWindowPos(g_pApp->m_hwnd, nullptr, 0, 0, wndWidth, wndHeight, 0);
		ShowWindow(g_pApp->m_hwnd, SW_SHOWNORMAL);
	}

	void GetProcessorInfo() noexcept
	{
		SYSTEM_LOGICAL_PROCESSOR_INFORMATION* buffer = nullptr;
		DWORD buffSize = 0;
		GetLogicalProcessorInformation(buffer, &buffSize);

		Assert(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "GetLogicalProcessorInformation() failed.");
		buffer = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(malloc(buffSize));
		Assert(buffer, "malloc() failed.");

		bool rc = GetLogicalProcessorInformation(buffer, &buffSize);
		Assert(rc, "GetLogicalProcessorInformation() failed.");

		int n = buffSize / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
		SYSTEM_LOGICAL_PROCESSOR_INFORMATION* curr = buffer;

		int logicalProcessorCount = 0;

		for (int i = 0; i < n; i++)
		{
			switch (curr->Relationship)
			{
			case RelationProcessorCore:
				g_pApp->m_processorCoreCount++;

				// A hyperthreaded core supplies more than one logical processor.
				logicalProcessorCount += (int)__popcnt64(curr->ProcessorMask);

			default:
				break;
			}

			curr++;
		}

		free(buffer);
	}

	void SetMoveSpeed(const ParamVariant& p) noexcept
	{
		g_pApp->m_cameraMoveSpeed = p.GetFloat().m_val;
	}

	void SetZoomSpeed(const ParamVariant& p) noexcept
	{
		g_pApp->m_cameraZoomSpeed = p.GetFloat().m_val;
	}
}

namespace ZetaRay
{
	inline int GetThreadIdx() noexcept
	{
		for (int i = 0; i < g_pApp->m_processorCoreCount + AppData::NUM_BACKGROUND_THREADS; i++)
		{
			if (g_pApp->m_threadIDs[i] == std::bit_cast<uint32_t, std::thread::id>(std::this_thread::get_id()))
			{
				return i;
			}
		}

		return -1;
	}
	
	ShaderReloadHandler::ShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg) noexcept
		: Dlg(dlg)
	{
		int n = std::min(MAX_LEN - 1, (int)strlen(name));
		Assert(n >= 1, "Invalid arg");
		memcpy(Name, name, n);
		Name[n] = '\0';

		ID = XXH3_64bits(Name, n);
	}

	void App::Init() noexcept
	{
		HINSTANCE instance = GetModuleHandleA(nullptr);
		AssertWin32(instance);

		g_pApp = new AppData;

		AppImpl::GetProcessorInfo();

		// create the window
		AppImpl::CreateAppWindow(instance);
		SetWindowTextA(g_pApp->m_hwnd, "ZetaRay");

		const int totalNumThreads = g_pApp->m_processorCoreCount + AppData::NUM_BACKGROUND_THREADS;

		// initialize thread pools
		g_pApp->m_mainThreadPool.Init(g_pApp->m_processorCoreCount - 1, 
			totalNumThreads,
			L"ZetaWorker", 
			THREAD_PRIORITY::NORMAL);

		g_pApp->m_backgroundThreadPool.Init(AppData::NUM_BACKGROUND_THREADS,
			totalNumThreads,
			L"ZetaBackgroundWorker", 
			THREAD_PRIORITY::BACKGROUND);

		// main thread
		g_pApp->m_threadContexts[0].MemPool.Init();
		g_pApp->m_threadIDs[0] = std::bit_cast<uint32_t, std::thread::id>(std::this_thread::get_id());
		g_pApp->m_threadContexts[0].Lock = SRWLOCK_INIT;
		g_pApp->m_threadContexts[0].Rng = RNG(g_pApp->m_threadIDs[0]);

		// initialize memory pools
		// this has to happen after thread pool has been created
		auto mainThreadIDs = g_pApp->m_mainThreadPool.ThreadIDs();
		for (int i = 0; i < mainThreadIDs.size(); i++)
		{
			g_pApp->m_threadIDs[i + 1] = std::bit_cast<uint32_t, std::thread::id>(mainThreadIDs[i]);
			g_pApp->m_threadContexts[i + 1].MemPool.Init();
			g_pApp->m_threadContexts[i + 1].Lock = SRWLOCK_INIT;
			g_pApp->m_threadContexts[i + 1].Rng = RNG(g_pApp->m_threadIDs[i + 1]);
		}

		// background threads don't have a dedicated memory pool
		auto backgroundThreadIDs = g_pApp->m_backgroundThreadPool.ThreadIDs();
		for (int i = 0; i < backgroundThreadIDs.size(); i++)
		{
			g_pApp->m_threadIDs[mainThreadIDs.size() + 1 + i] = std::bit_cast<uint32_t, std::thread::id>(backgroundThreadIDs[i]);
			g_pApp->m_threadContexts[mainThreadIDs.size() + i + 1].Rng = RNG(g_pApp->m_threadIDs[mainThreadIDs.size() + 1 + i]);
		}

//		g_pApp->m_mainThreadPool.SetThreadIds(Span(g_pApp->m_threadIDs, 1 + mainThreadIDs.size()));
//		g_pApp->m_backgroundThreadPool.SetThreadIds(Span(g_pApp->m_threadIDs, 1 + mainThreadIDs.size() + backgroundThreadIDs.size()));

		g_pApp->m_mainThreadPool.Start();
		g_pApp->m_backgroundThreadPool.Start();

		RECT rect;
		GetClientRect(g_pApp->m_hwnd, &rect);

		g_pApp->m_displayWidth = rect.right - rect.left;
		g_pApp->m_displayHeight = rect.bottom - rect.top;

		AppImpl::InitImGui();

		const float renderWidth = g_pApp->m_displayWidth / g_pApp->m_upscaleFactor;
		const float renderHeight = g_pApp->m_displayHeight / g_pApp->m_upscaleFactor;

		// initialize the renderer
		g_pApp->m_renderer.Init(g_pApp->m_hwnd, (int)renderWidth, (int)renderHeight, g_pApp->m_displayWidth, g_pApp->m_displayHeight);

		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2((float)g_pApp->m_displayWidth, (float)g_pApp->m_displayHeight);

		// scene can now be initialized
		g_pApp->m_scene.Init();

		ParamVariant p0;
		p0.InitFloat("Scene", "Camera", "MoveSpeedMult", fastdelegate::FastDelegate1<const ParamVariant&>(&AppImpl::SetMoveSpeed),
			g_pApp->m_cameraMoveSpeed,
			0.01f,
			50.0f,
			0.1f);
		g_pApp->m_params.push_back(p0);

		ParamVariant p1;
		p1.InitFloat("Scene", "Camera", "ZoomSpeedMult", fastdelegate::FastDelegate1<const ParamVariant&>(&AppImpl::SetZoomSpeed),
			g_pApp->m_cameraZoomSpeed,
			0.0001f,
			0.01f,
			0.001f);
		g_pApp->m_params.push_back(p1);

		//ParamVariant p2;
		//p2.InitBool("Renderer", "Settings", "Upscaling", fastdelegate::FastDelegate1<const ParamVariant&>(&AppImpl::SetUpscalingEnablement),
		//	g_pApp->m_upscaleFactor > 1.0f + 1e-3f);
		//g_pApp->m_params.push_back(p2);
	}

	void App::InitSimple() noexcept
	{
		// main thread
		if (g_pApp == nullptr || !g_pApp->m_isInitialized)
		{
			g_pApp = new AppData;
			g_pApp->m_processorCoreCount = 1;

			g_pApp->m_isInitialized = true;

			g_pApp->m_threadContexts[0].MemPool.Init();
			g_pApp->m_threadIDs[0] = std::bit_cast<uint32_t, std::thread::id>(std::this_thread::get_id());
			g_pApp->m_threadContexts[0].Lock = SRWLOCK_INIT;
			g_pApp->m_threadContexts[0].Rng = RNG(g_pApp->m_threadIDs[0]);
		}
	}

	int App::Run() noexcept
	{
		MSG msg = {};

		while (msg.message != WM_QUIT)
		{
			if (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageA(&msg);
			}
			else
			{
				if (!g_pApp->m_isActive)
				{
					Sleep(50);
					continue;
				}

				// Help out while there are unfinished tasks from the previous frame
				bool success = g_pApp->m_mainThreadPool.TryFlush();
				// don't block the message-handling thread
				if (!success)
					continue;

				// begin frame
				g_pApp->m_renderer.BeginFrame();
				g_pApp->m_timer.Tick();

				// at this point, we know all the (CPU) tasks from the previous frame are done
				g_pApp->m_currTaskSignalIdx.store(0, std::memory_order_relaxed);

				// update
				{
					TaskSet sceneTS;
					TaskSet sceneRendererTS;
					AppImpl::Update(sceneTS, sceneRendererTS);

					auto h0 = sceneRendererTS.EmplaceTask("ResourceUploadSubmission", []()
						{
							g_pApp->m_renderer.SubmitResourceCopies();
						});

					// make sure resource submission runs after everything else
					sceneRendererTS.AddIncomingEdgeFromAll(h0);

					sceneTS.Sort();
					sceneRendererTS.Sort();

					// sceneRendererTS has to run after sceneTS. this may seem sequential but
					// each task can spawn more tasks
					sceneTS.ConnectTo(sceneRendererTS);

					sceneTS.Finalize();
					sceneRendererTS.Finalize();

					// submit
					Submit(ZetaMove(sceneTS));
					Submit(ZetaMove(sceneRendererTS));
				}

				// make sure all updates are finished before moving to rendering
				success = false;
				while (!success)
				{
					success = g_pApp->m_mainThreadPool.TryFlush();
				}

				TaskSet renderTS;

				// render
				{
					g_pApp->m_scene.Render(renderTS);
					renderTS.Sort();
				}

				TaskSet endFrameTS;

				// end-frame
				{
					g_pApp->m_renderer.EndFrame(endFrameTS);

					auto h0 = endFrameTS.EmplaceTask("Scene::Recycle", []()
						{
							g_pApp->m_scene.Recycle();
						});

					endFrameTS.Sort();
					renderTS.ConnectTo(endFrameTS);

					renderTS.Finalize();
					endFrameTS.Finalize();
				}

				Submit(ZetaMove(renderTS));
				Submit(ZetaMove(endFrameTS));

				g_pApp->m_mainThreadPool.PumpUntilEmpty();
			}
		}

		return (int)msg.wParam;
	}

	void App::Abort() noexcept
	{
		AppImpl::OnDestroy();
		PostQuitMessage(0);
	}

	void* App::AllocateMemory(size_t size, const char* str, int alignment) noexcept
	{
		//return malloc(size);

		int idx = GetThreadIdx();
		Assert(idx != -1, "thread idx was not found");
		int poolIdx = g_pApp->m_threadContexts[idx].Rng.GetUniformUintBounded(g_pApp->m_processorCoreCount);
		void* mem;

		while (true)
		{
			if (TryAcquireSRWLockExclusive(&g_pApp->m_threadContexts[poolIdx].Lock))
			{
				mem = g_pApp->m_threadContexts[poolIdx].MemPool.AllocateAligned(size, alignment);
				ReleaseSRWLockExclusive(&g_pApp->m_threadContexts[poolIdx].Lock);

				break;
			}

			poolIdx = poolIdx + 1 < g_pApp->m_processorCoreCount ? poolIdx + 1 : 0;
		}

		return mem;	
	}

	void App::FreeMemory(void* pMem, size_t size, const char* str, int alignment) noexcept
	{
		//free(pMem);
	
		int idx = GetThreadIdx();
		Assert(idx != -1, "thread idx was not found");
		int poolIdx = g_pApp->m_threadContexts[idx].Rng.GetUniformUintBounded(g_pApp->m_processorCoreCount);

		while (true)
		{
			if (TryAcquireSRWLockExclusive(&g_pApp->m_threadContexts[poolIdx].Lock))
			{
				g_pApp->m_threadContexts[poolIdx].MemPool.FreeAligned(pMem, size, alignment);
				ReleaseSRWLockExclusive(&g_pApp->m_threadContexts[poolIdx].Lock);

				return;
			}

			poolIdx = poolIdx + 1 < g_pApp->m_processorCoreCount ? poolIdx + 1 : 0;
		}
	}

	int App::RegisterTask() noexcept
	{
		int idx = g_pApp->m_currTaskSignalIdx.fetch_add(1, std::memory_order_relaxed);
		Assert(idx < AppData::MAX_NUM_TASKS_PER_FRAME, "number of task signals exceeded MAX_NUM_TASKS_PER_FRAME");

		return idx;
	}

	void App::TaskFinalizedCallback(int handle, int indegree) noexcept
	{
		Assert(indegree > 0, "unnecessary call.");
		const int c = g_pApp->m_currTaskSignalIdx.load(std::memory_order_relaxed);
		Assert(handle < c, "received handle %d while #handles for current frame is %d", c);

		g_pApp->m_registeredTasks[handle].Indegree.store(indegree, std::memory_order_release);
		g_pApp->m_registeredTasks[handle].BlockFlag.store(true, std::memory_order_release);
	}

	void App::WaitForAdjacentHeadNodes(int handle) noexcept
	{
		const int c = g_pApp->m_currTaskSignalIdx.load(std::memory_order_relaxed);
		Assert(handle >= 0 && handle < c, "received handle %d while #handles for current frame is %d", c);

		auto& taskSignal = g_pApp->m_registeredTasks[handle];
		const int indegree = taskSignal.Indegree.load(std::memory_order_acquire);
		Assert(indegree >= 0, "invalid task indegree");

		if (indegree != 0)
		{
			taskSignal.BlockFlag.wait(true, std::memory_order_acquire);
			return;
		}
	}

	void App::SignalAdjacentTailNodes(int* taskIDs, int n) noexcept
	{
		for (int i = 0; i < n; i++)
		{
			int handle = taskIDs[i];

			auto& taskSignal = g_pApp->m_registeredTasks[handle];
			const int n = taskSignal.Indegree.fetch_sub(1, std::memory_order_acquire);

			// this was the last dependency, unblock the task
			if (n == 1)
			{
				taskSignal.BlockFlag.store(false, std::memory_order_release);
				taskSignal.BlockFlag.notify_one();
			}
		}
	}

	void App::Submit(Task&& t) noexcept
	{
		Assert(t.GetPriority() == TASK_PRIORITY::NORMAL, "Background task is not allowed to be executed in main thread-pool");
		g_pApp->m_mainThreadPool.Enqueue(ZetaMove(t));
	}

	void App::Submit(TaskSet&& ts) noexcept
	{
		g_pApp->m_mainThreadPool.Enqueue(ZetaMove(ts));
	}

	void App::SubmitBackground(Task&& t) noexcept
	{
		Assert(t.GetPriority() == TASK_PRIORITY::BACKGRUND, "Normal task is not allowed to be executed in background thread-pool");
		g_pApp->m_backgroundThreadPool.Enqueue(ZetaMove(t));
	}

	void App::FlushMainThreadPool() noexcept
	{
		bool success = false;
		while (!success)
		{
			success = g_pApp->m_mainThreadPool.TryFlush();
		}
	}
	
	void App::FlushAllThreadPools() noexcept
	{
		bool success = false;
		while (!success)
		{
			success = g_pApp->m_mainThreadPool.TryFlush();
		}

		success = false;
		while (!success)
		{
			success = g_pApp->m_backgroundThreadPool.TryFlush();
		}
	}

	Renderer& App::GetRenderer() noexcept { return g_pApp->m_renderer; }
	Scene& App::GetScene() noexcept { return g_pApp->m_scene; }
	int App::GetNumThreads() noexcept { return g_pApp->m_processorCoreCount; }
	uint32_t App::GetDPI() noexcept { return g_pApp->m_dpi; }
	float  App::GetUpscalingFactor() noexcept { return g_pApp->m_upscaleFactor; }
	bool App::IsFullScreen() noexcept { return g_pApp->m_isFullScreen; }
	const Win32::Timer& App::GetTimer() noexcept { return g_pApp->m_timer; }
	const char* App::GetPSOCacheDir() noexcept { return AppData::PSO_CACHE_DIR; }
	const char* App::GetCompileShadersDir() noexcept { return AppData::COMPILED_SHADER_DIR; }
	const char* App::GetAssetDir() noexcept { return AppData::ASSET_DIR; }
	const char* App::GetDXCPath() noexcept { return AppData::DXC_PATH; }
	const char* App::GetToolsDir() noexcept { return AppData::TOOLS_DIR; }
	const char* App::GetRenderPassDir() noexcept { return AppData::RENDER_PASS_DIR; }

	void App::SetUpscalingEnablement(bool e) noexcept
	{
		const float oldScaleFactor = g_pApp->m_upscaleFactor;

		if (e)
		{
			g_pApp->m_upscaleFactor = 1.5f;
		}
		else
		{
			g_pApp->m_upscaleFactor = 1.0f;
		}

		if (oldScaleFactor == g_pApp->m_upscaleFactor)
			return;

		const float renderWidth = g_pApp->m_displayWidth / g_pApp->m_upscaleFactor;
		const float renderHeight = g_pApp->m_displayHeight / g_pApp->m_upscaleFactor;

		g_pApp->m_renderer.OnWindowSizeChanged(g_pApp->m_hwnd, (int)renderWidth, (int)renderHeight, g_pApp->m_displayWidth, g_pApp->m_displayHeight);
		g_pApp->m_scene.OnWindowSizeChanged();
	}

	void App::LockStdOut() noexcept
	{
		AcquireSRWLockExclusive(&g_pApp->m_stdOutLock);
	}

	void App::UnlockStdOut() noexcept
	{
		ReleaseSRWLockExclusive(&g_pApp->m_stdOutLock);
	}

	Span<uint32_t> App::GetMainThreadIDs() noexcept
	{
		return Span(g_pApp->m_threadIDs, g_pApp->m_processorCoreCount);
	}

	Span<uint32_t> App::GetAllThreadIDs() noexcept
	{
		return Span(g_pApp->m_threadIDs, g_pApp->m_processorCoreCount + AppData::NUM_BACKGROUND_THREADS);
	}

	RWSynchronizedView<Vector<ParamVariant, alignof(std::max_align_t)>> App::GetParams() noexcept
	{
		return RWSynchronizedView<Vector<ParamVariant, alignof(std::max_align_t)>>(g_pApp->m_params, g_pApp->m_paramLock);
	}

	RSynchronizedView<Vector<ShaderReloadHandler, alignof(std::max_align_t)>> App::GetShaderReloadHandlers() noexcept
	{
		return RSynchronizedView<Vector<ShaderReloadHandler, alignof(std::max_align_t)>>(g_pApp->m_shaderReloadHandlers, g_pApp->m_shaderReloadLock);
	}

	RWSynchronizedView<Vector<Stat, alignof(std::max_align_t)>> App::GetStats() noexcept
	{
		return RWSynchronizedView<Vector<Stat, alignof(std::max_align_t)>>(g_pApp->m_frameStats, g_pApp->m_statsLock);
	}

	void App::AddParam(ParamVariant& p) noexcept
	{
		AcquireSRWLockExclusive(&g_pApp->m_paramLock);
		g_pApp->m_params.push_back(p);
		ReleaseSRWLockExclusive(&g_pApp->m_paramLock);
	}

	void App::RemoveParam(const char* group, const char* subgroup, const char* name) noexcept
	{
		Assert(group, "group can't be null");
		Assert(subgroup, "subgroup can't be null");
		Assert(name, "name can't be null");

		constexpr int BUFF_SIZE = ParamVariant::MAX_GROUP_LEN + ParamVariant::MAX_SUBGROUP_LEN + ParamVariant::MAX_NAME_LEN;
		char concatBuff[BUFF_SIZE];
		size_t ptr = 0;

		size_t p = strlen(group);
		Assert(p > 0 && p < BUFF_SIZE, "buffer overflow");
		memcpy(concatBuff, group, p);
		ptr += p;

		p = strlen(subgroup);
		Assert(p > 0 && ptr + p < BUFF_SIZE, "buffer overflow");
		memcpy(concatBuff + ptr, subgroup, p);
		ptr += p;

		p = strlen(name);
		Assert(p > 0 && ptr + p < BUFF_SIZE, "buffer overflow");
		memcpy(concatBuff + ptr, name, p);

		uint64_t id = XXH3_64bits(concatBuff, ptr + p);

		AcquireSRWLockExclusive(&g_pApp->m_paramLock);

		size_t i = 0;
		bool found = false;
		while (i < g_pApp->m_params.size())
		{
			if (g_pApp->m_params[i].GetID() == id)
			{
				found = true;
				break;
			}

			i++;
		}

		Assert(found, "parmeter {group: %s, subgroup: %s, name: %s} was not found.", group, subgroup, name);
		g_pApp->m_params.erase(i);

		ReleaseSRWLockExclusive(&g_pApp->m_paramLock);
	}

	void App::AddShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg) noexcept
	{
		AcquireSRWLockExclusive(&g_pApp->m_shaderReloadLock);
		g_pApp->m_shaderReloadHandlers.emplace_back(name, dlg);
		ReleaseSRWLockExclusive(&g_pApp->m_shaderReloadLock);
	}

	void App::RemoveShaderReloadHandler(const char* name) noexcept
	{
		uint64_t id = XXH3_64bits(name, std::min(ShaderReloadHandler::MAX_LEN, (int)strlen(name) + 1));

		AcquireSRWLockExclusive(&g_pApp->m_shaderReloadLock);
		size_t i = 0;
		bool found = false;

		for (i = 0; i < g_pApp->m_shaderReloadHandlers.size(); i++)
		{
			if (g_pApp->m_shaderReloadHandlers[i].ID == id)
			{
				found = true;
				break;
			}
		}

		if (found)
			g_pApp->m_shaderReloadHandlers.erase(i);

		ReleaseSRWLockExclusive(&g_pApp->m_shaderReloadLock);
	}

	void App::AddFrameStat(const char* group, const char* name, int i) noexcept
	{
		AcquireSRWLockExclusive(&g_pApp->m_statsLock);
		g_pApp->m_frameStats.emplace_back(group, name, i);
		ReleaseSRWLockExclusive(&g_pApp->m_statsLock);
	}

	void App::AddFrameStat(const char* group, const char* name, uint32_t u) noexcept
	{
		AcquireSRWLockExclusive(&g_pApp->m_statsLock);
		g_pApp->m_frameStats.emplace_back(group, name, u);
		ReleaseSRWLockExclusive(&g_pApp->m_statsLock);
	}

	void App::AddFrameStat(const char* group, const char* name, float f) noexcept
	{
		AcquireSRWLockExclusive(&g_pApp->m_statsLock);
		g_pApp->m_frameStats.emplace_back(group, name, f);
		ReleaseSRWLockExclusive(&g_pApp->m_statsLock);
	}	
	
	void App::AddFrameStat(const char* group, const char* name, uint64_t u) noexcept
	{
		AcquireSRWLockExclusive(&g_pApp->m_statsLock);
		g_pApp->m_frameStats.emplace_back(group, name, u);
		ReleaseSRWLockExclusive(&g_pApp->m_statsLock);
	}	
	
	void App::AddFrameStat(const char* group, const char* name, uint32_t num, uint32_t total) noexcept
	{
		AcquireSRWLockExclusive(&g_pApp->m_statsLock);
		g_pApp->m_frameStats.emplace_back(group, name, num, total);
		ReleaseSRWLockExclusive(&g_pApp->m_statsLock);
	}

	Span<double> App::GetFrameTimeHistory() noexcept
	{
		auto& frameStats = g_pApp->m_frameTime;
		return frameStats.FrameTimeHist;
	}
}
