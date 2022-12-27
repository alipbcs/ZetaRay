#include "../App/App.h"
#include "../Support/MemoryPool.h"
#include "../Support/FrameMemory.h"
#include "../Utility/SynchronizedView.h"
#include "../App/Timer.h"
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
#include <xxHash/xxhash.h>
#include <ImGui/imgui.h>
#include <ImGui/implot.h>
#include <ImGui/imnodes.h>

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
		inline static constexpr const char* PSO_CACHE_DIR = "Assets\\PsoCache\\Debug";
		inline static constexpr const char* COMPILED_SHADER_DIR = "Assets\\CSO\\Debug";
#else
		inline static constexpr const char* PSO_CACHE_DIR = "Assets\\PsoCache\\Release";
		inline static constexpr const char* COMPILED_SHADER_DIR = "Assets\\CSO\\Release";
#endif // _DEBUG

		inline static constexpr const char* ASSET_DIR = "Assets";
		inline static constexpr const char* TOOLS_DIR = "Tools";
		inline static constexpr const char* DXC_PATH = "Tools\\dxc\\bin\\x64\\dxc.exe";
		inline static constexpr const char* RENDER_PASS_DIR = "ZetaRay\\RenderPass";
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
		ThreadPool m_workerThreadPool;
		ThreadPool m_backgroundThreadPool;
		SceneCore m_scene;
		Camera m_camera;

		THREAD_ID_TYPE alignas(64) m_threadIDs[MAX_NUM_THREADS];
		MemoryPool m_memoryPools[MAX_NUM_THREADS];
		RNG m_rng;
		FrameMemory m_frameMemory;
		int alignas(64) m_threadFrameAllocIndices[MAX_NUM_THREADS] = { -1 };
		std::atomic_int32_t m_currFrameAllocIndex;

		SmallVector<ParamVariant, ThreadAllocator> m_params;
		SmallVector<ParamUpdate, ThreadAllocator, 32> m_paramsUpdates;

		SmallVector<ShaderReloadHandler, ThreadAllocator> m_shaderReloadHandlers;
		SmallVector<Stat, FrameAllocator> m_frameStats;
		FrameTime m_frameTime;

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

	static AppData* g_app = nullptr;
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
		HWND hovered_window = g_app->m_hwnd;
		HWND mouse_window = NULL;
		if (hovered_window && (hovered_window == g_app->m_hwnd || IsChild(hovered_window, g_app->m_hwnd)))
			mouse_window = hovered_window;
		else if (focused_window && (focused_window == g_app->m_hwnd || IsChild(focused_window, g_app->m_hwnd)))
			mouse_window = focused_window;
		if (mouse_window == NULL)
			return;

		// Set OS mouse position from Dear ImGui if requested (rarely used, only when ImGuiConfigFlags_NavEnableSetMousePos is enabled by user)
		if (io.WantSetMousePos)
		{
			POINT pos = { (int)mouse_pos_prev.x, (int)mouse_pos_prev.y };
			if (ClientToScreen(g_app->m_hwnd, &pos))
				SetCursorPos(pos.x, pos.y);
		}

		// Set Dear ImGui mouse position from OS position
		POINT pos;
		if (GetCursorPos(&pos) && ScreenToClient(mouse_window, &pos))
			io.MousePos = ImVec2((float)pos.x, (float)pos.y);

		// Update OS mouse cursor with the cursor requested by imgui
		ImGuiMouseCursor mouse_cursor = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
		if (g_app->m_imguiCursor != mouse_cursor)
		{
			g_app->m_imguiCursor = mouse_cursor;
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

		style.ScaleAllSizes((float)g_app->m_dpi / 96.0f);
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
		g_app->m_frameStats.free_memory();

		const float frameTimeMs = (float)(g_app->m_timer.GetElapsedTime() * 1000.0);

		auto& frameStats = g_app->m_frameTime;
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
		CheckHR(g_app->m_renderer.GetAdapter()->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryInfo));

		//g_app->m_frameStats.emplace_back("Frame", "#Frames", g_app->m_timer.GetTotalFrameCount());
		g_app->m_frameStats.emplace_back("Frame", "FrameTime", (float)frameTimeMs);
		//g_app->m_frameStats.emplace_back("Frame", "FrameTime Avg.", (float) movingAvg);
		g_app->m_frameStats.emplace_back("Frame", "FPS", g_app->m_timer.GetFramesPerSecond());
		g_app->m_frameStats.emplace_back("GPU", "VRam Usage (MB)", memoryInfo.CurrentUsage >> 20);

		/*
		printf("----------------------\n");

		for (int i = 0; i < g_app->m_processorCoreCount; i++)
		{
			printf("Thread %d: %llu kb\n", i, g_app->m_threadContexts[i].MemPool.TotalSize() / 1024);
		}

		printf("----------------------\n\n");
		*/
	}

	void Update(TaskSet& sceneTS, TaskSet& sceneRendererTS) noexcept
	{
		UpdateStats();

		ImGuiUpdateMouse();
		ImGui::NewFrame();

		g_app->m_frameMotion.dt = (float)g_app->m_timer.GetElapsedTime();

		float scale = g_app->m_inMouseWheelMove ? g_app->m_inMouseWheelMove * 20 : 1.0f;
		scale = g_app->m_frameMotion.Acceleration.z != 0 || g_app->m_frameMotion.Acceleration.x != 0 ?
			fabsf(scale) : scale;

		// 'W'
		if (g_app->m_inMouseWheelMove || (GetAsyncKeyState(0x57) & (1 << 16)))
			g_app->m_frameMotion.Acceleration.z = 1;
		// 'A'
		if (GetAsyncKeyState(0x41) & (1 << 16))
			g_app->m_frameMotion.Acceleration.x = -1;
		// 'S'
		if (!g_app->m_inMouseWheelMove && (GetAsyncKeyState(0x53) & (1 << 16)))
			g_app->m_frameMotion.Acceleration.z = -1;
		// 'D'
		if (GetAsyncKeyState(0x44) & (1 << 16))
			g_app->m_frameMotion.Acceleration.x = 1;

		g_app->m_frameMotion.Acceleration.normalize(); 
		g_app->m_frameMotion.Acceleration *= g_app->m_cameraAcceleration * scale;
		g_app->m_inMouseWheelMove = 0;
		g_app->m_camera.Update(g_app->m_frameMotion);

		g_app->m_scene.Update(g_app->m_timer.GetElapsedTime(), sceneTS, sceneRendererTS);
	}

	void OnActivated() noexcept
	{
		g_app->m_timer.Resume();
		g_app->m_isActive = true;
	}

	void OnDeactivated() noexcept
	{
		g_app->m_timer.Pause();
		g_app->m_isActive = false;
	}

	void OnWindowSizeChanged() noexcept
	{
		if (g_app->m_timer.GetTotalFrameCount() > 0)
		{
			RECT rect;
			GetClientRect(g_app->m_hwnd, &rect);

			//int newWidth = (int)((rect.right - rect.left) * 96.0f / m_dpi);
			//int newHeight = (int)((rect.bottom - rect.top) * 96.0f / m_dpi);
			int newWidth = rect.right - rect.left;
			int newHeight = rect.bottom - rect.top;

			if (newWidth == g_app->m_displayWidth && newHeight == g_app->m_displayHeight)
				return;

			g_app->m_displayWidth = newWidth;
			g_app->m_displayHeight = newHeight;

			const float renderWidth = g_app->m_displayWidth / g_app->m_upscaleFactor;
			const float renderHeight = g_app->m_displayHeight / g_app->m_upscaleFactor;

			// following order is important
			g_app->m_renderer.OnWindowSizeChanged(g_app->m_hwnd, (int)renderWidth, (int)renderHeight,
				g_app->m_displayWidth, g_app->m_displayHeight);
			g_app->m_scene.OnWindowSizeChanged();

			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2((float)g_app->m_displayWidth, (float)g_app->m_displayHeight);
		}
	}

	void OnToggleFullscreenWindow() noexcept
	{
		// switch from windowed to full-screen
		if (!g_app->m_isFullScreen)
		{
			GetWindowRect(g_app->m_hwnd, &g_app->m_wndRectCache);

			// Make the window borderless so that the client area can fill the screen.
			SetWindowLong(g_app->m_hwnd, GWL_STYLE, WS_OVERLAPPED & ~(WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU | WS_THICKFRAME));

			RECT fullscreenWindowRect;

			// Get the settings of the display on which the app's window is currently displayed
			DXGI_OUTPUT_DESC desc = g_app->m_renderer.GetOutputMonitorDesc();
			fullscreenWindowRect = desc.DesktopCoordinates;

			SetWindowPos(g_app->m_hwnd,
				HWND_NOTOPMOST,
				fullscreenWindowRect.left,
				fullscreenWindowRect.top,
				fullscreenWindowRect.right,
				fullscreenWindowRect.bottom,
				SWP_FRAMECHANGED | SWP_NOACTIVATE);

			ShowWindow(g_app->m_hwnd, SW_MAXIMIZE);
		}
		else
		{
			// Restore the window's attributes and size.
			SetWindowLong(g_app->m_hwnd, GWL_STYLE, WS_OVERLAPPED);

			SetWindowPos(
				g_app->m_hwnd,
				HWND_NOTOPMOST,
				g_app->m_wndRectCache.left,
				g_app->m_wndRectCache.top,
				g_app->m_wndRectCache.right - g_app->m_wndRectCache.left,
				g_app->m_wndRectCache.bottom - g_app->m_wndRectCache.top,
				SWP_FRAMECHANGED | SWP_NOACTIVATE);

			ShowWindow(g_app->m_hwnd, SW_NORMAL);
		}

		printf("g_app->m_isFullScreen was: %d\n", g_app->m_isFullScreen);
		g_app->m_isFullScreen = !g_app->m_isFullScreen;
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
				//g_app->m_frameMotion.Acceleration.x = -g_app->m_cameraAcceleration;
				return;

				//D
			case 0x44:
				//g_app->m_frameMotion.Acceleration.x = g_app->m_cameraAcceleration;
				return;

				//W
			case 0x57:
				//printf("%llu, OnKeyboard(), repeat count: %lld\n", g_app->m_timer.GetTotalFrameCount(), lParam & 0xffff);
				//g_app->m_frameMotion.Acceleration.z = g_app->m_cameraAcceleration;
				return;

				//S
			case 0x53:
				//g_app->m_frameMotion.Acceleration.z = -g_app->m_cameraAcceleration;
				return;
			}
			*/

			// ALT+ENTER:
			/*
			auto& renderer = g_app->m_renderer;
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
			SetCapture(g_app->m_hwnd);

		ImGuiIO& io = ImGui::GetIO();
		//io.MouseDown[button] = true;
		io.AddMouseButtonEvent(button, true);

		if (!io.WantCaptureMouse)
		{
			int x = GET_X_LPARAM(lParam);
			int y = GET_Y_LPARAM(lParam);

			if (btnState == MK_LBUTTON)
			{
				SetCapture(g_app->m_hwnd);
				g_app->m_lastMousePosX = x;
				g_app->m_lastMousePosY = y;
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

		if (!ImGui::IsAnyMouseDown() && GetCapture() == g_app->m_hwnd)
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
		if (!g_app->m_imguiMouseTracked)
		{
			TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, g_app->m_hwnd, 0 };
			TrackMouseEvent(&tme);
			g_app->m_imguiMouseTracked = true;
		}

		io.AddMousePosEvent((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));

		if (!io.WantCaptureMouse)
		{
			if (btnState == MK_LBUTTON)
			{
				int x = GET_X_LPARAM(lParam);
				int y = GET_Y_LPARAM(lParam);

				//g_app->m_scene.GetCamera().RotateY(Math::DegreeToRadians((float)(x - g_app->m_lastMousePosX)));
				//g_app->m_scene.GetCamera().RotateX(Math::DegreeToRadians((float)(y - g_app->m_lastMousePosY)));

				g_app->m_frameMotion.RotationDegreesY = Math::DegreeToRadians((float)(x - g_app->m_lastMousePosX));
				g_app->m_frameMotion.RotationDegreesX = Math::DegreeToRadians((float)(y - g_app->m_lastMousePosY));

				g_app->m_lastMousePosX = x;
				g_app->m_lastMousePosY = y;
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
				g_app->m_inMouseWheelMove = 1;
			else
				g_app->m_inMouseWheelMove = -1;
		}
	}

	void OnDestroy() noexcept
	{
		ImGui::DestroyContext();
		ImPlot::DestroyContext();
		ImNodes::DestroyContext();

		App::FlushAllThreadPools();

		g_app->m_workerThreadPool.Shutdown();
		g_app->m_backgroundThreadPool.Shutdown();
		g_app->m_scene.Shutdown();
		g_app->m_renderer.Shutdown();
		g_app->m_params.clear();

		// TODO fix
		//for (int i = 0; i < m_processorCoreCount; i++)
		//	m_threadMemPools[i].Clear();

		delete g_app;
		g_app = nullptr;
	}

	void ApplyParamUpdates() noexcept
	{
		AcquireSRWLockExclusive(&g_app->m_paramUpdateLock);
		AcquireSRWLockExclusive(&g_app->m_paramLock);

		for (auto& p : g_app->m_paramsUpdates)
		{
			if (p.Op == ParamUpdate::OP_TYPE::ADD)
			{
				g_app->m_params.push_back(p.P);
			}
			else if (p.Op == ParamUpdate::OP_TYPE::REMOVE)
			{
				size_t i = 0;
				bool found = false;
				while (i < g_app->m_params.size())
				{
					if (g_app->m_params[i].GetID() == p.P.GetID())
					{
						found = true;
						break;
					}

					i++;
				}

				Assert(found, "parmeter {group: %s, subgroup: %s, name: %s} was not found.", p.P.GetGroup(), p.P.GetSubGroup(), p.P.GetName());
				g_app->m_params.erase(i);
			}
		}

		g_app->m_paramsUpdates.clear();

		ReleaseSRWLockExclusive(&g_app->m_paramLock);
		ReleaseSRWLockExclusive(&g_app->m_paramUpdateLock);
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
			g_app->m_inSizeMove = true;
			AppImpl::OnDeactivated();

			return 0;

		case WM_EXITSIZEMOVE:
			g_app->m_inSizeMove = false;
			AppImpl::OnWindowSizeChanged();
			AppImpl::OnActivated();

			return 0;

		case WM_SIZE:
			if (!g_app->m_inSizeMove)
			{
				if (wParam == SIZE_MINIMIZED)
				{
					g_app->m_minimized = true;
					AppImpl::OnDeactivated();
				}
				else if (wParam == SIZE_RESTORED)
				{
					if (g_app->m_minimized)
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
			g_app->m_dpi = HIWORD(wParam);

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

		g_app->m_hwnd = CreateWindowA(wndClassName,
			"ZetaRay",
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT,
			AppData::INITIAL_WINDOW_WIDTH, AppData::INITIAL_WINDOW_HEIGHT,
			nullptr, nullptr,
			instance,
			nullptr);

		CheckWin32(g_app->m_hwnd);

		SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		SetProcessDPIAware();
		g_app->m_dpi = GetDpiForWindow(g_app->m_hwnd);

		const int wndWidth = (int)((AppData::INITIAL_WINDOW_WIDTH * g_app->m_dpi) / 96.0f);
		const int wndHeight = (int)((AppData::INITIAL_WINDOW_HEIGHT * g_app->m_dpi) / 96.0f);

		SetWindowPos(g_app->m_hwnd, nullptr, 0, 0, wndWidth, wndHeight, 0);
		ShowWindow(g_app->m_hwnd, SW_SHOWNORMAL);
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
				g_app->m_processorCoreCount++;

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
		g_app->m_cameraAcceleration = p.GetFloat().m_val;
	}}

namespace ZetaRay
{
	ZetaInline int GetThreadIdx() noexcept
	{
		for (int i = 0; i < g_app->m_processorCoreCount + AppData::NUM_BACKGROUND_THREADS; i++)
		{
			if (g_app->m_threadIDs[i] == std::bit_cast<THREAD_ID_TYPE, std::thread::id>(std::this_thread::get_id()))
				return i;
		}

		return -1;
	}

	void RejoinBackgroundMemPoolsToWorkers() noexcept
	{
		for (int i = 0; i < AppData::NUM_BACKGROUND_THREADS; i++)
		{
			int sourceThreadIdx = g_app->m_processorCoreCount + i;
			int destThreadIdx = g_app->m_rng.GetUniformUintBounded(g_app->m_processorCoreCount);
			g_app->m_memoryPools[sourceThreadIdx].MoveTo(g_app->m_memoryPools[destThreadIdx]);
		}
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

		g_app = new AppData;

		AppImpl::GetProcessorInfo();

		// create the window
		AppImpl::CreateAppWindow(instance);
		SetWindowTextA(g_app->m_hwnd, "ZetaRay");

		const int totalNumThreads = g_app->m_processorCoreCount + AppData::NUM_BACKGROUND_THREADS;

		// initialize thread pools
		g_app->m_workerThreadPool.Init(g_app->m_processorCoreCount - 1,
			totalNumThreads,
			L"ZetaWorker",
			THREAD_PRIORITY::NORMAL);

		g_app->m_backgroundThreadPool.Init(AppData::NUM_BACKGROUND_THREADS,
			totalNumThreads,
			L"ZetaBackgroundWorker",
			THREAD_PRIORITY::BACKGROUND);

		// initialize frame allocators
		memset(g_app->m_threadFrameAllocIndices, -1, sizeof(int) * MAX_NUM_THREADS);
		g_app->m_currFrameAllocIndex.store(0, std::memory_order_release);

		// initialize memory pools. has to happen after thread pool has been created

		// main thread
		g_app->m_memoryPools[0].Init();
		g_app->m_threadIDs[0] = std::bit_cast<THREAD_ID_TYPE, std::thread::id>(std::this_thread::get_id());

		// worker threads
		auto workerThreadIDs = g_app->m_workerThreadPool.ThreadIDs();

		for (int i = 0; i < workerThreadIDs.size(); i++)
		{
			g_app->m_threadIDs[i + 1] = std::bit_cast<THREAD_ID_TYPE, std::thread::id>(workerThreadIDs[i]);
			g_app->m_memoryPools[i + 1].Init();
		}

		// background threads
		auto backgroundThreadIDs = g_app->m_backgroundThreadPool.ThreadIDs();

		for (int i = 0; i < backgroundThreadIDs.size(); i++)
		{
			g_app->m_threadIDs[workerThreadIDs.size() + 1 + i] = std::bit_cast<THREAD_ID_TYPE, std::thread::id>(backgroundThreadIDs[i]);
			g_app->m_memoryPools[workerThreadIDs.size() + 1 + i].Init();
		}

		uint64_t seed = std::bit_cast<uint64_t, void*>(g_app);
		g_app->m_rng = RNG(seed);

		//		g_app->m_mainThreadPool.SetThreadIds(Span(g_app->m_threadIDs, 1 + mainThreadIDs.size()));
		//		g_app->m_backgroundThreadPool.SetThreadIds(Span(g_app->m_threadIDs, 1 + mainThreadIDs.size() + backgroundThreadIDs.size()));

		g_app->m_workerThreadPool.Start();
		g_app->m_backgroundThreadPool.Start();

		RECT rect;
		GetClientRect(g_app->m_hwnd, &rect);

		g_app->m_displayWidth = rect.right - rect.left;
		g_app->m_displayHeight = rect.bottom - rect.top;

		AppImpl::InitImGui();

		const float renderWidth = g_app->m_displayWidth / g_app->m_upscaleFactor;
		const float renderHeight = g_app->m_displayHeight / g_app->m_upscaleFactor;

		// initialize renderer
		g_app->m_renderer.Init(g_app->m_hwnd, (int)renderWidth, (int)renderHeight, g_app->m_displayWidth, g_app->m_displayHeight);

		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2((float)g_app->m_displayWidth, (float)g_app->m_displayHeight);

		// initialize camera
		g_app->m_frameMotion.Reset();

		//m_camera.Init(float3(-10.61f, 4.67f, -3.25f), App::GetRenderer().GetAspectRatio(), 
		//	Math::DegreeToRadians(85.0f), 0.1f, true);
		g_app->m_camera.Init(float3(-5.61f, 4.67f, -0.25f), App::GetRenderer().GetAspectRatio(),
			Math::DegreeToRadians(75.0f), 0.1f, true);
		//m_camera.Init(float3(-1127.61f, 348.67f, 66.25f), App::GetRenderer().GetAspectRatio(), 
		//	Math::DegreeToRadians(85.0f), 10.0f, true);
		//m_camera.Init(float3(0.61f, 3.67f, 0.25f), App::GetRenderer().GetAspectRatio(), Math::DegreeToRadians(85.0f), 0.1f);

		// scene can now be initialized
		g_app->m_scene.Init();

		ParamVariant acc;
		acc.InitFloat("Scene", "Camera", "Acceleration", fastdelegate::FastDelegate1(&AppImpl::SetCameraAcceleration),
			g_app->m_cameraAcceleration,
			0.1f,
			100.0f,
			1.0f);
		App::AddParam(acc);

		g_app->m_isInitialized = true;
	}

	void App::InitSimple() noexcept
	{
		// main thread
		if (g_app == nullptr || !g_app->m_isInitialized)
		{
			g_app = new AppData;
			g_app->m_processorCoreCount = 1;

			g_app->m_isInitialized = true;

			g_app->m_memoryPools[0].Init();
			g_app->m_threadIDs[0] = std::bit_cast<THREAD_ID_TYPE, std::thread::id>(std::this_thread::get_id());
			//g_app->m_threadContexts[0].Lock = SRWLOCK_INIT;
			//g_app->m_threadContexts[0].Rng = RNG(g_app->m_threadIDs[0]);
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
				if (!g_app->m_isActive)
				{
					Sleep(16);
					continue;
				}

				// help out while there are (non-background) unfinished tasks from the previous frame
				bool success = g_app->m_workerThreadPool.TryFlush();

				// don't block the message-handling thread
				if (!success)
					continue;

				// begin frame
				g_app->m_renderer.BeginFrame();
				g_app->m_timer.Tick();

				// at this point, all worker tasks from the previous frame are done (GPU may still be executing those though)
				g_app->m_currTaskSignalIdx.store(0, std::memory_order_relaxed);

				g_app->m_currFrameAllocIndex.store(0, std::memory_order_release);
				memset(g_app->m_threadFrameAllocIndices, -1, sizeof(int) * MAX_NUM_THREADS);
				g_app->m_frameMemory.Reset();		// set the offset to 0; essentially freeing the memory

				// background tasks are not necessarily done 
				if (g_app->m_backgroundThreadPool.AreAllTasksFinished())
					RejoinBackgroundMemPoolsToWorkers();

				// update app
				{
					TaskSet appTS;

					appTS.EmplaceTask("AppUpdates", []()
						{
							AppImpl::ApplyParamUpdates();
						});

					appTS.Sort();
					appTS.Finalize();
					Submit(ZetaMove(appTS));
				}

				// update scene
				{
					TaskSet sceneTS;
					TaskSet sceneRendererTS;
					AppImpl::Update(sceneTS, sceneRendererTS);

					auto h0 = sceneRendererTS.EmplaceTask("ResourceUploadSubmission", []()
						{
							g_app->m_renderer.SubmitResourceCopies();
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
					success = g_app->m_workerThreadPool.TryFlush();

				g_app->m_frameMotion.Reset();

				// render
				{
					TaskSet renderTS;
					TaskSet endFrameTS;

					g_app->m_scene.Render(renderTS);
					renderTS.Sort();

					// end-frame
					{
						g_app->m_renderer.EndFrame(endFrameTS);

						endFrameTS.EmplaceTask("Scene::Recycle", []()
							{
								g_app->m_scene.Recycle();
							});

						endFrameTS.Sort();
						renderTS.ConnectTo(endFrameTS);

						renderTS.Finalize();
						endFrameTS.Finalize();
					}

					Submit(ZetaMove(renderTS));
					Submit(ZetaMove(endFrameTS));
				}

				g_app->m_workerThreadPool.PumpUntilEmpty();
			}
		}
		
		return (int)msg.wParam;
	}

	void App::Abort() noexcept
	{
		AppImpl::OnDestroy();
		PostQuitMessage(0);
	}

	void* App::AllocateFromFrameAllocator(size_t size, size_t alignment) noexcept
	{
		alignment = std::max(alignof(std::max_align_t), alignment);

		// at most alignment - 1 extra bytes are required
		Assert(size + alignment - 1 <= FrameMemory::BLOCK_SIZE, "allocations larger than FrameMemory::BLOCK_SIZE are not possible with FrameAllocator.");

		const int threadIdx = GetThreadIdx();
		Assert(threadIdx != -1, "thread idx was not found");

		// current memory block has enough space
		int allocIdx = g_app->m_threadFrameAllocIndices[threadIdx];

		// first time in this frame
		if (allocIdx != -1)
		{
			FrameMemory::MemoryBlock& block = g_app->m_frameMemory.GetAndInitIfEmpty(allocIdx);

			const uintptr_t start = reinterpret_cast<uintptr_t>(block.Start);
			const uintptr_t ret = Math::AlignUp(start + block.Offset, alignment);
			const uintptr_t startOffset = ret - start;

			if (startOffset + size < FrameMemory::BLOCK_SIZE)
			{
				block.Offset = startOffset + size;
				return reinterpret_cast<void*>(ret);
			}
		}

		// allocate/reuse a new block
		allocIdx = g_app->m_currFrameAllocIndex.fetch_add(1, std::memory_order_relaxed);
		g_app->m_threadFrameAllocIndices[threadIdx] = allocIdx;
		FrameMemory::MemoryBlock& block = g_app->m_frameMemory.GetAndInitIfEmpty(allocIdx);
		Assert(block.Offset == 0, "block offset should be initially 0");

		const uintptr_t start = reinterpret_cast<uintptr_t>(block.Start);
		const uintptr_t ret = Math::AlignUp(start, alignment);
		const uintptr_t startOffset = ret - start;

		Assert(startOffset + size < FrameMemory::BLOCK_SIZE, "should never happen.");
		block.Offset = startOffset + size;

		return reinterpret_cast<void*>(ret);
	}

	void* App::AllocateFromMemoryPool(size_t size, size_t alignment) noexcept
	{
		//return malloc(size);

		const int idx = GetThreadIdx();
		Assert(idx != -1, "thread idx was not found");
		void* mem = g_app->m_memoryPools[idx].AllocateAligned(size, alignment);

		return mem;
	}

	void App::FreeMemoryPool(void* mem, size_t size, size_t alignment) noexcept
	{
		//free(mem);

		const int idx = GetThreadIdx();
		Assert(idx != -1, "thread idx was not found");
		g_app->m_memoryPools[idx].FreeAligned(mem, size, alignment);
	}

	int App::RegisterTask() noexcept
	{
		int idx = g_app->m_currTaskSignalIdx.fetch_add(1, std::memory_order_relaxed);
		Assert(idx < AppData::MAX_NUM_TASKS_PER_FRAME, "number of task signals exceeded MAX_NUM_TASKS_PER_FRAME");

		return idx;
	}

	void App::TaskFinalizedCallback(int handle, int indegree) noexcept
	{
		Assert(indegree > 0, "unnecessary call.");
		const int c = g_app->m_currTaskSignalIdx.load(std::memory_order_relaxed);
		Assert(handle < c, "received handle %d while #handles for current frame is %d", c);

		g_app->m_registeredTasks[handle].Indegree.store(indegree, std::memory_order_release);
		g_app->m_registeredTasks[handle].BlockFlag.store(true, std::memory_order_release);
	}

	void App::WaitForAdjacentHeadNodes(int handle) noexcept
	{
		const int c = g_app->m_currTaskSignalIdx.load(std::memory_order_relaxed);
		Assert(handle >= 0 && handle < c, "received handle %d while #handles for current frame is %d", c);

		auto& taskSignal = g_app->m_registeredTasks[handle];
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

			auto& taskSignal = g_app->m_registeredTasks[handle];
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
		g_app->m_workerThreadPool.Enqueue(ZetaMove(t));
	}

	void App::Submit(TaskSet&& ts) noexcept
	{
		g_app->m_workerThreadPool.Enqueue(ZetaMove(ts));
	}

	void App::SubmitBackground(Task&& t) noexcept
	{
		Assert(t.GetPriority() == TASK_PRIORITY::BACKGRUND, "Normal task is not allowed to be executed in background thread-pool");
		g_app->m_backgroundThreadPool.Enqueue(ZetaMove(t));
	}

	void App::FlushWorkerThreadPool() noexcept
	{
		bool success = false;
		while (!success)
			success = g_app->m_workerThreadPool.TryFlush();
	}

	void App::FlushAllThreadPools() noexcept
	{
		bool success = false;
		while (!success)
			success = g_app->m_workerThreadPool.TryFlush();

		success = false;
		while (!success)
			success = g_app->m_backgroundThreadPool.TryFlush();
	}

	Renderer& App::GetRenderer() noexcept { return g_app->m_renderer; }
	SceneCore& App::GetScene() noexcept { return g_app->m_scene; }
	const Camera& App::GetCamera() noexcept { return g_app->m_camera; }
	int App::GetNumWorkerThreads() noexcept { return g_app->m_processorCoreCount; }
	int App::GetNumBackgroundThreads() noexcept { return AppData::NUM_BACKGROUND_THREADS; }
	uint32_t App::GetDPI() noexcept { return g_app->m_dpi; }
	float App::GetUpscalingFactor() noexcept { return g_app->m_upscaleFactor; }
	bool App::IsFullScreen() noexcept { return g_app->m_isFullScreen; }
	const App::Timer& App::GetTimer() noexcept { return g_app->m_timer; }
	const char* App::GetPSOCacheDir() noexcept { return AppData::PSO_CACHE_DIR; }
	const char* App::GetCompileShadersDir() noexcept { return AppData::COMPILED_SHADER_DIR; }
	const char* App::GetAssetDir() noexcept { return AppData::ASSET_DIR; }
	const char* App::GetDXCPath() noexcept { return AppData::DXC_PATH; }
	const char* App::GetToolsDir() noexcept { return AppData::TOOLS_DIR; }
	const char* App::GetRenderPassDir() noexcept { return AppData::RENDER_PASS_DIR; }

	void App::SetUpscalingEnablement(bool e) noexcept
	{
		const float oldScaleFactor = g_app->m_upscaleFactor;

		if (e)
			g_app->m_upscaleFactor = 1.5f;
		else
			g_app->m_upscaleFactor = 1.0f;

		if (oldScaleFactor == g_app->m_upscaleFactor)
			return;

		const float renderWidth = g_app->m_displayWidth / g_app->m_upscaleFactor;
		const float renderHeight = g_app->m_displayHeight / g_app->m_upscaleFactor;

		g_app->m_renderer.OnWindowSizeChanged(g_app->m_hwnd, (int)renderWidth, (int)renderHeight, g_app->m_displayWidth, g_app->m_displayHeight);
		g_app->m_scene.OnWindowSizeChanged();
	}

	void App::LockStdOut() noexcept
	{
		if(g_app)
			AcquireSRWLockExclusive(&g_app->m_stdOutLock);
	}

	void App::UnlockStdOut() noexcept
	{
		if(g_app)
			ReleaseSRWLockExclusive(&g_app->m_stdOutLock);
	}

	Span<uint32_t> App::GetWorkerThreadIDs() noexcept
	{
		return Span(g_app->m_threadIDs, g_app->m_processorCoreCount);
	}

	Span<uint32_t> App::GetBackgroundThreadIDs() noexcept
	{
		return Span(g_app->m_threadIDs + g_app->m_processorCoreCount, AppData::NUM_BACKGROUND_THREADS);
	}

	Span<uint32_t> App::GetAllThreadIDs() noexcept
	{
		return Span(g_app->m_threadIDs, g_app->m_processorCoreCount + AppData::NUM_BACKGROUND_THREADS);
	}

	RWSynchronizedView<Vector<ParamVariant, ThreadAllocator>> App::GetParams() noexcept
	{
		return RWSynchronizedView<Vector<ParamVariant, ThreadAllocator>>(g_app->m_params, g_app->m_paramLock);
	}

	RSynchronizedView<Vector<ShaderReloadHandler, ThreadAllocator>> App::GetShaderReloadHandlers() noexcept
	{
		return RSynchronizedView<Vector<ShaderReloadHandler, ThreadAllocator>>(g_app->m_shaderReloadHandlers, g_app->m_shaderReloadLock);
	}

	RWSynchronizedView<Vector<Stat, FrameAllocator>> App::GetStats() noexcept
	{
		return RWSynchronizedView<Vector<Stat, FrameAllocator>>(g_app->m_frameStats, g_app->m_statsLock);
	}

	void App::AddParam(ParamVariant& p) noexcept
	{
		AcquireSRWLockExclusive(&g_app->m_paramUpdateLock);

		g_app->m_paramsUpdates.push_back(ParamUpdate{
			.P = p,
			.Op = ParamUpdate::ADD });

		ReleaseSRWLockExclusive(&g_app->m_paramUpdateLock);
	}

	void App::RemoveParam(const char* group, const char* subgroup, const char* name) noexcept
	{
		AcquireSRWLockExclusive(&g_app->m_paramUpdateLock);

		// create a dummy ParamVariant (never exposed to outside)
		ParamVariant dummy;
		dummy.InitBool(group, subgroup, name, fastdelegate::FastDelegate1<const ParamVariant&>(), false);

		g_app->m_paramsUpdates.push_back(ParamUpdate{
			.P = dummy,
			.Op = ParamUpdate::REMOVE });

		ReleaseSRWLockExclusive(&g_app->m_paramUpdateLock);
	}

	void App::AddShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg) noexcept
	{
		AcquireSRWLockExclusive(&g_app->m_shaderReloadLock);
		g_app->m_shaderReloadHandlers.emplace_back(name, dlg);
		ReleaseSRWLockExclusive(&g_app->m_shaderReloadLock);
	}

	void App::RemoveShaderReloadHandler(const char* name) noexcept
	{
		uint64_t id = XXH3_64bits(name, std::min(ShaderReloadHandler::MAX_LEN, (int)strlen(name) + 1));

		AcquireSRWLockExclusive(&g_app->m_shaderReloadLock);
		size_t i = 0;
		bool found = false;

		for (i = 0; i < g_app->m_shaderReloadHandlers.size(); i++)
		{
			if (g_app->m_shaderReloadHandlers[i].ID == id)
			{
				found = true;
				break;
			}
		}

		if (found)
			g_app->m_shaderReloadHandlers.erase(i);

		ReleaseSRWLockExclusive(&g_app->m_shaderReloadLock);
	}

	void App::AddFrameStat(const char* group, const char* name, int i) noexcept
	{
		AcquireSRWLockExclusive(&g_app->m_statsLock);
		g_app->m_frameStats.emplace_back(group, name, i);
		ReleaseSRWLockExclusive(&g_app->m_statsLock);
	}

	void App::AddFrameStat(const char* group, const char* name, uint32_t u) noexcept
	{
		AcquireSRWLockExclusive(&g_app->m_statsLock);
		g_app->m_frameStats.emplace_back(group, name, u);
		ReleaseSRWLockExclusive(&g_app->m_statsLock);
	}

	void App::AddFrameStat(const char* group, const char* name, float f) noexcept
	{
		AcquireSRWLockExclusive(&g_app->m_statsLock);
		g_app->m_frameStats.emplace_back(group, name, f);
		ReleaseSRWLockExclusive(&g_app->m_statsLock);
	}

	void App::AddFrameStat(const char* group, const char* name, uint64_t u) noexcept
	{
		AcquireSRWLockExclusive(&g_app->m_statsLock);
		g_app->m_frameStats.emplace_back(group, name, u);
		ReleaseSRWLockExclusive(&g_app->m_statsLock);
	}

	void App::AddFrameStat(const char* group, const char* name, uint32_t num, uint32_t total) noexcept
	{
		AcquireSRWLockExclusive(&g_app->m_statsLock);
		g_app->m_frameStats.emplace_back(group, name, num, total);
		ReleaseSRWLockExclusive(&g_app->m_statsLock);
	}

	Span<float> App::GetFrameTimeHistory() noexcept
	{
		auto& frameStats = g_app->m_frameTime;
		return frameStats.FrameTimeHist;
	}
}
