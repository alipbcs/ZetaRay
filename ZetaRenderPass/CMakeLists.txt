include("${CMAKE_INCLUDE_DIR}/CompileHLSL.cmake")

add_subdirectory(AutoExposure)
add_subdirectory(Common)
add_subdirectory(Clear)
add_subdirectory(Compositing)
add_subdirectory(Denoiser)
add_subdirectory(Display)
add_subdirectory(FSR2)
add_subdirectory(GBuffer)
add_subdirectory(GUI)
add_subdirectory(IndirectDiffuse)
add_subdirectory(Sky)
add_subdirectory(SunShadow)
add_subdirectory(TAA)

set(RENDERPASS_SRC 
	"${ZETA_RENDER_PASS_DIR}/RenderPass.cpp"
    "${ZETA_RENDER_PASS_DIR}/RenderPass.h"
	${RENDERPASS_SRC} 
    ${RP_AUTO_EXPOSURE_SRC} 
    ${RP_CLEAR_SRC} 
    ${RP_COMMON_SRC} 
    ${RP_COMPOSITING_SRC} 
    ${RP_DISPLAY_SRC} 
    ${RP_FSR2_SRC} 
    ${RP_GBUFFER_SRC} 
    ${RP_GUI_SRC} 
    ${RP_IND_DIFF_SRC} 
    ${RP_SKY_SRC} 
    ${RP_DENOISER_SRC} 
    ${RP_SUN_SRC} 
    ${RP_TAA_SRC})
        
file(GLOB_RECURSE ALL_SHADERS "${ZETA_RENDER_PASS_DIR}/*.hlsl")

foreach(HLSL ${ALL_SHADERS})
    CompileHLSL(${HLSL} "${RP_COMMON_SRC}" CSOS)
    set(ALL_CSOS ${ALL_CSOS} ${CSOS})
endforeach()

add_custom_target(CompileShaders ALL DEPENDS ${ALL_CSOS})

# override MSBuild, which tries to call fxc
if(MSVC)
	set_source_files_properties(${ALL_SHADERS} PROPERTIES VS_TOOL_OVERRIDE "None")
endif()

# build ZetaRenderPass as a static library
add_library(ZetaRenderPass STATIC ${RENDERPASS_SRC})
add_dependencies(ZetaRenderPass ZetaCore CompileShaders)
target_include_directories(ZetaRenderPass AFTER PUBLIC ${EXTERNAL_DIR} ${ZETA_CORE_DIR})
set_target_properties(ZetaRenderPass PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

source_group(TREE "${ZETA_RENDER_PASS_DIR}" FILES ${RENDERPASS_SRC})

# 
# FSR2
# 
add_library(FSR2 INTERFACE)
set(FSR2_SDK_BIN
	"${EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_x64.dll"
    "${EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_dx12_x64.dll")

# custom command that copies the dlls into the runtime directory
Copy("${FSR2_SDK_BIN}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" CopyFsr2SDKBins)
add_dependencies(FSR2 CopyFsr2SDKBins)

# add_library(FSR2_API STATIC IMPORTED)
# set_target_properties(FSR2_API PROPERTIES IMPORTED_LOCATION_DEBUG "${EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_x64d.lib")
# set_target_properties(FSR2_API PROPERTIES IMPORTED_LOCATION_RELEASE "${EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_x64.lib")
# add_library(FSR2_DX12 STATIC IMPORTED)
# set_target_properties(FSR2_DX12 PROPERTIES IMPORTED_LOCATION_DEBUG "${EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_dx12_x64d.lib")
# set_target_properties(FSR2_DX12 PROPERTIES IMPORTED_LOCATION_RELEASE "${EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_dx12_x64.lib")

# custom command that copies static libs into runtime directory
# set(FSR2_LIB_PATHS
# 	"${EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_x64d.lib"
# 	"${EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_dx12_x64d.lib"
# 	"${EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_x64.lib"
# 	"${EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_dx12_x64.lib")
# Copy("${FSR2_LIB_PATHS}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}" CopyFSR2Libs)
# add_dependencies(FSR2_API CopyFSR2Libs)
# add_dependencies(FSR2_DX12 CopyFSR2Libs)

# 
# link against all the external libraries
# 
# set(LIBS d3d12 dxgi dxguid DX12AgilitySDK WinPixEventRuntimeLib FSR2_API FSR2_DX12)
set(LIBS FSR2)
target_link_libraries(ZetaRenderPass debug ${LIBS})
target_link_libraries(ZetaRenderPass optimized ${LIBS})
