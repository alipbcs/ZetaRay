include("${CMAKE_INCLUDE_DIR}/Copy.cmake")
include("${CMAKE_INCLUDE_DIR}/SetupAgilitySDK.cmake")
include("${CMAKE_INCLUDE_DIR}/SetupWinPIX.cmake")
include("${CMAKE_INCLUDE_DIR}/SetupxxHash.cmake")
include("${CMAKE_INCLUDE_DIR}/Setupcgltf.cmake")

add_subdirectory(App)
add_subdirectory(Core)
add_subdirectory(Math)
add_subdirectory(Model)
add_subdirectory(RayTracing)
add_subdirectory(Scene)
add_subdirectory(Support)
add_subdirectory(Utility)
add_subdirectory(Win32)

set(CORE_SRC
	${APP_SRC}
	${CORE_SRC}
	${MATH_SRC}
	${MODEL_SRC}
	${RT_SRC}
	${SCENE_SRC}
	${SUPPORT_SRC}
	${UTIL_SRC}
	${WIN32_SRC})

# ImGui
set(IMGUI_DIR "${EXTERNAL_DIR}/imgui")
set(IMGUI_SRC "${IMGUI_DIR}/imgui.cpp"
	"${IMGUI_DIR}/imconfig.h"
	"${IMGUI_DIR}/imgui_draw.cpp"
	"${IMGUI_DIR}/imgui_tables.cpp"
	"${IMGUI_DIR}/imgui_widgets.cpp"
	"${IMGUI_DIR}/imnodes.cpp"
	"${IMGUI_DIR}/implot.cpp"
	"${IMGUI_DIR}/implot_items.cpp")

# natvis
set(NATVIS_SRC "${CMAKE_SOURCE_DIR}/Natvis/Container.natvis"
	"${CMAKE_SOURCE_DIR}/Natvis/imgui.natvis"
	"${CMAKE_SOURCE_DIR}/Natvis/App.natvis")

source_group(TREE "${ZETA_CORE_DIR}" FILES ${CORE_SRC})
source_group(TREE "${CMAKE_SOURCE_DIR}" FILES ${NATVIS_SRC})
source_group(TREE "${EXTERNAL_DIR}" PREFIX "External" FILES ${IMGUI_SRC})

# build ZetaCore as a static library
add_library(ZetaCore STATIC ${CORE_SRC} ${IMGUI_SRC} ${NATVIS_SRC})
target_include_directories(ZetaCore AFTER PUBLIC ${EXTERNAL_DIR})
set_target_properties(ZetaCore PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# 
# WinPixEventRuntime
# 
SetupWinPIX()
add_library(WinPixEventRuntimeLib SHARED IMPORTED GLOBAL)
set_target_properties(WinPixEventRuntimeLib PROPERTIES IMPORTED_IMPLIB "${EXTERNAL_DIR}/WinPixEventRuntime/WinPixEventRuntime.lib")

# 
# xxHash
# 
SetupxxHash()

# 
# cgltf
# 
Setupcgltf()

# custom command that copies the dll to the runtime directory
set(WINPIXEVENT_DLL_PATH "${EXTERNAL_DIR}/WinPixEventRuntime/WinPixEventRuntime.dll")
Copy("${WINPIXEVENT_DLL_PATH}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}" CopyWinPixDLL)
add_dependencies(WinPixEventRuntimeLib CopyWinPixDLL)

# 
# DirectX Agility SDK
# 
SetupAgilitySDK(AGILITY_SDK_VER)
add_library(DX12AgilitySDK INTERFACE)
set(DX12AgilitySDK_BIN
	"${EXTERNAL_DIR}/D3D12/${AGILITY_SDK_VER}/D3D12Core.dll"
    "${EXTERNAL_DIR}/D3D12/${AGILITY_SDK_VER}/d3d12SDKLayers.dll")

Copy("${DX12AgilitySDK_BIN}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/D3D12/" CopyDX12AgilitySDKBins)
add_dependencies(DX12AgilitySDK CopyDX12AgilitySDKBins)

# Font
add_library(FONT INTERFACE)
set(FONT_LIB "${ASSET_DIR}/Font/Font.dll")

# custom command that copies the dlls into the runtime directory
Copy("${FONT_LIB}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" CopyFontDLL)
add_dependencies(FONT CopyFontDLL)

# 
# link against all the external libraries
# 
set(LIBS d3d12 dxgi dxguid DX12AgilitySDK WinPixEventRuntimeLib FONT)
target_link_libraries(ZetaCore debug ${LIBS} dbghelp)
target_link_libraries(ZetaCore optimized ${LIBS})
