include("${CMAKE_INCLUDE_DIR}/CompileHLSL.cmake")

add_subdirectory(AutoExposure)
add_subdirectory(Common)
add_subdirectory(Compositing)
add_subdirectory(Display)
add_subdirectory(DirectLighting)
add_subdirectory(FSR2)
add_subdirectory(GBuffer)
add_subdirectory(GUI)
add_subdirectory(IndirectLighting)
add_subdirectory(PreLighting)
add_subdirectory(Sky)
add_subdirectory(SunShadow)
add_subdirectory(TAA)

set(RENDERPASS_SRC 
    "${ZETA_RENDER_PASS_DIR}/RenderPass.h"
    ${RP_AUTO_EXPOSURE_SRC} 
    ${RP_COMMON_SRC} 
    ${RP_COMPOSITING_SRC} 
    ${RP_DI_SRC} 
    ${RP_DISPLAY_SRC} 
    ${RP_FSR2_SRC} 
    ${RP_GBUFFER_RT_SRC} 
    ${RP_GUI_SRC} 
    ${RP_IND_LIGHTING_SRC} 
    ${RP_PRE_LIGHTING_SRC} 
    ${RP_SKY_SRC} 
    ${RP_SUN_SRC} 
    ${RP_TAA_SRC})
        
file(GLOB_RECURSE ALL_SHADERS "${ZETA_RENDER_PASS_DIR}/*.hlsl")

foreach(HLSL ${ALL_SHADERS})
    CompileHLSL(${HLSL} CSOS)
    set(ALL_CSOS ${ALL_CSOS} ${CSOS})
endforeach()

add_custom_target(CompileShaders ALL DEPENDS ${ALL_CSOS})

# override MSBuild, which tries to call fxc
if(MSVC)
    set_source_files_properties(${ALL_SHADERS} PROPERTIES VS_TOOL_OVERRIDE "None")
endif()

# build ZetaRenderPass as a static library
add_library(ZetaRenderPass STATIC ${RENDERPASS_SRC})
target_include_directories(ZetaRenderPass PUBLIC "${ZETA_CORE_DIR}" 
    PRIVATE "${EXTERNAL_DIR}" "${ZETA_RENDER_PASS_DIR}" "${EXTERNAL_DIR}/ImGui" AFTER)
set_target_properties(ZetaRenderPass PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

source_group(TREE "${ZETA_RENDER_PASS_DIR}" FILES ${RENDERPASS_SRC})

# 
# FSR2
# 
add_library(FSR2 INTERFACE)
set(FSR2_SDK_BIN
    "${EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_x64.dll"
    "${EXTERNAL_DIR}/FSR2/Lib/ffx_fsr2_api_dx12_x64.dll")

# custom command that copies dlls into runtime directory
Copy("${FSR2_SDK_BIN}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" CopyFsr2SDKBins)
add_dependencies(FSR2 CopyFsr2SDKBins)

# 
# link against all the external libraries
# 
set(PUBLIC_LIBS ZetaCore)
set(PRIVATE_LIBS FSR2)
target_link_libraries(ZetaRenderPass PUBLIC ${PUBLIC_LIBS} PRIVATE ${PRIVATE_LIBS})
