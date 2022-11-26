#include "App.h"
#include "../Support/MemoryPool.h"
#include "../Utility/SynchronizedView.h"
#include "Timer.h"
#include "../Support/Param.h"
#include "../Support/Stat.h"
#include "../Utility/HashTable.h"
#include "../Utility/Span.h"
#include "../Core/Renderer.h"
#include "../Core/CommandQueue.h"			// just for std::unique_ptr<>
#include "../Core/SharedShaderResources.h"	// just for std::unique_ptr<>
#include "../Scene/SceneCore.h"
#include "../Support/ThreadPool.h"
#include "../Utility/RNG.h"
#include "../../Assets/Fonts/SegoeUI.h"
#include <atomic>

//#define STB_SPRINTF_IMPLEMENTATION
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION 
#include <xxHash-0.8.1/xxhash.h>
#include <ImGui/imgui.h>
#include <ImGui/implot.h>
#include <ImGui/imnodes.h>

using namespace ZetaRay::Win32;
using namespace ZetaRay::App;
using namespace ZetaRay::Core;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Math;

namespace
{
	struct FrameTime
	{
		static constexpr int HIST_LEN = 60;
		float FrameTimeHist[HIST_LEN] = { 0.0 };
		int NextFramHistIdx = 0;
	};

	struct ParamUpdate
	{
		enum OP_TYPE
		{
			ADD,
			REMOVE
		};

		ZetaRay::Support::ParamVariant P;
		OP_TYPE Op;
	};
}

namespace
{
	struct AppData
	{
		//static const int INITIAL_WINDOW_WIDTH = 1024;
		//static const int INITIAL_WINDOW_HEIGHT = 576;
		static const int INITIAL_WINDOW_WIDTH = 1536;
		static const int INITIAL_WINDOW_HEIGHT = 864;
#if defined(_DEBUG)
		inline static const char* PSO_CACHE_DIR = "Assets\\PsoCache\\Debug";
		inline static const char* COMPILED_SHADER_DIR = "Assets\\CSO\\Debug";
#else
		inline static const char* PSO_CACHE_DIR = "Assets\\PsoCache\\Release";
		inline static const char* COMPILED_SHADER_DIR = "Assets\\CSO\\Release";
#endif // _DEBUG

		inline static const char* ASSET_DIR = "Assets";
		inline static const char* TOOLS_DIR = "Tools";
		inline static const char* DXC_PATH = "Tools\\dxc\\bin\\x64\\dxc.exe";
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
		int m_inMouseWheelMove = 0;
		bool m_inSizeMove = false;
		bool m_minimized = false;
		bool m_isFullScreen = false;
		ImGuiMouseCursor m_imguiCursor = ImGuiMouseCursor_COUNT;
		bool m_imguiMouseTracked = false;
		uint32_t m_dpi;
		float m_upscaleFactor = 1.0f;
		float m_cameraAcceleration = 15.0f;

		Timer m_timer;
		Renderer m_renderer;
		ThreadPool m_mainThreadPool;
		ThreadPool m_backgroundThreadPool;
		SceneCore m_scene;
		Camera m_camera;

		struct alignas(64) ThreadContext
		{
			MemoryPool MemPool;
			SRWLOCK Lock;
			RNG Rng;
		};

		ThreadContext m_threadContexts[MAX_NUM_THREADS];
		//std::thread::id m_threadIDs[MAX_NUM_THREADS];
		uint32_t m_threadIDs[MAX_NUM_THREADS];

		SmallVector<ParamVariant, PoolAllocator> m_params;
		SmallVector<ParamUpdate, PoolAllocator, 32> m_paramsUpdates;

		SmallVector<ShaderReloadHandler, PoolAllocator> m_shaderReloadHandlers;
		SmallVector<Stat, PoolAllocator> m_frameStats;
		FrameTime m_frameTime;

		//std::shared_mutex m_stdOutMtx;
		SRWLOCK m_stdOutLock = SRWLOCK_INIT;
		SRWLOCK m_paramLock = SRWLOCK_INIT;
		SRWLOCK m_paramUpdateLock = SRWLOCK_INIT;
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

		Motion m_frameMotion;
	};

	static AppData* g_pApp = nullptr;
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

