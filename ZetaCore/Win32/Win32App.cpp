#include "../App/Log.h"
#include "../Support/MemoryPool.h"
#include "../Support/FrameMemory.h"
#include "../Utility/SynchronizedView.h"
#include "../App/Timer.h"
#include "../App/Common.h"
#include "../Support/Param.h"
#include "../Support/Stat.h"
#include "../Utility/HashTable.h"
#include "../Utility/Span.h"
#include "../Core/RendererCore.h"
#include "../Core/CommandQueue.h"			// just for std::unique_ptr<>
#include "../Core/SharedShaderResources.h"	// just for std::unique_ptr<>
#include "../Scene/SceneCore.h"
#include "../Scene/Camera.h"
#include "../Support/ThreadPool.h"
#include "../Assets/Font/Font.h"
#include <atomic>

#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION 
#include <xxHash/xxhash.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <ImGui/imgui.h>
#include <ImGui/implot.h>
#include <ImGui/imnodes.h>

#include <Uxtheme.h>	// for HTHEME

using namespace ZetaRay::App;
using namespace ZetaRay::App::Common;
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

	// Ref: https://github.com/ysc3839/win32-darkmode
	enum PreferredAppMode
	{
		Default,
		AllowDark,
		ForceDark,
		ForceLight,
		Max
	};

	enum WINDOWCOMPOSITIONATTRIB
	{
		WCA_UNDEFINED = 0,
		WCA_NCRENDERING_ENABLED = 1,
		WCA_NCRENDERING_POLICY = 2,
		WCA_TRANSITIONS_FORCEDISABLED = 3,
		WCA_ALLOW_NCPAINT = 4,
		WCA_CAPTION_BUTTON_BOUNDS = 5,
		WCA_NONCLIENT_RTL_LAYOUT = 6,
		WCA_FORCE_ICONIC_REPRESENTATION = 7,
		WCA_EXTENDED_FRAME_BOUNDS = 8,
		WCA_HAS_ICONIC_BITMAP = 9,
		WCA_THEME_ATTRIBUTES = 10,
		WCA_NCRENDERING_EXILED = 11,
		WCA_NCADORNMENTINFO = 12,
		WCA_EXCLUDED_FROM_LIVEPREVIEW = 13,
		WCA_VIDEO_OVERLAY_ACTIVE = 14,
		WCA_FORCE_ACTIVEWINDOW_APPEARANCE = 15,
		WCA_DISALLOW_PEEK = 16,
		WCA_CLOAK = 17,
		WCA_CLOAKED = 18,
		WCA_ACCENT_POLICY = 19,
		WCA_FREEZE_REPRESENTATION = 20,
		WCA_EVER_UNCLOAKED = 21,
		WCA_VISUAL_OWNER = 22,
		WCA_HOLOGRAPHIC = 23,
		WCA_EXCLUDED_FROM_DDA = 24,
		WCA_PASSIVEUPDATEMODE = 25,
		WCA_USEDARKMODECOLORS = 26,
		WCA_LAST = 27
	};

	struct WINDOWCOMPOSITIONATTRIBDATA
	{
		WINDOWCOMPOSITIONATTRIB Attrib;
		PVOID pvData;
		SIZE_T cbData;
	};

	using fnSetWindowCompositionAttribute = BOOL(WINAPI*)(HWND hWnd, WINDOWCOMPOSITIONATTRIBDATA*);
	// 1809 17763
	using fnShouldAppsUseDarkMode = bool (WINAPI*)(); // ordinal 132
	using fnAllowDarkModeForWindow = bool (WINAPI*)(HWND hWnd, bool allow); // ordinal 133
	using fnRefreshImmersiveColorPolicyState = void (WINAPI*)(); // ordinal 104
	using fnIsDarkModeAllowedForWindow = bool (WINAPI*)(HWND hWnd); // ordinal 137
	using fnOpenNcThemeData = HTHEME(WINAPI*)(HWND hWnd, LPCWSTR pszClassList); // ordinal 49
	// 1903 18362
	using fnShouldSystemUseDarkMode = bool (WINAPI*)(); // ordinal 138
	using fnSetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode appMode); // ordinal 135, in 1903
	using fnIsDarkModeAllowedForApp = bool (WINAPI*)(); // ordinal 139
}

namespace
{
	struct FrameMemoryContext
	{
		int alignas(64) m_threadFrameAllocIndices[MAX_NUM_THREADS] = { -1 };
		std::atomic_int32_t m_currFrameAllocIndex;
	};

