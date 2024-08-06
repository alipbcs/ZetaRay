#include "../App/Log.h"
#include "../Support/FrameMemory.h"
#include "../Utility/SynchronizedView.h"
#include "../App/Timer.h"
#include "../App/Common.h"
#include "../Support/Param.h"
#include "../Support/Stat.h"
#include "../Core/RendererCore.h"
#include "../Scene/SceneCore.h"
#include "../Scene/Camera.h"
#include "../Support/ThreadPool.h"
#include "../Assets/Font/Font.h"
#include "../Assets/Font/IconsFontAwesome6.h"

#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION 
#include <xxHash/xxhash.h>

#include <ImGui/imgui.h>
#include <ImGui/implot.h>
#include <ImGui/imnodes.h>

#include <Uxtheme.h>    // for HTHEME

using namespace ZetaRay;
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
        alignas(64) int m_threadFrameAllocIndices[ZETA_MAX_NUM_THREADS] = { -1 };
        std::atomic_int32_t m_currFrameAllocIndex;
    };

    struct AppData
    {
        inline static constexpr const char* PSO_CACHE_PARENT = "..\\Assets\\PsoCache";
#if defined(_DEBUG) && defined(HAS_DEBUG_SHADERS)
        inline static constexpr const char* COMPILED_SHADER_DIR = "..\\Assets\\CSO\\Debug";
#else
        inline static constexpr const char* COMPILED_SHADER_DIR = "..\\Assets\\CSO\\Release";
#endif

#if defined(_DEBUG)
        inline static constexpr const char* PSO_CACHE_DIR = "..\\Assets\\PsoCache\\Debug";
#else
        inline static constexpr const char* PSO_CACHE_DIR = "..\\Assets\\PsoCache\\Release";
#endif

        inline static constexpr const char* ASSET_DIR = "..\\Assets";
        inline static constexpr const char* TOOLS_DIR = "..\\Tools";
        inline static constexpr const char* DXC_PATH = "..\\Tools\\dxc\\bin\\x64\\dxc.exe";
        inline static constexpr const char* RENDER_PASS_DIR = "..\\Source\\ZetaRenderPass";
        static constexpr int NUM_BACKGROUND_THREADS = 2;
        static constexpr int MAX_NUM_TASKS_PER_FRAME = 256;
        static constexpr int CLIPBOARD_LEN = 128;
        static constexpr int FRAME_ALLOCATOR_BLOCK_SIZE = FRAME_ALLOCATOR_MAX_ALLOCATION_SIZE;

        struct alignas(64) TaskSignal
        {
            std::atomic_int32_t Indegree;
            std::atomic_bool BlockFlag;
        };

        ThreadPool m_workerThreadPool;
        ThreadPool m_backgroundThreadPool;
        RendererCore m_renderer;
        Timer m_timer;
        SceneCore m_scene;
        Camera m_camera;

        FrameMemory<FRAME_ALLOCATOR_BLOCK_SIZE> m_frameMemory;
        FrameMemoryContext m_frameMemoryContext;

        uint16_t m_processorCoreCount = 0;
        HWND m_hwnd;
        RECT m_wndRectCache;
        uint16_t m_displayWidth;
        uint16_t m_displayHeight;
        bool m_isActive = true;
        bool m_manuallyPaused = false;
        int16 m_lastMousePosX = 0;
        int16 m_lastMousePosY = 0;
        int16 m_lastLMBClickPosX = 0;
        int16 m_lastLMBClickPosY = 0;
        bool m_picked = false;
        int m_inMouseWheelMove = 0;
        bool m_inSizeMove = false;
        bool m_minimized = false;
        bool m_isFullScreen = false;
        ImGuiMouseCursor m_imguiCursor = ImGuiMouseCursor_COUNT;
        HWND m_mouseHwnd;
        int m_imguiMouseTrackedArea = 0;   // 0: not tracked, 1: client are, 2: non-client area
        int m_imguiMouseButtonsDown = 0;
        bool m_imguiMouseTracked = false;
        uint16_t m_dpi;
        Core::GpuMemory::Texture m_imguiFontTex;
        Core::DescriptorTable m_fontTexSRV;

        float m_upscaleFactor = 1.0f;
        float m_queuedUpscaleFactor = 1.0f;
        float m_cameraAcceleration = 40.0f;

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

        alignas(64) ZETA_THREAD_ID_TYPE m_threadIDs[ZETA_MAX_NUM_THREADS];
        TaskSignal m_registeredTasks[MAX_NUM_TASKS_PER_FRAME];
        std::atomic_int32_t m_currTaskSignalIdx = 0;
        Motion m_frameMotion;
        SmallVector<LogMessage, FrameAllocator> m_frameLogs;
        char m_clipboard[CLIPBOARD_LEN];
        bool m_isInitialized = false;
        bool m_issueResize = false;
    };

    AppData* g_app = nullptr;
}

// Copied from imgui_impl_win32
namespace
{
    // See https://learn.microsoft.com/en-us/windows/win32/tablet/system-events-and-mouse-messages
    // Prefer to call this at the top of the message handler to avoid the possibility of 
    // other Win32 calls interfering with this.
    ImGuiMouseSource GetMouseSourceFromMessageExtraInfo()
    {
        LPARAM extra_info = GetMessageExtraInfo();
        if ((extra_info & 0xFFFFFF80) == 0xFF515700)
            return ImGuiMouseSource_Pen;
        if ((extra_info & 0xFFFFFF80) == 0xFF515780)
            return ImGuiMouseSource_TouchScreen;
        return ImGuiMouseSource_Mouse;
    }

    bool ImGui_UpdateMouseCursor()
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
            return false;

        ImGuiMouseCursor imgui_cursor = ImGui::GetMouseCursor();
        if (imgui_cursor == ImGuiMouseCursor_None || io.MouseDrawCursor)
        {
            // Hide OS mouse cursor if imgui is drawing it or if it wants no cursor
            SetCursor(nullptr);
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

            SetCursor(LoadCursor(nullptr, win32_cursor));
        }

