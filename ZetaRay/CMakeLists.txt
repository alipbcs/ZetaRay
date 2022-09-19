include("${CMAKE_INCLUDE_DIR}/Copy.cmake")

add_subdirectory(Core)
add_subdirectory(Math)
add_subdirectory(Model)
add_subdirectory(RayTracing)
add_subdirectory(RenderPass)
add_subdirectory(Scene)
add_subdirectory(Support)
add_subdirectory(Utility)
add_subdirectory(Win32)

set(ZETA_RENDERER_SRC
	${ZETA_CORE_SRC}
	${ZETA_MATH_SRC}
	${ZETA_MODEL_SRC}
	${ZETA_RENDERPASS_SRC}
	${ZETA_RT_SRC}
	${ZETA_SCENE_SRC}
	${ZETA_SCENERENDERER_SRC}
	${ZETA_SUPPORT_SRC}
	${ZETA_UTIL_SRC}
	${ZETA_WIN32_SRC})

# ImGui
set(ZETA_IMGUI_DIR "${ZETA_EXTERNAL_DIR}/imgui")
set(ZETA_IMGUI_SRC "${ZETA_IMGUI_DIR}/imgui.cpp"
	"${ZETA_IMGUI_DIR}/imgui_draw.cpp"
	"${ZETA_IMGUI_DIR}/imgui_tables.cpp"
	"${ZETA_IMGUI_DIR}/imgui_widgets.cpp"
	"${ZETA_IMGUI_DIR}/imnodes.cpp"
	"${ZETA_IMGUI_DIR}/implot.cpp"
	"${ZETA_IMGUI_DIR}/implot_items.cpp")

# natvis
set(ZETA_NATVIS_SRC "${CMAKE_SOURCE_DIR}/natvis/Container.natvis"
	"${CMAKE_SOURCE_DIR}/natvis/imgui.natvis"
	"${CMAKE_SOURCE_DIR}/natvis/Win32.natvis")

if(MSVC)
	source_group(TREE "${ZETA_RENDERER_DIR}" FILES ${ZETA_RENDERER_SRC})
	source_group(TREE "${CMAKE_SOURCE_DIR}" FILES ${ZETA_NATVIS_SRC})
	source_group(TREE "${ZETA_EXTERNAL_DIR}" PREFIX "External" FILES ${ZETA_IMGUI_SRC})
endif()

# build renderer as a static library
add_library(ZetaRay STATIC ${ZETA_RENDERER_SRC} ${ZETA_IMGUI_SRC} ${ZETA_NATVIS_SRC})
add_dependencies(ZetaRay CompileShaders)
target_include_directories(ZetaRay AFTER PUBLIC "${ZETA_EXTERNAL_DIR}")
set_target_properties(ZetaRay PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# override MSBuild, which tries to call fxc
if(MSVC)
	file(GLOB_RECURSE ALL_SHADERS "${ZETA_RENDERER_DIR}/RenderPass/*.hlsl")
	set_source_files_properties(${ALL_SHADERS} PROPERTIES VS_TOOL_OVERRIDE "None")
endif()

# 
# WinPixEventRuntime
# 
add_library(WinPixEventRuntimeLib SHARED IMPORTED GLOBAL)
set_target_properties(WinPixEventRuntimeLib PROPERTIES IMPORTED_IMPLIB "${ZETA_EXTERNAL_DIR}/WinPixEventRuntime/WinPixEventRuntime.lib")

# custom command that copies dll to runtime directory
set(WINPIXEVENT_DLL_PATH "${ZETA_EXTERNAL_DIR}/WinPixEventRuntime/WinPixEventRuntime.dll")
Copy("${WINPIXEVENT_DLL_PATH}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}" CopyWinPixDLL)
add_dependencies(WinPixEventRuntimeLib CopyWinPixDLL)

# 
# DirectX Agility SDK
# 
add_library(DX12AgilitySDK INTERFACE)

set(DX12AgilitySDK_BIN
	"${ZETA_EXTERNAL_DIR}/D3D12/1.606.3/D3D12Core.dll"
    "${ZETA_EXTERNAL_DIR}/D3D12/1.606.3/d3d12SDKLayers.dll")

Copy("${DX12AgilitySDK_BIN}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/D3D12/" CopyDX12AgilitySDKBins)
add_dependencies(DX12AgilitySDK CopyDX12AgilitySDKBins)

# 
# FSR2
# 
add_library(FSR2_API STATIC IMPORTED)
set_target_properties(FSR2_API PROPERTIES IMPORTED_LOCATION_DEBUG "${ZETA_EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_x64d.lib")
set_target_properties(FSR2_API PROPERTIES IMPORTED_LOCATION_RELEASE "${ZETA_EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_x64.lib")
add_library(FSR2_DX12 STATIC IMPORTED)
set_target_properties(FSR2_DX12 PROPERTIES IMPORTED_LOCATION_DEBUG "${ZETA_EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_dx12_x64d.lib")
set_target_properties(FSR2_DX12 PROPERTIES IMPORTED_LOCATION_RELEASE "${ZETA_EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_dx12_x64.lib")

# custom command that copies static libs into runtime directory
set(FSR2_LIB_PATHS
	"${ZETA_EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_x64d.lib"
	"${ZETA_EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_dx12_x64d.lib"
	"${ZETA_EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_x64.lib"
	"${ZETA_EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_dx12_x64.lib")
Copy("${FSR2_LIB_PATHS}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}" CopyFSR2Libs)
add_dependencies(FSR2_API CopyFSR2Libs)
add_dependencies(FSR2_DX12 CopyFSR2Libs)

# 
# link against all external libraries
# 
set(ZETA_LIBS d3d12 dxgi dxguid DX12AgilitySDK WinPixEventRuntimeLib FSR2_API FSR2_DX12)
target_link_libraries(ZetaRay debug ${ZETA_LIBS})
target_link_libraries(ZetaRay optimized ${ZETA_LIBS})