	struct AppData
	{
		inline static constexpr const char* PSO_CACHE_PARENT = "..\\Assets\\PsoCache";
#if defined(_DEBUG)
		inline static constexpr const char* PSO_CACHE_DIR = "..\\Assets\\PsoCache\\Debug";
		inline static constexpr const char* COMPILED_SHADER_DIR = "..\\Assets\\CSO\\Debug";
#else
		inline static constexpr const char* PSO_CACHE_DIR = "..\\Assets\\PsoCache\\Release";
		inline static constexpr const char* COMPILED_SHADER_DIR = "..\\Assets\\CSO\\Release";
#endif // _DEBUG

		inline static constexpr const char* ASSET_DIR = "..\\Assets";
		inline static constexpr const char* TOOLS_DIR = "..\\Tools";
		inline static constexpr const char* DXC_PATH = "..\\Tools\\dxc\\bin\\x64\\dxc.exe";
		inline static constexpr const char* RENDER_PASS_DIR = "..\\ZetaRenderPass";
		static constexpr int NUM_BACKGROUND_THREADS = 2;
		static constexpr int MAX_NUM_TASKS_PER_FRAME = 256;

		int m_processorCoreCount = 0;
		HWND m_hwnd;
		RECT m_wndRectCache;
		int m_displayWidth;
		int m_displayHeight;
		bool m_isActive = true;
		bool m_manuallyPaused = false;
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
		float m_cameraAcceleration = 40.0f;

		Timer m_timer;
		RendererCore m_renderer;
		ThreadPool m_workerThreadPool;
		ThreadPool m_backgroundThreadPool;
		SceneCore m_scene;
		Camera m_camera;

		THREAD_ID_TYPE alignas(64) m_threadIDs[MAX_NUM_THREADS];

		FrameMemory<512 * 1024> m_smallFrameMemory;
		FrameMemory<5 * 1024 * 1024> m_largeFrameMemory;
		FrameMemoryContext m_smallFrameMemoryContext;
		FrameMemoryContext m_largeFrameMemoryContext;

		SmallVector<ParamVariant> m_params;
		SmallVector<ParamUpdate, SystemAllocator, 32> m_paramsUpdates;

		SmallVector<ShaderReloadHandler> m_shaderReloadHandlers;
		SmallVector<Stat, FrameAllocator> m_frameStats;
		FrameTime m_frameTime;

		SRWLOCK m_stdOutLock = SRWLOCK_INIT;
		SRWLOCK m_paramLock = SRWLOCK_INIT;
		SRWLOCK m_paramUpdateLock = SRWLOCK_INIT;
		SRWLOCK m_shaderReloadLock = SRWLOCK_INIT;
		SRWLOCK m_statsLock = SRWLOCK_INIT;
		SRWLOCK m_logLock = SRWLOCK_INIT;

		struct alignas(64) TaskSignal
		{
			std::atomic_int32_t Indegree;
			std::atomic_bool BlockFlag;
		};

		TaskSignal m_registeredTasks[MAX_NUM_TASKS_PER_FRAME];
		std::atomic_int32_t m_currTaskSignalIdx = 0;

		bool m_isInitialized = false;

		Motion m_frameMotion;
		SmallVector<LogMessage, FrameAllocator> m_frameLogs;

		fastdelegate::FastDelegate0<> m_rebuildFontTexDlg;

		bool m_issueResize = false;
	};

	AppData* g_app = nullptr;
}