        return true;
    }

    void ImGui_UpdateMouse()
    {
        ImGuiIO& io = ImGui::GetIO();

        HWND focused_window = ::GetForegroundWindow();
        const bool is_app_focused = (focused_window == g_app->m_hwnd);
        if (is_app_focused)
        {
            // (Optional) Set OS mouse position from Dear ImGui if requested (rarely used, only when 
            // ImGuiConfigFlags_NavEnableSetMousePos is enabled by user)
            if (io.WantSetMousePos)
            {
                POINT pos = { (int)io.MousePos.x, (int)io.MousePos.y };
                if (ClientToScreen(g_app->m_hwnd, &pos))
                    SetCursorPos(pos.x, pos.y);
            }

            // (Optional) Fallback to provide mouse position when focused (WM_MOUSEMOVE already provides this when hovered or captured)
            // This also fills a short gap when clicking non-client area: WM_NCMOUSELEAVE -> modal OS move -> gap -> WM_NCMOUSEMOVE
            if (!io.WantSetMousePos && g_app->m_imguiMouseTrackedArea == 0)
            {
                POINT pos;
                if (GetCursorPos(&pos) && ::ScreenToClient(g_app->m_hwnd, &pos))
                    io.AddMousePosEvent((float)pos.x, (float)pos.y);
            }
        }

        // Update OS mouse cursor with the cursor requested by imgui
        ImGuiMouseCursor mouse_cursor = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
        if (g_app->m_imguiCursor != mouse_cursor)
        {
            g_app->m_imguiCursor = mouse_cursor;
            ImGui_UpdateMouseCursor();
        }
    }

    ImGuiKey ImGui_VirtualKeyToImGuiKey(WPARAM wParam)
    {
        switch (wParam)
        {
        case VK_TAB: return ImGuiKey_Tab;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_SPACE: return ImGuiKey_Space;
        case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_OEM_7: return ImGuiKey_Apostrophe;
        case VK_OEM_COMMA: return ImGuiKey_Comma;
        case VK_OEM_MINUS: return ImGuiKey_Minus;
        case VK_OEM_PERIOD: return ImGuiKey_Period;
        case VK_OEM_2: return ImGuiKey_Slash;
        case VK_OEM_1: return ImGuiKey_Semicolon;
        case VK_OEM_PLUS: return ImGuiKey_Equal;
        case VK_OEM_4: return ImGuiKey_LeftBracket;
        case VK_OEM_5: return ImGuiKey_Backslash;
        case VK_OEM_6: return ImGuiKey_RightBracket;
        case VK_OEM_3: return ImGuiKey_GraveAccent;
        case VK_CAPITAL: return ImGuiKey_CapsLock;
        case VK_SCROLL: return ImGuiKey_ScrollLock;
        case VK_NUMLOCK: return ImGuiKey_NumLock;
        case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
        case VK_PAUSE: return ImGuiKey_Pause;
        case VK_NUMPAD0: return ImGuiKey_Keypad0;
        case VK_NUMPAD1: return ImGuiKey_Keypad1;
        case VK_NUMPAD2: return ImGuiKey_Keypad2;
        case VK_NUMPAD3: return ImGuiKey_Keypad3;
        case VK_NUMPAD4: return ImGuiKey_Keypad4;
        case VK_NUMPAD5: return ImGuiKey_Keypad5;
        case VK_NUMPAD6: return ImGuiKey_Keypad6;
        case VK_NUMPAD7: return ImGuiKey_Keypad7;
        case VK_NUMPAD8: return ImGuiKey_Keypad8;
        case VK_NUMPAD9: return ImGuiKey_Keypad9;
        case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
        case VK_DIVIDE: return ImGuiKey_KeypadDivide;
        case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
        case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
        case VK_ADD: return ImGuiKey_KeypadAdd;
            //case IM_VK_KEYPAD_ENTER: return ImGuiKey_KeypadEnter;
        case VK_LSHIFT: return ImGuiKey_LeftShift;
        case VK_LCONTROL: return ImGuiKey_LeftCtrl;
        case VK_LMENU: return ImGuiKey_LeftAlt;
        case VK_LWIN: return ImGuiKey_LeftSuper;
        case VK_RSHIFT: return ImGuiKey_RightShift;
        case VK_RCONTROL: return ImGuiKey_RightCtrl;
        case VK_RMENU: return ImGuiKey_RightAlt;
        case VK_RWIN: return ImGuiKey_RightSuper;
        case VK_APPS: return ImGuiKey_Menu;
        case '0': return ImGuiKey_0;
        case '1': return ImGuiKey_1;
        case '2': return ImGuiKey_2;
        case '3': return ImGuiKey_3;
        case '4': return ImGuiKey_4;
        case '5': return ImGuiKey_5;
        case '6': return ImGuiKey_6;
        case '7': return ImGuiKey_7;
        case '8': return ImGuiKey_8;
        case '9': return ImGuiKey_9;
        case 'A': return ImGuiKey_A;
        case 'B': return ImGuiKey_B;
        case 'C': return ImGuiKey_C;
        case 'D': return ImGuiKey_D;
        case 'E': return ImGuiKey_E;
        case 'F': return ImGuiKey_F;
        case 'G': return ImGuiKey_G;
        case 'H': return ImGuiKey_H;
        case 'I': return ImGuiKey_I;
        case 'J': return ImGuiKey_J;
        case 'K': return ImGuiKey_K;
        case 'L': return ImGuiKey_L;
        case 'M': return ImGuiKey_M;
        case 'N': return ImGuiKey_N;
        case 'O': return ImGuiKey_O;
        case 'P': return ImGuiKey_P;
        case 'Q': return ImGuiKey_Q;
        case 'R': return ImGuiKey_R;
        case 'S': return ImGuiKey_S;
        case 'T': return ImGuiKey_T;
        case 'U': return ImGuiKey_U;
        case 'V': return ImGuiKey_V;
        case 'W': return ImGuiKey_W;
        case 'X': return ImGuiKey_X;
        case 'Y': return ImGuiKey_Y;
        case 'Z': return ImGuiKey_Z;
        case VK_F1: return ImGuiKey_F1;
        case VK_F2: return ImGuiKey_F2;
        case VK_F3: return ImGuiKey_F3;
        case VK_F4: return ImGuiKey_F4;
        case VK_F5: return ImGuiKey_F5;
        case VK_F6: return ImGuiKey_F6;
        case VK_F7: return ImGuiKey_F7;
        case VK_F8: return ImGuiKey_F8;
        case VK_F9: return ImGuiKey_F9;
        case VK_F10: return ImGuiKey_F10;
        case VK_F11: return ImGuiKey_F11;
        case VK_F12: return ImGuiKey_F12;
        case VK_F13: return ImGuiKey_F13;
        case VK_F14: return ImGuiKey_F14;
        case VK_F15: return ImGuiKey_F15;
        case VK_F16: return ImGuiKey_F16;
        case VK_F17: return ImGuiKey_F17;
        case VK_F18: return ImGuiKey_F18;
        case VK_F19: return ImGuiKey_F19;
        case VK_F20: return ImGuiKey_F20;
        case VK_F21: return ImGuiKey_F21;
        case VK_F22: return ImGuiKey_F22;
        case VK_F23: return ImGuiKey_F23;
        case VK_F24: return ImGuiKey_F24;
        case VK_BROWSER_BACK: return ImGuiKey_AppBack;
        case VK_BROWSER_FORWARD: return ImGuiKey_AppForward;
        default: return ImGuiKey_None;
        }
    }

    bool ImGui_IsVkDown(int vk)
    {
        return (GetKeyState(vk) & 0x8000) != 0;
    }

    void ImGui_AddKeyEvent(ImGuiKey key, bool down, int native_keycode, int native_scancode = -1)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(key, down);
        // To support legacy indexing (<1.87 user code)
        io.SetKeyEventNativeData(key, native_keycode, native_scancode);
        IM_UNUSED(native_scancode);
    }

    void ImGui_UpdateKeyModifiers()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, ImGui_IsVkDown(VK_CONTROL));
        io.AddKeyEvent(ImGuiMod_Shift, ImGui_IsVkDown(VK_SHIFT));
        io.AddKeyEvent(ImGuiMod_Alt, ImGui_IsVkDown(VK_MENU));
        io.AddKeyEvent(ImGuiMod_Super, ImGui_IsVkDown(VK_LWIN) || ImGui_IsVkDown(VK_RWIN));
    }
}