		// TODO remove hard-coded path
		io.IniFilename = "temp//imgui.ini";
	}

	void UpdateStats() noexcept
	{
		g_pApp->m_frameStats.clear();

		const float frameTimeMs = (float)(g_pApp->m_timer.GetElapsedTime() * 1000.0);

		auto& frameStats = g_pApp->m_frameTime;
		frameStats.NextFramHistIdx = (frameStats.NextFramHistIdx < 59) ? frameStats.NextFramHistIdx + 1 : frameStats.NextFramHistIdx;
		Assert(frameStats.NextFramHistIdx >= 0 && frameStats.NextFramHistIdx < 60, "bug");

		// shift left
		float temp[FrameTime::HIST_LEN];
		memcpy(temp, frameStats.FrameTimeHist + 1, sizeof(float) * (FrameTime::HIST_LEN - 1));
		memcpy(frameStats.FrameTimeHist, temp, sizeof(float) * (FrameTime::HIST_LEN - 1));
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

		g_pApp->m_frameMotion.dt = (float)g_pApp->m_timer.GetElapsedTime();

		float scale = g_pApp->m_inMouseWheelMove ? g_pApp->m_inMouseWheelMove * 20 : 1.0f;
		scale = g_pApp->m_frameMotion.Acceleration.z != 0 || g_pApp->m_frameMotion.Acceleration.x != 0 ?
			fabsf(scale) : scale;

		// 'W'
		if (g_pApp->m_inMouseWheelMove || (GetAsyncKeyState(0x57) & (1 << 16)))
			g_pApp->m_frameMotion.Acceleration.z = 1;
		// 'A'
		if (GetAsyncKeyState(0x41) & (1 << 16))
			g_pApp->m_frameMotion.Acceleration.x = -1;
		// 'S'
		if (!g_pApp->m_inMouseWheelMove && (GetAsyncKeyState(0x53) & (1 << 16)))
			g_pApp->m_frameMotion.Acceleration.z = -1;
		// 'D'
		if (GetAsyncKeyState(0x44) & (1 << 16))
			g_pApp->m_frameMotion.Acceleration.x = 1;

		g_pApp->m_frameMotion.Acceleration.normalize(); 
		g_pApp->m_frameMotion.Acceleration *= g_pApp->m_cameraAcceleration * scale;
		g_pApp->m_inMouseWheelMove = 0;
		g_pApp->m_camera.Update(g_pApp->m_frameMotion);

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
			/*
			switch (vkKey)
			{
				//A
			case 0x41:
				//g_pApp->m_frameMotion.Acceleration.x = -g_pApp->m_cameraAcceleration;
				return;

				//D
			case 0x44:
				//g_pApp->m_frameMotion.Acceleration.x = g_pApp->m_cameraAcceleration;
				return;

				//W
			case 0x57:
				//printf("%llu, OnKeyboard(), repeat count: %lld\n", g_pApp->m_timer.GetTotalFrameCount(), lParam & 0xffff);
				//g_pApp->m_frameMotion.Acceleration.z = g_pApp->m_cameraAcceleration;
				return;

				//S
			case 0x53:
				//g_pApp->m_frameMotion.Acceleration.z = -g_pApp->m_cameraAcceleration;
				return;
			}
			*/

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
			button = 0;
		else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK)
			button = 1;
		else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK)
			button = 2;

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
			button = 0;
		else if (message == WM_RBUTTONUP)
			button = 1;
		else if (message == WM_MBUTTONUP)
			button = 2;

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

				//g_pApp->m_scene.GetCamera().RotateY(Math::DegreeToRadians((float)(x - g_pApp->m_lastMousePosX)));
				//g_pApp->m_scene.GetCamera().RotateX(Math::DegreeToRadians((float)(y - g_pApp->m_lastMousePosY)));

				g_pApp->m_frameMotion.RotationDegreesY = Math::DegreeToRadians((float)(x - g_pApp->m_lastMousePosX));
				g_pApp->m_frameMotion.RotationDegreesX = Math::DegreeToRadians((float)(y - g_pApp->m_lastMousePosY));

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

			if (zDelta > 0)
				g_pApp->m_inMouseWheelMove = 1;
			else
				g_pApp->m_inMouseWheelMove = -1;
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

	void ApplyParamUpdates() noexcept
	{
		AcquireSRWLockExclusive(&g_pApp->m_paramUpdateLock);
		AcquireSRWLockExclusive(&g_pApp->m_paramLock);

		for (auto& p : g_pApp->m_paramsUpdates)
		{
			if (p.Op == ParamUpdate::OP_TYPE::ADD)
			{
				g_pApp->m_params.push_back(p.P);
			}
			else if (p.Op == ParamUpdate::OP_TYPE::REMOVE)
			{
				size_t i = 0;
				bool found = false;
				while (i < g_pApp->m_params.size())
				{
					if (g_pApp->m_params[i].GetID() == p.P.GetID())
					{
						found = true;
						break;
					}

					i++;
				}

				Assert(found, "parmeter {group: %s, subgroup: %s, name: %s} was not found.", p.P.GetGroup(), p.P.GetSubGroup(), p.P.GetName());
				g_pApp->m_params.erase(i);
			}
		}

		g_pApp->m_paramsUpdates.clear();

		ReleaseSRWLockExclusive(&g_pApp->m_paramLock);
		ReleaseSRWLockExclusive(&g_pApp->m_paramUpdateLock);
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

		CheckWin32(g_pApp->m_hwnd);

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

	void SetCameraAcceleration(const ParamVariant& p) noexcept
	{
		g_pApp->m_cameraAcceleration = p.GetFloat().m_val;
	}}