namespace ZetaRay::AppImpl
{
	void LoadFont() noexcept
	{
		ImGuiIO& io = ImGui::GetIO();
		io.Fonts->Clear();

		using getFontFP = FontSpan(*)(FONT_TYPE f);
		HINSTANCE fontLib = LoadLibraryExA("Font", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
		CheckWin32(fontLib);

		auto fpGetFont = reinterpret_cast<getFontFP>(GetProcAddress(fontLib, "GetFont"));
		CheckWin32(fpGetFont);

		constexpr auto fontType = FONT_TYPE::ROBOTO_REGULAR;
		FontSpan f = fpGetFont(fontType);
		Check(f.Data, "font was not found.");

		float fontSizePixels96;

		if constexpr (fontType == FONT_TYPE::SEGOE_UI)
			fontSizePixels96 = 13.8f;
		else if constexpr (fontType == FONT_TYPE::ROBOTO_REGULAR)
			fontSizePixels96 = 12.8f;
		else if constexpr (fontType == FONT_TYPE::DOMINE_MEDIUM)
			fontSizePixels96 = 12.0f;
		else
			fontSizePixels96 = 13.6f;

		const float fontSizePixelsDPI = ((float)g_app->m_dpi / USER_DEFAULT_SCREEN_DPI) * fontSizePixels96;
		ImFont* font = io.Fonts->AddFontFromMemoryCompressedBase85TTF(f.Data, fontSizePixelsDPI);

		FreeLibrary(fontLib);

		// font texture is always built in the first frame
		if (g_app->m_timer.GetTotalFrameCount() > 0)
		{
			Assert(!g_app->m_rebuildFontTexDlg.empty(), "delegate hasn't been set.");
			g_app->m_rebuildFontTexDlg();
		}
	}

	void OnActivated() noexcept
	{
		g_app->m_timer.Resume();
		g_app->m_isActive = true;
		SetWindowTextA(g_app->m_hwnd, "ZetaRay");
	}

	void OnDeactivated() noexcept
	{
		g_app->m_timer.Pause();
		g_app->m_isActive = false;
		SetWindowTextA(g_app->m_hwnd, "ZetaRay (Paused - press 'P' to resume)");
	}

	void OnDPIChanged(int newDPI, const RECT* newRect) noexcept
	{
		g_app->m_dpi = newDPI;

		SetWindowPos(g_app->m_hwnd, nullptr,
			newRect->left,
			newRect->top,
			newRect->right - newRect->left,
			newRect->bottom - newRect->top,
			SWP_NOZORDER | SWP_NOACTIVATE);

		LoadFont();

		ImGuiStyle& style = ImGui::GetStyle();
		style.ScaleAllSizes((float)g_app->m_dpi / USER_DEFAULT_SCREEN_DPI);
	}

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

		colors[ImGuiCol_WindowBg] = ImVec4(0.012286487f, 0.012286487f, 0.012286487f, 1.0f);
		colors[ImGuiCol_Border] = ImVec4(1.0f / 255, 1.0f / 255, 1.1f / 255, 0.0f);
		colors[ImGuiCol_TitleBg] = ImVec4(26 / 255.0f, 26 / 255.0f, 26 / 255.0f, 1.0f);
		colors[ImGuiCol_Tab] = ImVec4(0.046665083f, 0.046665083f, 0.046665083f, 1.0f);
		colors[ImGuiCol_TabHovered] = ImVec4(40 / 255.0f, 42 / 255.0f, 47 / 255.0f, 1.0f);
		colors[ImGuiCol_TabActive] = ImVec4(7 / 255.0f, 26 / 255.0f, 56 / 255.0f, 1.0f);
		colors[ImGuiCol_TitleBg] = colors[ImGuiCol_Tab];
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.08865560f, 0.08865560f, 0.08865560f, 1.0f);
		colors[ImGuiCol_FrameBg] = ImVec4(10 / 255.0f, 10 / 255.0f, 10 / 255.0f, 1.0f);
		colors[ImGuiCol_Header] = ImVec4(0.046665083f, 0.046665083f, 0.046665083f, 1.0f);
		colors[ImGuiCol_HeaderActive] = colors[ImGuiCol_WindowBg];
		colors[ImGuiCol_HeaderHovered] = ImVec4(33 / 255.0f, 33 / 255.0f, 33 / 255.0f, 1.0f);

		style.FramePadding = ImVec2(7.0f, 3.0f);
		style.GrabMinSize = 13.0f;
		style.FrameRounding = 12.0f;
		style.GrabRounding = style.FrameRounding;
		style.ItemSpacing = ImVec2(8.0f, 7.0f);

		style.ScaleAllSizes((float)g_app->m_dpi / USER_DEFAULT_SCREEN_DPI);

		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2((float)g_app->m_displayWidth, (float)g_app->m_displayHeight);

		// TODO remove hard-coded path
		io.IniFilename = "..//temp//imgui.ini";