namespace ZetaRay::AppImpl
{
    void LoadFont()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();

        using getFontFP = FontSpan(*)(FONT_TYPE f);
        HINSTANCE fontLib = LoadLibraryExA("Font", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
        CheckWin32(fontLib);

        auto fpGetFont = reinterpret_cast<getFontFP>(GetProcAddress(fontLib, "GetFont"));
        CheckWin32(fpGetFont);

        constexpr auto fontType = FONT_TYPE::BFONT;
        FontSpan f = fpGetFont(fontType);
        Check(f.Data, "font was not found.");

        constexpr float fontSizePixels96 = 12.0f;
        float fontSizePixelsDPI = ((float)g_app->m_dpi / USER_DEFAULT_SCREEN_DPI) * fontSizePixels96;
        fontSizePixelsDPI = roundf(fontSizePixelsDPI);

        ImFontConfig font_cfg;
        font_cfg.FontDataOwnedByAtlas = false;
        io.Fonts->AddFontFromMemoryCompressedBase85TTF(reinterpret_cast<const char*>(f.Data), 
            fontSizePixelsDPI, &font_cfg);

        float baseFontSize = 16;
        baseFontSize *= ((float)g_app->m_dpi / USER_DEFAULT_SCREEN_DPI);
        // FontAwesome fonts need to have their sizes reduced by 2.0f/3.0f in order to align correctly
        float iconFontSize = baseFontSize * 2.0f / 3.0f;

        static const ImWchar icons_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
        ImFontConfig icons_config;
        icons_config.MergeMode = true;
        icons_config.PixelSnapH = true;
        icons_config.GlyphMinAdvanceX = iconFontSize;
        icons_config.FontDataOwnedByAtlas = false;

        auto iconFont = fpGetFont(FONT_TYPE::FONT_AWESOME_6);
        io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(iconFont.Data), (int)iconFont.N, 
            iconFontSize, &icons_config, icons_ranges);

        unsigned char* pixels;
        int width;
        int height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        g_app->m_imguiFontTex = GpuMemory::GetTexture2DAndInit("ImGuiFont", width, height,
            DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, pixels);

        g_app->m_fontTexSRV = App::GetRenderer().GetGpuDescriptorHeap().Allocate(1);
        Direct3DUtil::CreateTexture2DSRV(g_app->m_imguiFontTex, g_app->m_fontTexSRV.CPUHandle(0));

        const uint32_t gpuDescHeapIdx = g_app->m_fontTexSRV.GPUDesciptorHeapIndex(0);
        static_assert(sizeof(gpuDescHeapIdx) <= sizeof(io.UserData), "overflow");
        memcpy(&io.UserData, &gpuDescHeapIdx, sizeof(gpuDescHeapIdx));