namespace ZetaRay
{
	inline int GetThreadIdx() noexcept
	{
		for (int i = 0; i < g_pApp->m_processorCoreCount + AppData::NUM_BACKGROUND_THREADS; i++)
		{
			if (g_pApp->m_threadIDs[i] == std::bit_cast<uint32_t, std::thread::id>(std::this_thread::get_id()))
				return i;
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
		CheckWin32(instance);
		
		// set locale to C
		setlocale(LC_ALL, "C");

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

		// initialize renderer
		g_pApp->m_renderer.Init(g_pApp->m_hwnd, (int)renderWidth, (int)renderHeight, g_pApp->m_displayWidth, g_pApp->m_displayHeight);

		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2((float)g_pApp->m_displayWidth, (float)g_pApp->m_displayHeight);

		// initialize camera
		g_pApp->m_frameMotion.Reset();

		//m_camera.Init(float3(-10.61f, 4.67f, -3.25f), App::GetRenderer().GetAspectRatio(), 
		//	Math::DegreeToRadians(85.0f), 0.1f, true);
		g_pApp->m_camera.Init(float3(-5.61f, 4.67f, -0.25f), App::GetRenderer().GetAspectRatio(),
			Math::DegreeToRadians(75.0f), 0.1f, true);
		//m_camera.Init(float3(-1127.61f, 348.67f, 66.25f), App::GetRenderer().GetAspectRatio(), 
		//	Math::DegreeToRadians(85.0f), 10.0f, true);
		//m_camera.Init(float3(0.61f, 3.67f, 0.25f), App::GetRenderer().GetAspectRatio(), Math::DegreeToRadians(85.0f), 0.1f);

		// scene can now be initialized
		g_pApp->m_scene.Init();

		ParamVariant acc;
		acc.InitFloat("Scene", "Camera", "Acceleration", fastdelegate::FastDelegate1(&AppImpl::SetCameraAcceleration),
			g_pApp->m_cameraAcceleration,
			0.1f,
			100.0f,
			1.0f);
		App::AddParam(acc);

		g_pApp->m_isInitialized = true;
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
			// process messages
			if (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageA(&msg);
			}
			// game loop
			else
			{
				if (!g_pApp->m_isActive)
				{
					Sleep(16);
					continue;
				}

				// help out while there are unfinished tasks from the previous frame
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
					TaskSet appTS;

					auto ha0 = appTS.EmplaceTask("AppUpdates", []()
						{
							AppImpl::ApplyParamUpdates();
						});

					appTS.Sort();
					appTS.Finalize();
					Submit(ZetaMove(appTS));

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
					// each taskset is spawning many tasks (which potentially can run in parallel)
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
					success = g_pApp->m_mainThreadPool.TryFlush();

				g_pApp->m_frameMotion.Reset();

				// render
				{
					TaskSet renderTS;
					TaskSet endFrameTS;

					g_pApp->m_scene.Render(renderTS);
					renderTS.Sort();

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
				}

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

	void* App::AllocateFromMemoryPool(size_t size, const char* str, uint32_t alignment) noexcept
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

	void App::FreeMemoryPool(void* pMem, size_t size, const char* str, uint32_t alignment) noexcept
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
			success = g_pApp->m_mainThreadPool.TryFlush();
	}

	void App::FlushAllThreadPools() noexcept
	{
		bool success = false;
		while (!success)
			success = g_pApp->m_mainThreadPool.TryFlush();

		success = false;
		while (!success)
			success = g_pApp->m_backgroundThreadPool.TryFlush();
	}

	Renderer& App::GetRenderer() noexcept { return g_pApp->m_renderer; }
	SceneCore& App::GetScene() noexcept { return g_pApp->m_scene; }
	const Camera& App::GetCamera() noexcept { return g_pApp->m_camera; }
	int App::GetNumMainThreads() noexcept { return g_pApp->m_processorCoreCount; }
	int App::GetNumBackgroundThreads() noexcept { return AppData::NUM_BACKGROUND_THREADS; }
	uint32_t App::GetDPI() noexcept { return g_pApp->m_dpi; }
	float App::GetUpscalingFactor() noexcept { return g_pApp->m_upscaleFactor; }
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
			g_pApp->m_upscaleFactor = 1.5f;
		else
			g_pApp->m_upscaleFactor = 1.0f;

		if (oldScaleFactor == g_pApp->m_upscaleFactor)
			return;

		const float renderWidth = g_pApp->m_displayWidth / g_pApp->m_upscaleFactor;
		const float renderHeight = g_pApp->m_displayHeight / g_pApp->m_upscaleFactor;

		g_pApp->m_renderer.OnWindowSizeChanged(g_pApp->m_hwnd, (int)renderWidth, (int)renderHeight, g_pApp->m_displayWidth, g_pApp->m_displayHeight);
		g_pApp->m_scene.OnWindowSizeChanged();
	}

	void App::LockStdOut() noexcept
	{
		if(g_pApp)
			AcquireSRWLockExclusive(&g_pApp->m_stdOutLock);
	}

	void App::UnlockStdOut() noexcept
	{
		if(g_pApp)
			ReleaseSRWLockExclusive(&g_pApp->m_stdOutLock);
	}

	Span<uint32_t> App::GetMainThreadIDs() noexcept
	{
		return Span(g_pApp->m_threadIDs, g_pApp->m_processorCoreCount);
	}

	Span<uint32_t> App::GetBackgroundThreadIDs() noexcept
	{
		return Span(g_pApp->m_threadIDs + g_pApp->m_processorCoreCount, AppData::NUM_BACKGROUND_THREADS);
	}

	Span<uint32_t> App::GetAllThreadIDs() noexcept
	{
		return Span(g_pApp->m_threadIDs, g_pApp->m_processorCoreCount + AppData::NUM_BACKGROUND_THREADS);
	}

	RWSynchronizedView<Vector<ParamVariant, PoolAllocator>> App::GetParams() noexcept
	{
		return RWSynchronizedView<Vector<ParamVariant, PoolAllocator>>(g_pApp->m_params, g_pApp->m_paramLock);
	}

	RSynchronizedView<Vector<ShaderReloadHandler, PoolAllocator>> App::GetShaderReloadHandlers() noexcept
	{
		return RSynchronizedView<Vector<ShaderReloadHandler, PoolAllocator>>(g_pApp->m_shaderReloadHandlers, g_pApp->m_shaderReloadLock);
	}

	RWSynchronizedView<Vector<Stat, PoolAllocator>> App::GetStats() noexcept
	{
		return RWSynchronizedView<Vector<Stat, PoolAllocator>>(g_pApp->m_frameStats, g_pApp->m_statsLock);
	}

	void App::AddParam(ParamVariant& p) noexcept
	{
		AcquireSRWLockExclusive(&g_pApp->m_paramUpdateLock);

		g_pApp->m_paramsUpdates.push_back(ParamUpdate{
			.P = p,
			.Op = ParamUpdate::ADD });

		ReleaseSRWLockExclusive(&g_pApp->m_paramUpdateLock);
	}

	void App::RemoveParam(const char* group, const char* subgroup, const char* name) noexcept
	{
		AcquireSRWLockExclusive(&g_pApp->m_paramUpdateLock);

		// create a dummy ParamVariant (never exposed to outside)
		ParamVariant dummy;
		dummy.InitBool(group, subgroup, name, fastdelegate::FastDelegate1<const ParamVariant&>(), false);

		g_pApp->m_paramsUpdates.push_back(ParamUpdate{
			.P = dummy,
			.Op = ParamUpdate::REMOVE });

		ReleaseSRWLockExclusive(&g_pApp->m_paramUpdateLock);
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

	Span<float> App::GetFrameTimeHistory() noexcept
	{
		auto& frameStats = g_pApp->m_frameTime;
		return frameStats.FrameTimeHist;
	}
}