		io.UserData = &g_app->m_rebuildFontTexDlg;
		LoadFont();
	}

	void UpdateStats() noexcept
	{
		g_app->m_frameStats.free_memory();

		const float frameTimeMs = g_app->m_timer.GetTotalFrameCount() > 1 ?
			(float)(g_app->m_timer.GetElapsedTime() * 1000.0f) : 
			0.0f;

		auto& frameStats = g_app->m_frameTime;

		if (frameStats.NextFramHistIdx < frameStats.HIST_LEN)
			frameStats.FrameTimeHist[frameStats.NextFramHistIdx++] = frameTimeMs;
		else
		{
			// shift left
			for (int i = 0; i < frameStats.HIST_LEN - 1; i++)
				frameStats.FrameTimeHist[i] = frameStats.FrameTimeHist[i + 1];

			frameStats.FrameTimeHist[frameStats.HIST_LEN - 1] = frameTimeMs;
		}

		DXGI_QUERY_VIDEO_MEMORY_INFO memoryInfo = {};
		CheckHR(g_app->m_renderer.GetAdapter()->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryInfo));

		if (memoryInfo.CurrentUsage > memoryInfo.Budget)
			LOG_UI_WARNING("VRam usage exceeded available budget; performance can be severely impacted.");

		g_app->m_frameStats.emplace_back("Frame", "FPS", g_app->m_timer.GetFramesPerSecond());
		g_app->m_frameStats.emplace_back("GPU", "VRam Usage (MB)", memoryInfo.CurrentUsage >> 20);
		g_app->m_frameStats.emplace_back("GPU", "VRam Budget (MB)", memoryInfo.Budget >> 20);
	}

	void Update(TaskSet& sceneTS, TaskSet& sceneRendererTS) noexcept
	{
		UpdateStats();

		if (g_app->m_timer.GetTotalFrameCount() > 1)
			g_app->m_frameLogs.free_memory();

		ImGuiUpdateMouse();
		ImGui::NewFrame();

		g_app->m_frameMotion.dt = (float)g_app->m_timer.GetElapsedTime();

		float scale = g_app->m_inMouseWheelMove ? g_app->m_inMouseWheelMove * 20 : 1.0f;
		scale = g_app->m_frameMotion.Acceleration.z != 0 || g_app->m_frameMotion.Acceleration.x != 0 ?
			fabsf(scale) : scale;

		if (g_app->m_inMouseWheelMove || (GetAsyncKeyState('W') & (1 << 16)))
			g_app->m_frameMotion.Acceleration.z = 1;
		if (GetAsyncKeyState('A') & (1 << 16))
			g_app->m_frameMotion.Acceleration.x = -1;
		if (!g_app->m_inMouseWheelMove && (GetAsyncKeyState('S') & (1 << 16)))
			g_app->m_frameMotion.Acceleration.z = -1;
		if (GetAsyncKeyState('D') & (1 << 16))
			g_app->m_frameMotion.Acceleration.x = 1;

		g_app->m_frameMotion.Acceleration.normalize();
		g_app->m_frameMotion.Acceleration *= g_app->m_cameraAcceleration * scale;
		g_app->m_inMouseWheelMove = 0;
		g_app->m_camera.Update(g_app->m_frameMotion);

		g_app->m_scene.Update(g_app->m_timer.GetElapsedTime(), sceneTS, sceneRendererTS);
	}

	void OnWindowSizeChanged() noexcept
	{
		if (g_app->m_timer.GetTotalFrameCount() > 0)
		{
			RECT rect;
			GetClientRect(g_app->m_hwnd, &rect);

			const int newWidth = rect.right - rect.left;
			const int newHeight = rect.bottom - rect.top;

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
			if (GetAsyncKeyState('P') & (1 << 16))
			{
				if (g_app->m_isActive)
				{
					g_app->m_manuallyPaused = true;
					AppImpl::OnDeactivated();
				}
				else
				{
					g_app->m_manuallyPaused = false;
					AppImpl::OnActivated();
				}
			}

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

		g_app->m_scene.Shutdown();
		g_app->m_renderer.Shutdown();
		g_app->m_params.free_memory();
		g_app->m_workerThreadPool.Shutdown();
		g_app->m_backgroundThreadPool.Shutdown();

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
				g_app->m_params.push_back(p.P);
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

				Assert(found, "parameter {group: %s, subgroup: %s, name: %s} was not found.", p.P.GetGroup(), p.P.GetSubGroup(), p.P.GetName());
				if(found)
					g_app->m_params.erase(i);
			}		
		}

		g_app->m_paramsUpdates.clear();

		ReleaseSRWLockExclusive(&g_app->m_paramLock);
		ReleaseSRWLockExclusive(&g_app->m_paramUpdateLock);
	}

	// Ref: https://github.com/ysc3839/win32-darkmode
	bool TryInitDarkMode(HMODULE* uxthemeLib) noexcept
	{
		bool darkModeEnabled = false;

		*uxthemeLib = LoadLibraryExA("uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (*uxthemeLib)
		{
			fnOpenNcThemeData openNcThemeData = reinterpret_cast<fnOpenNcThemeData>(GetProcAddress(*uxthemeLib, MAKEINTRESOURCEA(49)));
			fnRefreshImmersiveColorPolicyState refreshImmersiveColorPolicyState = reinterpret_cast<fnRefreshImmersiveColorPolicyState>(
				GetProcAddress(*uxthemeLib, MAKEINTRESOURCEA(104)));
			fnShouldAppsUseDarkMode shouldAppsUseDarkMode = reinterpret_cast<fnShouldAppsUseDarkMode>(
				GetProcAddress(*uxthemeLib, MAKEINTRESOURCEA(132)));
			fnAllowDarkModeForWindow allowDarkModeForWindow = reinterpret_cast<fnAllowDarkModeForWindow>(
				GetProcAddress(*uxthemeLib, MAKEINTRESOURCEA(133)));

			auto ord135 = GetProcAddress(*uxthemeLib, MAKEINTRESOURCEA(135));
			fnSetPreferredAppMode setPreferredAppMode = reinterpret_cast<fnSetPreferredAppMode>(ord135);

			fnIsDarkModeAllowedForWindow isDarkModeAllowedForWindow = reinterpret_cast<fnIsDarkModeAllowedForWindow>(
				GetProcAddress(*uxthemeLib, MAKEINTRESOURCEA(137)));

			if (openNcThemeData &&
				refreshImmersiveColorPolicyState &&
				shouldAppsUseDarkMode &&
				allowDarkModeForWindow &&
				setPreferredAppMode &&
				isDarkModeAllowedForWindow)
			{
				setPreferredAppMode(PreferredAppMode::AllowDark);

				refreshImmersiveColorPolicyState();

				bool isHighContrast = false;
				HIGHCONTRASTW highContrast = { sizeof(highContrast) };
				if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, FALSE))
					isHighContrast = highContrast.dwFlags & HCF_HIGHCONTRASTON;

				darkModeEnabled = shouldAppsUseDarkMode() && !isHighContrast;
			}
		}

		return darkModeEnabled;
	}

	LRESULT WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
	{
		switch (message)
		{
		case WM_CREATE:
		{
			HMODULE uxthemeLib = nullptr;
			BOOL dark = TryInitDarkMode(&uxthemeLib);

			fnSetWindowCompositionAttribute setWindowCompositionAttribute = reinterpret_cast<fnSetWindowCompositionAttribute>(
				GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowCompositionAttribute"));
			if (setWindowCompositionAttribute)
			{
				WINDOWCOMPOSITIONATTRIBDATA data = { WCA_USEDARKMODECOLORS, &dark, sizeof(dark) };
				setWindowCompositionAttribute(hWnd, &data);
			}

			FreeLibrary(uxthemeLib);
		}

			return 0;

		case WM_ACTIVATEAPP:
			if (wParam && !g_app->m_manuallyPaused)
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

			if (!g_app->m_manuallyPaused)
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
					if (g_app->m_minimized && !g_app->m_manuallyPaused)
						AppImpl::OnActivated();

					AppImpl::OnWindowSizeChanged();
				}
				else if (wParam == SIZE_MAXIMIZED)
					AppImpl::OnWindowSizeChanged();
			}

			return 0;

		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
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

		case WM_DPICHANGED:
		{
			OnDPIChanged(HIWORD(wParam), reinterpret_cast<RECT*>(lParam));
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
			CW_USEDEFAULT, CW_USEDEFAULT,
			nullptr, nullptr,
			instance,
			nullptr);

		CheckWin32(g_app->m_hwnd);

		RECT workingArea;
		CheckWin32(SystemParametersInfoA(SPI_GETWORKAREA, 0, &workingArea, 0));

		CheckWin32(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
		g_app->m_dpi = GetDpiForWindow(g_app->m_hwnd);

		const int monitorWidth = workingArea.right - workingArea.left;
		const int monitorHeight = workingArea.bottom - workingArea.top;

		const int wndWidth = (int)((monitorWidth * g_app->m_dpi) / USER_DEFAULT_SCREEN_DPI);
		const int wndHeight = (int)((monitorHeight * g_app->m_dpi) / USER_DEFAULT_SCREEN_DPI);

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
	}

	void ResizeIfQueued() noexcept
	{
		if (g_app->m_issueResize)
		{
			if (g_app->m_upscaleFactor == 1.5f)
				g_app->m_upscaleFactor = 1.0f;
			else
				g_app->m_upscaleFactor = 1.5f;

			const float renderWidth = g_app->m_displayWidth / g_app->m_upscaleFactor;
			const float renderHeight = g_app->m_displayHeight / g_app->m_upscaleFactor;

			g_app->m_renderer.OnWindowSizeChanged(g_app->m_hwnd, (int)renderWidth, (int)renderHeight, g_app->m_displayWidth, g_app->m_displayHeight);
			g_app->m_scene.OnWindowSizeChanged();

			g_app->m_issueResize = false;
		}
	}

	ZetaInline int GetThreadIdx()
	{
		static_assert(ZetaArrayLen(g_app->m_threadIDs) / 8 == 2, "following assumes there are at most 16 threads.");

		const THREAD_ID_TYPE id = std::bit_cast<THREAD_ID_TYPE, std::thread::id>(std::this_thread::get_id());

		__m256i vKey = _mm256_set1_epi32(id);
		__m256i vIDs1 = _mm256_load_si256(reinterpret_cast<__m256i*>(g_app->m_threadIDs));
		__m256i vIDs2 = _mm256_load_si256(reinterpret_cast<__m256i*>(g_app->m_threadIDs + 8));

		uint32_t mask1 = (uint32_t) _mm256_movemask_epi8(_mm256_cmpeq_epi32(vKey, vIDs1));
		uint32_t mask2 = (uint32_t) _mm256_movemask_epi8(_mm256_cmpeq_epi32(vKey, vIDs2));
		uint64_t mask = (uint64_t(mask2) << 32) | mask1;

		uint64_t idx = _tzcnt_u64(mask);
		auto ret = idx == 64 ? -1 : int(idx >> 2);
		Assert(ret != -1, "thread index was not found.");

		return ret;
	}

	template<size_t blockSize>
	ZetaInline void* AllocateFrameAllocator(FrameMemory<blockSize>& frameMemory, FrameMemoryContext& context, size_t size, size_t alignment) noexcept
	{
		alignment = Math::Max(alignof(std::max_align_t), alignment);

		// at most alignment - 1 extra bytes are required
		Assert(size + alignment - 1 <= frameMemory.BLOCK_SIZE, "allocations larger than FrameMemory::BLOCK_SIZE are not possible with FrameAllocator.");

		const int threadIdx = GetThreadIdx();
		Assert(threadIdx != -1, "thread idx was not found");

		// current memory block has enough space
		int allocIdx = context.m_threadFrameAllocIndices[threadIdx];

		// first time in this frame
		if (allocIdx != -1)
		{
			auto& block = frameMemory.GetAndInitIfEmpty(allocIdx);

			const uintptr_t start = reinterpret_cast<uintptr_t>(block.Start);
			const uintptr_t ret = Math::AlignUp(start + block.Offset, alignment);
			const uintptr_t startOffset = ret - start;

			if (startOffset + size < frameMemory.BLOCK_SIZE)
			{
				block.Offset = startOffset + size;
				return reinterpret_cast<void*>(ret);
			}
		}

		// allocate/reuse a new block
		allocIdx = context.m_currFrameAllocIndex.fetch_add(1, std::memory_order_relaxed);
		context.m_threadFrameAllocIndices[threadIdx] = allocIdx;
		auto& block = frameMemory.GetAndInitIfEmpty(allocIdx);
		Assert(block.Offset == 0, "block offset should be initially 0");

		const uintptr_t start = reinterpret_cast<uintptr_t>(block.Start);
		const uintptr_t ret = Math::AlignUp(start, alignment);
		const uintptr_t startOffset = ret - start;

		Assert(startOffset + size < frameMemory.BLOCK_SIZE, "should never happen.");
		block.Offset = startOffset + size;

		return reinterpret_cast<void*>(ret);
	}
}