        FreeLibrary(fontLib);
    }

    void OnActivated()
    {
        g_app->m_timer.Resume();
        g_app->m_isActive = true;
        SetWindowTextA(g_app->m_hwnd, "ZetaRay");
    }

    void OnDeactivated()
    {
        g_app->m_timer.Pause();
        g_app->m_isActive = false;
        SetWindowTextA(g_app->m_hwnd, "ZetaRay (Paused - press 'P' to resume)");
    }

    void OnDPIChanged(uint16_t newDPI, const RECT* newRect)
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

    void InitImGui()
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
        colors[ImGuiCol_CheckMark] = ImVec4(112 / 255.0f, 118 / 255.0f, 128 / 255.0f, 1.0f);
        colors[ImGuiCol_TableHeaderBg] = ImVec4(15 / 255.0f, 15 / 255.0f, 15 / 255.0f, 1.0f);
        colors[ImGuiCol_TableRowBg] = ImVec4(1 / 255.0f, 1 / 255.0f, 1 / 255.0f, 1.0f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(7 / 255.0f, 7 / 255.0f, 8 / 255.0f, 1.0f);
        colors[ImGuiCol_TableBorderLight] = ImVec4(15 / 255.0f, 15 / 255.0f, 15 / 255.0f, 1.0f);
        colors[ImGuiCol_TableBorderStrong] = ImVec4(27 / 255.0f, 27 / 255.0f, 27 / 255.0f, 1.0f);
        colors[ImGuiCol_Button] = ImVec4(31 / 255.0f, 31 / 255.0f, 31 / 255.0f, 1.0f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(95 / 255.0f, 95 / 255.0f, 95 / 255.0f, 1.0f);
        colors[ImGuiCol_ButtonActive] = ImVec4(46 / 255.0f, 103 / 255.0f, 130 / 255.0f, 1.0f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(23 / 255.0f, 23 / 255.0f, 23 / 255.0f, 1.0f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(73 / 255.0f, 73 / 255.0f, 73 / 255.0f, 1.0f);
        colors[ImGuiCol_SliderGrab] = ImVec4(41 / 255.0f, 41 / 255.0f, 41 / 255.0f, 1.0f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(150 / 255.0f, 150 / 255.0f, 150 / 255.0f, 1.0f);

        style.FramePadding = ImVec2(7.0f, 3.0f);
        style.GrabMinSize = 13.0f;
        style.FrameRounding = 2.5f;
        style.GrabRounding = 2.5f;
        style.ItemSpacing = ImVec2(8.0f, 7.0f);
        style.CellPadding.x = 10.0;

        style.ScaleAllSizes((float)g_app->m_dpi / USER_DEFAULT_SCREEN_DPI);

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)g_app->m_displayWidth, (float)g_app->m_displayHeight);
        io.IniFilename = nullptr;
        // We can honor GetMouseCursor() values (optional)
        io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
        // We can honor io.WantSetMousePos requests (optional, rarely used)
        io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;

        LoadFont();

        ImNodes::GetIO().AltMouseButton = ImGuiMouseButton_Right;
    }

    void UpdateStats(size_t tempMemoryUsage)
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
        g_app->m_frameStats.emplace_back("Frame", "Frame temp memory usage (kb)", tempMemoryUsage >> 10);
    }

    void Update(TaskSet& sceneTS, TaskSet& sceneRendererTS, size_t tempMemoryUsage)
    {
        UpdateStats(tempMemoryUsage);

        ImGui_UpdateMouse();
        ImGui::NewFrame();

        if (!(GetAsyncKeyState(VK_LSHIFT) & (1 << 16)))
        {
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
        }
        else
        {
            if(g_app->m_inMouseWheelMove)
                g_app->m_cameraAcceleration *= (1 + g_app->m_inMouseWheelMove * 0.1f);
        }

        g_app->m_inMouseWheelMove = 0;
        g_app->m_frameMotion.dt = (float)g_app->m_timer.GetElapsedTime();

        g_app->m_camera.Update(g_app->m_frameMotion);

        if (g_app->m_picked)
        {
            g_app->m_scene.Pick(g_app->m_lastLMBClickPosX, g_app->m_lastLMBClickPosY);
            g_app->m_picked = false;
        }

        g_app->m_scene.Update(g_app->m_timer.GetElapsedTime(), sceneTS, sceneRendererTS);
    }

    void OnWindowSizeChanged()
    {
        if (g_app->m_timer.GetTotalFrameCount() > 0)
        {
            RECT rect;
            GetClientRect(g_app->m_hwnd, &rect);

            const uint16_t newWidth = (uint16_t)(rect.right - rect.left);
            const uint16_t newHeight = (uint16_t)(rect.bottom - rect.top);

            if (newWidth == g_app->m_displayWidth && newHeight == g_app->m_displayHeight)
                return;

            g_app->m_displayWidth = newWidth;
            g_app->m_displayHeight = newHeight;

            const float renderWidth = g_app->m_displayWidth / g_app->m_upscaleFactor;
            const float renderHeight = g_app->m_displayHeight / g_app->m_upscaleFactor;

            // following order is important
            g_app->m_renderer.OnWindowSizeChanged(g_app->m_hwnd, (uint16_t)renderWidth, (uint16_t)renderHeight,
                g_app->m_displayWidth, g_app->m_displayHeight);
            g_app->m_scene.OnWindowSizeChanged();
            g_app->m_camera.OnWindowSizeChanged();

            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)g_app->m_displayWidth, (float)g_app->m_displayHeight);
        }
    }

    void OnToggleFullscreenWindow()
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

        g_app->m_isFullScreen = !g_app->m_isFullScreen;
    }

    void OnKeyboard(UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui::GetCurrentContext() == nullptr)
            return;

        ImGuiIO& io = ImGui::GetIO();

        const bool is_key_down = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
        if (wParam < 256)
        {
            // Submit modifiers
            ImGui_UpdateKeyModifiers();

            // Obtain virtual key code
            // (keypad enter doesn't have its own... VK_RETURN with KF_EXTENDED flag means 
            // keypad enter, see IM_VK_KEYPAD_ENTER definition for details, it is mapped to ImGuiKey_KeyPadEnter.)
            int vk = (int)wParam;
            //if ((wParam == VK_RETURN) && (HIWORD(lParam) & KF_EXTENDED))
            //    vk = IM_VK_KEYPAD_ENTER;
            const ImGuiKey key = ImGui_VirtualKeyToImGuiKey(vk);
            const int scancode = (int)LOBYTE(HIWORD(lParam));

            // Special behavior for VK_SNAPSHOT / ImGuiKey_PrintScreen as Windows doesn't emit the key down event.
            if (key == ImGuiKey_PrintScreen && !is_key_down)
                ImGui_AddKeyEvent(key, true, vk, scancode);

            // Submit key event
            if (key != ImGuiKey_None)
                ImGui_AddKeyEvent(key, is_key_down, vk, scancode);

            // Submit individual left/right modifier events
            if (vk == VK_SHIFT)
            {
                // Important: Shift keys tend to get stuck when pressed together, missing key-up 
                // events are corrected in ImGui_ImplWin32_ProcessKeyEventsWorkarounds()
                if (ImGui_IsVkDown(VK_LSHIFT) == is_key_down)
                    ImGui_AddKeyEvent(ImGuiKey_LeftShift, is_key_down, VK_LSHIFT, scancode);
                if (ImGui_IsVkDown(VK_RSHIFT) == is_key_down)
                    ImGui_AddKeyEvent(ImGuiKey_RightShift, is_key_down, VK_RSHIFT, scancode);
            }
            else if (vk == VK_CONTROL)
            {
                if (ImGui_IsVkDown(VK_LCONTROL) == is_key_down)
                    ImGui_AddKeyEvent(ImGuiKey_LeftCtrl, is_key_down, VK_LCONTROL, scancode);
                if (ImGui_IsVkDown(VK_RCONTROL) == is_key_down)
                    ImGui_AddKeyEvent(ImGuiKey_RightCtrl, is_key_down, VK_RCONTROL, scancode);
            }
            else if (vk == VK_MENU)
            {
                if (ImGui_IsVkDown(VK_LMENU) == is_key_down)
                    ImGui_AddKeyEvent(ImGuiKey_LeftAlt, is_key_down, VK_LMENU, scancode);
                if (ImGui_IsVkDown(VK_RMENU) == is_key_down)
                    ImGui_AddKeyEvent(ImGuiKey_RightAlt, is_key_down, VK_RMENU, scancode);
            }
        }

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

            if (GetAsyncKeyState(VK_ESCAPE) & (1 << 16))
                g_app->m_scene.ClearPick();
        }
    }

    void OnMouseDown(UINT message, WPARAM btnState, LPARAM lParam)
    {
        if (ImGui::GetCurrentContext() == nullptr)
            return;

        ImGuiIO& io = ImGui::GetIO();

        ImGuiMouseSource mouse_source = GetMouseSourceFromMessageExtraInfo();
        int button = 0;
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) { button = 0; }
        if (message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK) { button = 1; }
        if (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK) { button = 2; }
        if (g_app->m_imguiMouseButtonsDown == 0 && GetCapture() == nullptr)
            SetCapture(g_app->m_hwnd);

        g_app->m_imguiMouseButtonsDown |= 1 << button;
        io.AddMouseSourceEvent(mouse_source);
        io.AddMouseButtonEvent(button, true);

        if (!io.WantCaptureMouse)
        {
            int16 x = GET_X_LPARAM(lParam);
            int16 y = GET_Y_LPARAM(lParam);

            if (btnState == MK_LBUTTON)
            {
                SetCapture(g_app->m_hwnd);
                g_app->m_lastMousePosX = x;
                g_app->m_lastMousePosY = y;
                g_app->m_lastLMBClickPosX = x;
                g_app->m_lastLMBClickPosY = y;
            }
        }
    }

    void OnMouseUp(UINT message, WPARAM btnState, LPARAM lParam)
    {
        if (ImGui::GetCurrentContext() == nullptr)
            return;

        ImGuiIO& io = ImGui::GetIO();

        ImGuiMouseSource mouse_source = GetMouseSourceFromMessageExtraInfo();
        int button = 0;
        if (message == WM_LBUTTONUP) { button = 0; }
        if (message == WM_RBUTTONUP) { button = 1; }
        if (message == WM_MBUTTONUP) { button = 2; }

        g_app->m_imguiMouseButtonsDown &= ~(1 << button);
        if (g_app->m_imguiMouseButtonsDown == 0 && GetCapture() == g_app->m_hwnd)
            ReleaseCapture();

        io.AddMouseSourceEvent(mouse_source);
        io.AddMouseButtonEvent(button, false);

        if (!io.WantCaptureMouse)
        {
            if (message == WM_LBUTTONUP)
                ReleaseCapture();

            if (g_app->m_lastLMBClickPosX == GET_X_LPARAM(lParam) &&
                g_app->m_lastLMBClickPosY == GET_Y_LPARAM(lParam))
            {
                g_app->m_picked = true;
            }
        }
    }

    void OnMouseMove(UINT message, WPARAM btnState, LPARAM lParam, HWND hwnd)
    {
        if (ImGui::GetCurrentContext() == nullptr)
            return;

        ImGuiIO& io = ImGui::GetIO();

        // We need to call TrackMouseEvent in order to receive WM_MOUSELEAVE events
        ImGuiMouseSource mouse_source = GetMouseSourceFromMessageExtraInfo();
        const int area = (message == WM_MOUSEMOVE) ? 1 : 2;
        g_app->m_mouseHwnd = hwnd;
        if (g_app->m_imguiMouseTrackedArea != area)
        {
            TRACKMOUSEEVENT tme_cancel = { sizeof(tme_cancel), TME_CANCEL, hwnd, 0 };
            TRACKMOUSEEVENT tme_track = { sizeof(tme_track), (DWORD)((area == 2) ? 
                (TME_LEAVE | TME_NONCLIENT) : TME_LEAVE), hwnd, 0 };
            if (g_app->m_imguiMouseTrackedArea != 0)
                TrackMouseEvent(&tme_cancel);

            TrackMouseEvent(&tme_track);
            g_app->m_imguiMouseTrackedArea = area;
        }

        POINT mouse_pos = { (LONG)GET_X_LPARAM(lParam), (LONG)GET_Y_LPARAM(lParam) };
        // WM_NCMOUSEMOVE are provided in absolute coordinates.
        if (message == WM_NCMOUSEMOVE && ScreenToClient(hwnd, &mouse_pos) == FALSE)
            return;

        io.AddMouseSourceEvent(mouse_source);
        io.AddMousePosEvent((float)mouse_pos.x, (float)mouse_pos.y);

        if (message != WM_MOUSEMOVE)
            return;

        if (!io.WantCaptureMouse)
        {
            if (btnState == MK_LBUTTON)
            {
                int16 x = GET_X_LPARAM(lParam);
                int16 y = GET_Y_LPARAM(lParam);

                g_app->m_frameMotion.dMouse_x = int16_t(x - g_app->m_lastMousePosX);
                g_app->m_frameMotion.dMouse_y = int16_t(y - g_app->m_lastMousePosY);

                g_app->m_lastMousePosX = x;
                g_app->m_lastMousePosY = y;
            }
        }
    }

    void OnMouseWheel(UINT message, WPARAM btnState, LPARAM lParam)
    {
        if (ImGui::GetCurrentContext() == nullptr)
            return;

        ImGuiIO& io = ImGui::GetIO();
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

    void OnDestroy()
    {
        App::FlushAllThreadPools();
        g_app->m_renderer.FlushAllCommandQueues();

        g_app->m_imguiFontTex.Reset(false);

        // Shuts down render passes and releases scene GPU resources
        g_app->m_scene.Shutdown();

        ImGui::DestroyContext();
        ImPlot::DestroyContext();
        ImNodes::DestroyContext();

        // Shuts down GPU memory
        g_app->m_renderer.Shutdown();

        g_app->m_workerThreadPool.Shutdown();
        g_app->m_backgroundThreadPool.Shutdown();

        delete g_app;
        g_app = nullptr;
    }

    void ApplyParamUpdates()
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

                if (found)
                    g_app->m_params.erase(i);
            }
        }

        g_app->m_paramsUpdates.clear();

        ReleaseSRWLockExclusive(&g_app->m_paramLock);
        ReleaseSRWLockExclusive(&g_app->m_paramUpdateLock);
    }

    // Ref: https://github.com/ysc3839/win32-darkmode
    bool TryInitDarkMode(HMODULE* uxthemeLib)
    {
        bool darkModeEnabled = false;

        *uxthemeLib = LoadLibraryExA("uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (*uxthemeLib)
        {
            fnOpenNcThemeData openNcThemeData = reinterpret_cast<fnOpenNcThemeData>(
                GetProcAddress(*uxthemeLib, MAKEINTRESOURCEA(49)));
            fnRefreshImmersiveColorPolicyState refreshImmersiveColorPolicyState = 
                reinterpret_cast<fnRefreshImmersiveColorPolicyState>(
                GetProcAddress(*uxthemeLib, MAKEINTRESOURCEA(104)));
            fnShouldAppsUseDarkMode shouldAppsUseDarkMode = reinterpret_cast<fnShouldAppsUseDarkMode>(
                GetProcAddress(*uxthemeLib, MAKEINTRESOURCEA(132)));
            fnAllowDarkModeForWindow allowDarkModeForWindow = reinterpret_cast<fnAllowDarkModeForWindow>(
                GetProcAddress(*uxthemeLib, MAKEINTRESOURCEA(133)));

            auto ord135 = GetProcAddress(*uxthemeLib, MAKEINTRESOURCEA(135));
            fnSetPreferredAppMode setPreferredAppMode = reinterpret_cast<fnSetPreferredAppMode>(ord135);

            fnIsDarkModeAllowedForWindow isDarkModeAllowedForWindow = 
                reinterpret_cast<fnIsDarkModeAllowedForWindow>(GetProcAddress(*uxthemeLib, 
                    MAKEINTRESOURCEA(137)));

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
                HIGHCONTRASTW highContrast;
                highContrast.cbSize = sizeof(highContrast);
                highContrast.dwFlags = 0;
                highContrast.lpszDefaultScheme = nullptr;
                if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, FALSE))
                    isHighContrast = highContrast.dwFlags & HCF_HIGHCONTRASTON;

                darkModeEnabled = shouldAppsUseDarkMode() && !isHighContrast;
            }
        }

        return darkModeEnabled;
    }

    LRESULT WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            HMODULE uxthemeLib = nullptr;
            BOOL dark = TryInitDarkMode(&uxthemeLib);

            fnSetWindowCompositionAttribute setWindowCompositionAttribute = 
                reinterpret_cast<fnSetWindowCompositionAttribute>(
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
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            AppImpl::OnKeyboard(message, wParam, lParam);
            return 0;

        case WM_CHAR:
        {
            wchar_t wch = 0;
            MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, (char*)&wParam, 1, &wch, 1);
            ImGui::GetIO().AddInputCharacter(wch);
        }
        return 0;

        case WM_MOUSELEAVE:
        case WM_NCMOUSELEAVE:
        {
            const int area = (message == WM_MOUSELEAVE) ? 1 : 2;
            if (g_app->m_imguiMouseTrackedArea == area)
            {
                if (g_app->m_mouseHwnd == hWnd)
                    g_app->m_mouseHwnd = nullptr;

                g_app->m_imguiMouseTrackedArea = 0;
                ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            }
            return 0;
        }

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
        case WM_NCMOUSEMOVE:
            AppImpl::OnMouseMove(message, wParam, lParam, hWnd);
            return 0;

        case WM_MOUSEWHEEL:
            AppImpl::OnMouseWheel(message, wParam, lParam);
            return 0;

        case WM_DPICHANGED:
            OnDPIChanged(HIWORD(wParam), reinterpret_cast<RECT*>(lParam));
            return 0;

        case WM_SETCURSOR:
            // This is required to restore cursor when transitioning from e.g resize borders to client area.
            if (LOWORD(lParam) == HTCLIENT && ImGui_UpdateMouseCursor())
                return 1;
            return 0;

        case WM_DESTROY:
            AppImpl::OnDestroy();
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    void CreateAppWindow(HINSTANCE instance)
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
        g_app->m_dpi = (uint16_t)GetDpiForWindow(g_app->m_hwnd);

        const int monitorWidth = workingArea.right - workingArea.left;
        const int monitorHeight = workingArea.bottom - workingArea.top;

        const int wndWidth = (int)((monitorWidth * g_app->m_dpi) / USER_DEFAULT_SCREEN_DPI);
        const int wndHeight = (int)((monitorHeight * g_app->m_dpi) / USER_DEFAULT_SCREEN_DPI);

        SetWindowPos(g_app->m_hwnd, nullptr, 0, 0, wndWidth, wndHeight, 0);
        ShowWindow(g_app->m_hwnd, SW_SHOWNORMAL);
    }

    void GetProcessorInfo()
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION* buffer = nullptr;
        DWORD buffSize = 0;
        GetLogicalProcessorInformation(buffer, &buffSize);

        Assert(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "GetLogicalProcessorInformation() failed.");
        buffer = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(malloc(buffSize));

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
                break;

            default:
                break;
            }

            curr++;
        }

        free(buffer);

        g_app->m_processorCoreCount = Math::Min(g_app->m_processorCoreCount, (uint16_t)ZETA_MAX_NUM_THREADS);
    }

    void SetCameraAcceleration(const ParamVariant& p)
    {
        g_app->m_cameraAcceleration = p.GetFloat().m_value;
    }

    void ResizeIfQueued()
    {
        if (g_app->m_issueResize)
        {
            g_app->m_upscaleFactor = g_app->m_queuedUpscaleFactor;

            const float renderWidth = g_app->m_displayWidth / g_app->m_upscaleFactor;
            const float renderHeight = g_app->m_displayHeight / g_app->m_upscaleFactor;

            g_app->m_renderer.OnWindowSizeChanged(g_app->m_hwnd, (uint16_t)renderWidth, (uint16_t)renderHeight,
                g_app->m_displayWidth, g_app->m_displayHeight);
            g_app->m_scene.OnWindowSizeChanged();
            g_app->m_camera.OnWindowSizeChanged();

            g_app->m_issueResize = false;
        }
    }

    ZetaInline int GetThreadIdx()
    {
        const ZETA_THREAD_ID_TYPE id = std::bit_cast<ZETA_THREAD_ID_TYPE, std::thread::id>(std::this_thread::get_id());

        int ret = -1;
        __m256i vKey = _mm256_set1_epi32(id);

        for (int i = 0; i < ZETA_MAX_NUM_THREADS; i += 8)
        {
            __m256i vIDs = _mm256_load_si256((__m256i*)(g_app->m_threadIDs + i));
            __m256i vRes = _mm256_cmpeq_epi32(vIDs, vKey);
            int mask = _mm256_movemask_ps(_mm256_castsi256_ps(vRes));

            if (mask != 0)
            {
                ret = i + _tzcnt_u32(mask);
                break;
            }
        }

        Assert(ret != -1, "thread index was not found.");

        return ret;
    }

    template<size_t blockSize>
    ZetaInline void* AllocateFrameAllocator(FrameMemory<blockSize>& frameMemory, FrameMemoryContext& context,
        size_t size, size_t alignment)
    {
        alignment = Math::Max(alignof(std::max_align_t), alignment);

        // at most alignment - 1 extra bytes are required
        Assert(size + alignment - 1 <= frameMemory.BLOCK_SIZE,
            "allocations larger than FrameMemory::BLOCK_SIZE are not possible with FrameAllocator.");

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
    ShaderReloadHandler::ShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg)
        : Dlg(dlg)
    {
        int n = std::min(MAX_LEN - 1, (int)strlen(name));
        Assert(n >= 1, "Invalid arg");
        memcpy(Name, name, n);
        Name[n] = '\0';

        ID = XXH3_64bits(Name, n);
    }

    LogMessage::LogMessage(const char* msg, LogMessage::MsgType t)
    {
        const int n = Math::Min((int)strlen(msg), LogMessage::MAX_LEN - 1);
        Assert(n > 0, "Invalid log message.");

        const char* logType = t == MsgType::INFO ? "INFO" : "WARNING";
        Type = t;

        stbsp_snprintf(Msg, LogMessage::MAX_LEN - 1, "[Frame %04d] [tid %05d] [%s] | %s",
            g_app->m_timer.GetTotalFrameCount(), GetCurrentThreadId(), logType, msg);
    }

    void App::Init(Scene::Renderer::Interface& rendererInterface, const char* name)
    {
        // check intrinsics support
        const auto supported = Common::CheckIntrinsicSupport();
        Check(supported & CPU_Intrinsic::AVX2, "AVX2 is not supported.");
        Check(supported & CPU_Intrinsic::F16C, "F16C is not supported.");
        Check(supported & CPU_Intrinsic::BMI1, "BMI1 is not supported.");

        setlocale(LC_ALL, "C");        // set locale to C

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
        memset(g_app->m_frameMemoryContext.m_threadFrameAllocIndices, -1, sizeof(int) * ZETA_MAX_NUM_THREADS);
        g_app->m_frameMemoryContext.m_currFrameAllocIndex.store(0, std::memory_order_release);

        memset(g_app->m_threadIDs, 0, ZetaArrayLen(g_app->m_threadIDs) * sizeof(uint32_t));

        // main thread
        g_app->m_threadIDs[0] = std::bit_cast<ZETA_THREAD_ID_TYPE, std::thread::id>(std::this_thread::get_id());

        // worker threads
        auto workerThreadIDs = g_app->m_workerThreadPool.ThreadIDs();

        for (int i = 0; i < workerThreadIDs.size(); i++)
            g_app->m_threadIDs[i + 1] = std::bit_cast<ZETA_THREAD_ID_TYPE, std::thread::id>(workerThreadIDs[i]);

        // background threads
        auto backgroundThreadIDs = g_app->m_backgroundThreadPool.ThreadIDs();

        for (int i = 0; i < backgroundThreadIDs.size(); i++)
            g_app->m_threadIDs[workerThreadIDs.size() + 1 + i] = std::bit_cast<ZETA_THREAD_ID_TYPE, std::thread::id>(backgroundThreadIDs[i]);

        g_app->m_workerThreadPool.Start();
        g_app->m_backgroundThreadPool.Start();

        RECT rect;
        GetClientRect(g_app->m_hwnd, &rect);

        g_app->m_displayWidth = (uint16_t)(rect.right - rect.left);
        g_app->m_displayHeight = (uint16_t)(rect.bottom - rect.top);

        // initialize renderer
        const float renderWidth = g_app->m_displayWidth / g_app->m_upscaleFactor;
        const float renderHeight = g_app->m_displayHeight / g_app->m_upscaleFactor;
        g_app->m_renderer.Init(g_app->m_hwnd, (uint16_t)renderWidth, (uint16_t)renderHeight, g_app->m_displayWidth, g_app->m_displayHeight);

        // ImGui
        AppImpl::InitImGui();

        // initialize camera
        g_app->m_frameMotion.Reset();

        g_app->m_camera.Init(float3(-0.245, 1.322, -4.043), App::GetRenderer().GetAspectRatio(),
            Math::DegreesToRadians(75.0f), 0.2f, true, float3(0, 0, 1), false);

        // scene can now be initialized
        g_app->m_scene.Init(rendererInterface);

        ParamVariant acc;
        acc.InitFloat("Scene", "Camera", "Acceleration",
            fastdelegate::FastDelegate1<const ParamVariant&>(&AppImpl::SetCameraAcceleration),
            g_app->m_cameraAcceleration, 1.0f, 100.0f, 1.0f, "Motion");
        App::AddParam(acc);

        g_app->m_isInitialized = true;

        LOG_UI(INFO, "Detected %d physical cores.", g_app->m_processorCoreCount);
        LOG_UI(INFO, "Work area on the primary display monitor is %dx%d.", g_app->m_displayWidth, g_app->m_displayHeight);
    }

    int App::Run()
    {
        MSG msg = {};
        bool success = false;
        g_app->m_timer.Start();

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

            // at this point, all worker tasks from previous frame are done (GPU may still be executing those though)
            g_app->m_currTaskSignalIdx.store(0, std::memory_order_relaxed);
            const size_t tempMemoryUsed = g_app->m_frameMemory.TotalSize();

            // Skip first frame
            if (g_app->m_timer.GetTotalFrameCount() > 0)
            {
                g_app->m_frameMemoryContext.m_currFrameAllocIndex.store(0, std::memory_order_release);
                for (int i = 0; i < ZETA_MAX_NUM_THREADS; i++)
                    g_app->m_frameMemoryContext.m_threadFrameAllocIndices[i] = -1;
                g_app->m_frameMemory.Reset();        // set the offset to 0, essentially releasing the memory

                g_app->m_frameLogs.free_memory();
            }

            g_app->m_renderer.BeginFrame();
            // Startup is counted as "frame" 0, so program loop starts from frame 1
            g_app->m_timer.Tick();
            AppImpl::ResizeIfQueued();

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
                AppImpl::Update(sceneTS, sceneRendererTS, tempMemoryUsed);

                auto h0 = sceneRendererTS.EmplaceTask("ResourceUploadSubmission", []()
                    {
                        g_app->m_renderer.SubmitResourceCopies();
                    });

                // make sure resource submission runs after everything else
                sceneRendererTS.AddIncomingEdgeFromAll(h0);

                sceneTS.Sort();
                sceneRendererTS.Sort();

                // sceneRendererTS has to run after sceneTS. This may seem sequential but
                // each taskset is spawning more tasks (which can potentially run in parallel).
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

                g_app->m_renderer.EndFrame(endFrameTS);
                endFrameTS.Sort();

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

    void App::Abort()
    {
        AppImpl::OnDestroy();
        PostQuitMessage(0);
    }

    void* App::AllocateFrameAllocator(size_t size, size_t alignment)
    {
        return AppImpl::AllocateFrameAllocator<>(g_app->m_frameMemory,
            g_app->m_frameMemoryContext, size, alignment);
    }

    int App::RegisterTask()
    {
        int idx = g_app->m_currTaskSignalIdx.fetch_add(1, std::memory_order_relaxed);
        Assert(idx < AppData::MAX_NUM_TASKS_PER_FRAME, 
            "Number of task signals exceeded MAX_NUM_TASKS_PER_FRAME.");

        return idx;
    }

    void App::TaskFinalizedCallback(int handle, int indegree)
    {
        Assert(indegree > 0, "Redundant call.");
        const int c = g_app->m_currTaskSignalIdx.load(std::memory_order_relaxed);
        Assert(handle < c, "Received handle %d while #handles for current frame is %d.", c);

        g_app->m_registeredTasks[handle].Indegree.store(indegree, std::memory_order_release);
        g_app->m_registeredTasks[handle].BlockFlag.store(true, std::memory_order_release);
    }

    void App::WaitForAdjacentHeadNodes(int handle)
    {
        const int c = g_app->m_currTaskSignalIdx.load(std::memory_order_relaxed);
        Assert(handle >= 0 && handle < c, "Received handle %d while #handles for current frame is %d.", c);

        auto& taskSignal = g_app->m_registeredTasks[handle];
        const int indegree = taskSignal.Indegree.load(std::memory_order_acquire);
        Assert(indegree >= 0, "Invalid task indegree.");

        if (indegree != 0)
        {
            taskSignal.BlockFlag.wait(true, std::memory_order_acquire);
            return;
        }
    }

    void App::SignalAdjacentTailNodes(Span<int> taskIDs)
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

    void App::Submit(Task&& t)
    {
        Assert(t.GetPriority() == TASK_PRIORITY::NORMAL, 
            "Background task is not allowed to be executed on the main thread pool.");
        g_app->m_workerThreadPool.Enqueue(ZetaMove(t));
    }

    void App::Submit(TaskSet&& ts)
    {
        g_app->m_workerThreadPool.Enqueue(ZetaMove(ts));
    }

    void App::SubmitBackground(Task&& t)
    {
        Assert(t.GetPriority() == TASK_PRIORITY::BACKGRUND, 
            "Normal-priority task is not allowed to be executed on the background thread pool.");
        g_app->m_backgroundThreadPool.Enqueue(ZetaMove(t));
    }

    void App::FlushWorkerThreadPool()
    {
        bool success = false;
        while (!success)
            success = g_app->m_workerThreadPool.TryFlush();
    }

    void App::FlushAllThreadPools()
    {
        bool success = false;
        while (!success)
            success = g_app->m_workerThreadPool.TryFlush();

        success = false;
        while (!success)
            success = g_app->m_backgroundThreadPool.TryFlush();
    }

    RendererCore& App::GetRenderer() { return g_app->m_renderer; }
    SceneCore& App::GetScene() { return g_app->m_scene; }
    const Camera& App::GetCamera() { return g_app->m_camera; }
    int App::GetNumWorkerThreads() { return g_app->m_processorCoreCount; }
    int App::GetNumBackgroundThreads() { return AppData::NUM_BACKGROUND_THREADS; }
    uint32_t App::GetDPI() { return g_app->m_dpi; }
    float App::GetUpscalingFactor() { return g_app->m_upscaleFactor; }
    bool App::IsFullScreen() { return g_app->m_isFullScreen; }
    const App::Timer& App::GetTimer() { return g_app->m_timer; }
    const char* App::GetPSOCacheDir() { return AppData::PSO_CACHE_DIR; }
    const char* App::GetCompileShadersDir() { return AppData::COMPILED_SHADER_DIR; }
    const char* App::GetAssetDir() { return AppData::ASSET_DIR; }
    const char* App::GetDXCPath() { return AppData::DXC_PATH; }
    const char* App::GetToolsDir() { return AppData::TOOLS_DIR; }
    const char* App::GetRenderPassDir() { return AppData::RENDER_PASS_DIR; }

    void App::SetUpscaleFactor(float f)
    {
        Assert(f >= 1.0f, "Invalid upscale factor.");
        const float oldScaleFactor = g_app->m_upscaleFactor;

        if (f != oldScaleFactor)
        {
            g_app->m_issueResize = true;
            g_app->m_queuedUpscaleFactor = f;
        }
    }

    void App::LockStdOut()
    {
        if (g_app)
            AcquireSRWLockExclusive(&g_app->m_stdOutLock);
    }

    void App::UnlockStdOut()
    {
        if (g_app)
            ReleaseSRWLockExclusive(&g_app->m_stdOutLock);
    }

    Span<uint32_t> App::GetWorkerThreadIDs()
    {
        return Span(g_app->m_threadIDs, g_app->m_processorCoreCount);
    }

    Span<uint32_t> App::GetBackgroundThreadIDs()
    {
        return Span(g_app->m_threadIDs + g_app->m_processorCoreCount, AppData::NUM_BACKGROUND_THREADS);
    }

    Span<uint32_t> App::GetAllThreadIDs()
    {
        return Span(g_app->m_threadIDs, g_app->m_processorCoreCount + AppData::NUM_BACKGROUND_THREADS);
    }

    RWSynchronizedVariable<MutableSpan<ParamVariant>> App::GetParams()
    {
        return RWSynchronizedVariable<MutableSpan<ParamVariant>>(g_app->m_params, g_app->m_paramLock);
    }

    RSynchronizedVariable<MutableSpan<ShaderReloadHandler>> App::GetShaderReloadHandlers()
    {
        return RSynchronizedVariable<MutableSpan<ShaderReloadHandler>>(g_app->m_shaderReloadHandlers, g_app->m_shaderReloadLock);
    }

    RWSynchronizedVariable<Span<Stat>> App::GetStats()
    {
        return RWSynchronizedVariable<Span<Stat>>(g_app->m_frameStats, g_app->m_statsLock);
    }

    void App::AddParam(ParamVariant& p)
    {
        AcquireSRWLockExclusive(&g_app->m_paramUpdateLock);

        g_app->m_paramsUpdates.push_back(ParamUpdate{
            .P = p,
            .Op = ParamUpdate::ADD });

        ReleaseSRWLockExclusive(&g_app->m_paramUpdateLock);
    }

    void App::RemoveParam(const char* group, const char* subgroup, const char* name)
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

    void App::AddShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg)
    {
        AcquireSRWLockExclusive(&g_app->m_shaderReloadLock);
        g_app->m_shaderReloadHandlers.emplace_back(name, dlg);
        ReleaseSRWLockExclusive(&g_app->m_shaderReloadLock);
    }

    void App::RemoveShaderReloadHandler(const char* name)
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

    void App::AddFrameStat(const char* group, const char* name, int i)
    {
        AcquireSRWLockExclusive(&g_app->m_statsLock);
        g_app->m_frameStats.emplace_back(group, name, i);
        ReleaseSRWLockExclusive(&g_app->m_statsLock);
    }

    void App::AddFrameStat(const char* group, const char* name, uint32_t u)
    {
        AcquireSRWLockExclusive(&g_app->m_statsLock);
        g_app->m_frameStats.emplace_back(group, name, u);
        ReleaseSRWLockExclusive(&g_app->m_statsLock);
    }

    void App::AddFrameStat(const char* group, const char* name, float f)
    {
        AcquireSRWLockExclusive(&g_app->m_statsLock);
        g_app->m_frameStats.emplace_back(group, name, f);
        ReleaseSRWLockExclusive(&g_app->m_statsLock);
    }

    void App::AddFrameStat(const char* group, const char* name, uint64_t u)
    {
        AcquireSRWLockExclusive(&g_app->m_statsLock);
        g_app->m_frameStats.emplace_back(group, name, u);
        ReleaseSRWLockExclusive(&g_app->m_statsLock);
    }

    void App::AddFrameStat(const char* group, const char* name, uint32_t num, uint32_t total)
    {
        AcquireSRWLockExclusive(&g_app->m_statsLock);
        g_app->m_frameStats.emplace_back(group, name, num, total);
        ReleaseSRWLockExclusive(&g_app->m_statsLock);
    }

    Span<float> App::GetFrameTimeHistory()
    {
        auto& frameStats = g_app->m_frameTime;
        return frameStats.FrameTimeHist;
    }

    void App::Log(const char* msg, LogMessage::MsgType t)
    {
        AcquireSRWLockExclusive(&g_app->m_logLock);
        g_app->m_frameLogs.emplace_back(msg, t);
        ReleaseSRWLockExclusive(&g_app->m_logLock);
    }

    Util::RSynchronizedVariable<Util::Span<App::LogMessage>> App::GetFrameLogs()
    {
        return RSynchronizedVariable<Span<LogMessage>>(g_app->m_frameLogs, g_app->m_logLock);;
    }
    
    void App::CopyToClipboard(StrView data)
    {
        if (data.empty())
            return;

        const size_t n = Math::Min(data.size() - 1, (size_t)AppData::CLIPBOARD_LEN - 1);
        memcpy(g_app->m_clipboard, data.data(), data.size());

        Task t("Clipboard", TASK_PRIORITY::BACKGRUND, [str = g_app->m_clipboard, n, hwnd = g_app->m_hwnd]()
            {
                auto h = GlobalAlloc(GMEM_MOVEABLE, n + 1);
                void* dst = GlobalLock(h);
                CheckWin32(dst);

                memcpy(dst, str, n);
                reinterpret_cast<char*>(dst)[n] = '\0';

                if (GlobalUnlock(h) == 0)
                    Check(GetLastError() == NO_ERROR, "GlobalUnlock() failed.");

                CheckWin32(OpenClipboard(hwnd));
                CheckWin32(EmptyClipboard());
                // "If SetClipboardData succeeds, the system owns the object identified by 
                // the hMem parameter. The application may not write to or free the data once 
                // ownership has been transferred to the system".
                CheckWin32(SetClipboardData(CF_TEXT, h));

                CheckWin32(CloseClipboard());
            });

        App::SubmitBackground(ZetaMove(t));
    }
}