namespace ZetaRay
{
	ShaderReloadHandler::ShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg) noexcept
		: Dlg(dlg)
	{
		int n = std::min(MAX_LEN - 1, (int)strlen(name));
		Assert(n >= 1, "Invalid arg");
		memcpy(Name, name, n);
		Name[n] = '\0';

		ID = XXH3_64bits(Name, n);
	}

	LogMessage::LogMessage(const char* msg, LogMessage::MsgType t) noexcept
	{
		const int n = Math::Min((int)strlen(msg), LogMessage::MAX_LEN - 1);
		Assert(n > 0, "invalid log message.");

		const char* logType = t == MsgType::INFO ? "INFO" : "WARNING";
		Type = t;

		stbsp_snprintf(Msg, LogMessage::MAX_LEN - 1, "[Frame %04d] [tid %05d] [%s] | %s",
			g_app->m_timer.GetTotalFrameCount(), GetCurrentThreadId(), logType, msg);
	}

	void App::Init(Scene::Renderer::Interface& rendererInterface, const char* name) noexcept
	{
		// check intrinsics support
		const auto supported = Common::CheckIntrinsicSupport();
		Check(supported & CPU_Intrinsic::AVX2, "AVX2 is not supported.");
		Check(supported & CPU_Intrinsic::F16C, "F16C is not supported.");
		Check(supported & CPU_Intrinsic::BMI1, "BMI1 is not supported.");

		setlocale(LC_ALL, "C");		// set locale to C

		// create PSO cache directories
		Filesystem::CreateDirectoryIfNotExists(AppData::PSO_CACHE_PARENT);
		Filesystem::CreateDirectoryIfNotExists(AppData::PSO_CACHE_DIR);

		HINSTANCE instance = GetModuleHandleA(nullptr);
		CheckWin32(instance);

		g_app = new (std::nothrow) AppData;

		AppImpl::GetProcessorInfo();

		// create the window
		AppImpl::CreateAppWindow(instance);
		SetWindowTextA(g_app->m_hwnd, name ? name : "ZetaRay");

		// initialize thread pools
		const int totalNumThreads = g_app->m_processorCoreCount + AppData::NUM_BACKGROUND_THREADS;
		g_app->m_workerThreadPool.Init(g_app->m_processorCoreCount - 1,
			totalNumThreads,
			L"ZetaWorker",
			THREAD_PRIORITY::NORMAL);

		g_app->m_backgroundThreadPool.Init(AppData::NUM_BACKGROUND_THREADS,
			totalNumThreads,
			L"ZetaBackgroundWorker",
			THREAD_PRIORITY::BACKGROUND);

		// initialize frame allocators
		memset(g_app->m_smallFrameMemoryContext.m_threadFrameAllocIndices, -1, sizeof(int) * MAX_NUM_THREADS);
		g_app->m_smallFrameMemoryContext.m_currFrameAllocIndex.store(0, std::memory_order_release);
		memset(g_app->m_largeFrameMemoryContext.m_threadFrameAllocIndices, -1, sizeof(int) * MAX_NUM_THREADS);
		g_app->m_largeFrameMemoryContext.m_currFrameAllocIndex.store(0, std::memory_order_release);

		memset(g_app->m_threadIDs, 0, ZetaArrayLen(g_app->m_threadIDs) * sizeof(uint32_t));

		// main thread
		g_app->m_threadIDs[0] = std::bit_cast<THREAD_ID_TYPE, std::thread::id>(std::this_thread::get_id());

		// worker threads
		auto workerThreadIDs = g_app->m_workerThreadPool.ThreadIDs();

		for (int i = 0; i < workerThreadIDs.size(); i++)
			g_app->m_threadIDs[i + 1] = std::bit_cast<THREAD_ID_TYPE, std::thread::id>(workerThreadIDs[i]);

		// background threads
		auto backgroundThreadIDs = g_app->m_backgroundThreadPool.ThreadIDs();

		for (int i = 0; i < backgroundThreadIDs.size(); i++)
			g_app->m_threadIDs[workerThreadIDs.size() + 1 + i] = std::bit_cast<THREAD_ID_TYPE, std::thread::id>(backgroundThreadIDs[i]);

		g_app->m_workerThreadPool.Start();
		g_app->m_backgroundThreadPool.Start();

		RECT rect;
		GetClientRect(g_app->m_hwnd, &rect);

		g_app->m_displayWidth = rect.right - rect.left;
		g_app->m_displayHeight = rect.bottom - rect.top;

		// ImGui
		AppImpl::InitImGui();

		// initialize renderer
		const float renderWidth = g_app->m_displayWidth / g_app->m_upscaleFactor;
		const float renderHeight = g_app->m_displayHeight / g_app->m_upscaleFactor;
		g_app->m_renderer.Init(g_app->m_hwnd, (int)renderWidth, (int)renderHeight, g_app->m_displayWidth, g_app->m_displayHeight);

		// initialize camera
		g_app->m_frameMotion.Reset();

		g_app->m_camera.Init(float3(-0.245, 1.322, -4.043), App::GetRenderer().GetAspectRatio(),
			Math::DegreeToRadians(75.0f), 0.2f, true, float3(0, 0, 1), false);

		// scene can now be initialized
		g_app->m_scene.Init(rendererInterface);

		ParamVariant acc;
		acc.InitFloat("Scene", "Camera", "Acceleration", fastdelegate::FastDelegate1<const ParamVariant&>(&AppImpl::SetCameraAcceleration),
			g_app->m_cameraAcceleration,
			1.0f,
			300.0f,
			1.0f);
		App::AddParam(acc);

		g_app->m_isInitialized = true;

		LOG_UI(INFO, "Detected %d physical cores.", g_app->m_processorCoreCount);
		LOG_UI(INFO, "Work area on the primary display monitor is %dx%d.", g_app->m_displayWidth, g_app->m_displayHeight);
	}

	int App::Run() noexcept
	{
		MSG msg = {};
		bool success = false;

		while (true)
		{
			if (g_app->m_isActive && success)
				g_app->m_renderer.WaitForSwapChainWaitableObject();

			// process messages
			while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				if (msg.message == WM_QUIT)
					return (int)msg.wParam;

				TranslateMessage(&msg);
				DispatchMessageA(&msg);
			}

			if (!g_app->m_isActive)
			{
				Sleep(16);
				continue;
			}

			// help out while there are (non-background) unfinished tasks from previous frame
			success = g_app->m_workerThreadPool.TryFlush();

			// don't block the message-handling thread
			if (!success)
				continue;

			// game loop
			g_app->m_renderer.BeginFrame();
			g_app->m_timer.Tick();
			AppImpl::ResizeIfQueued();

			// at this point, all worker tasks from previous frame are done (GPU may still be executing those though)
			g_app->m_currTaskSignalIdx.store(0, std::memory_order_relaxed);

			if (g_app->m_timer.GetTotalFrameCount() > 1)
			{
				g_app->m_smallFrameMemoryContext.m_currFrameAllocIndex.store(0, std::memory_order_release);
				memset(g_app->m_smallFrameMemoryContext.m_threadFrameAllocIndices, -1, sizeof(int) * MAX_NUM_THREADS);
				g_app->m_smallFrameMemory.Reset();		// set the offset to 0, essentially freeing the memory

				g_app->m_largeFrameMemoryContext.m_currFrameAllocIndex.store(0, std::memory_order_release);
				memset(g_app->m_largeFrameMemoryContext.m_threadFrameAllocIndices, -1, sizeof(int) * MAX_NUM_THREADS);
				g_app->m_largeFrameMemory.Reset();		// set the offset to 0, essentially freeing the memory
			}

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
				// each taskset is spawning more tasks (which can potentially run in parallel)
				sceneTS.ConnectTo(sceneRendererTS);

				sceneTS.Finalize();
				sceneRendererTS.Finalize();

				Submit(ZetaMove(sceneTS));
				Submit(ZetaMove(sceneRendererTS));
			}

			// help out as long as updates are not finished before moving to rendering
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

				// end frame
				{
					g_app->m_renderer.EndFrame(endFrameTS);

					endFrameTS.EmplaceTask("Scene::Recycle", []()
						{
							g_app->m_scene.Recycle();
						});

					endFrameTS.Sort();
				}

				renderTS.ConnectTo(endFrameTS);

				renderTS.Finalize();
				endFrameTS.Finalize();

				Submit(ZetaMove(renderTS));
				Submit(ZetaMove(endFrameTS));
			}

			g_app->m_workerThreadPool.PumpUntilEmpty();
		}

		return (int)msg.wParam;
	}

	void App::Abort() noexcept
	{
		AppImpl::OnDestroy();
		PostQuitMessage(0);
	}

	void* App::AllocateSmallFrameAllocator(size_t size, size_t alignment) noexcept
	{
		return AppImpl::AllocateFrameAllocator<>(g_app->m_smallFrameMemory, g_app->m_smallFrameMemoryContext, size, alignment);
	}

	void* App::AllocateLargeFrameAllocator(size_t size, size_t alignment) noexcept
	{
		return AppImpl::AllocateFrameAllocator<>(g_app->m_largeFrameMemory, g_app->m_largeFrameMemoryContext, size, alignment);
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

	void App::SignalAdjacentTailNodes(Span<int> taskIDs) noexcept
	{
		for (auto handle : taskIDs)
		{
			auto& taskSignal = g_app->m_registeredTasks[handle];
			const int remaining = taskSignal.Indegree.fetch_sub(1, std::memory_order_acquire);

			// this was the last dependency, unblock the task
			if (remaining == 1)
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

	RendererCore& App::GetRenderer() noexcept { return g_app->m_renderer; }
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

		if (e && oldScaleFactor == 1.0)
			g_app->m_issueResize = true;
		else if (!e && oldScaleFactor == 1.5f)
			g_app->m_issueResize = true;
	}

	void App::LockStdOut() noexcept
	{
		if (g_app)
			AcquireSRWLockExclusive(&g_app->m_stdOutLock);
	}

	void App::UnlockStdOut() noexcept
	{
		if (g_app)
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

	RWSynchronizedVariable<Span<ParamVariant>> App::GetParams() noexcept
	{
		return RWSynchronizedVariable<Span<ParamVariant>>(g_app->m_params, g_app->m_paramLock);
	}

	RSynchronizedVariable<Span<ShaderReloadHandler>> App::GetShaderReloadHandlers() noexcept
	{
		return RSynchronizedVariable<Span<ShaderReloadHandler>>(g_app->m_shaderReloadHandlers, g_app->m_shaderReloadLock);
	}

	RWSynchronizedVariable<Span<Stat>> App::GetStats() noexcept
	{
		return RWSynchronizedVariable<Span<Stat>>(g_app->m_frameStats, g_app->m_statsLock);
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
		uint64_t id = XXH3_64bits(name, Math::Min(ShaderReloadHandler::MAX_LEN - 1, (int)strlen(name)));

		AcquireSRWLockExclusive(&g_app->m_shaderReloadLock);
		int i = 0;
		bool found = false;

		for (i = 0; i < (int)g_app->m_shaderReloadHandlers.size(); i++)
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

	void App::Log(const char* msg, LogMessage::MsgType t) noexcept
	{
		AcquireSRWLockExclusive(&g_app->m_logLock);
		g_app->m_frameLogs.emplace_back(msg, t);
		ReleaseSRWLockExclusive(&g_app->m_logLock);
	}

	Util::RSynchronizedVariable<Util::Span<App::LogMessage>> App::GetFrameLogs() noexcept
	{
		return RSynchronizedVariable<Span<LogMessage>>(g_app->m_frameLogs, g_app->m_logLock);;
	}
}
