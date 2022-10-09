include("${CMAKE_INCLUDE_DIR}/CompileHLSL.cmake")

set(RENDERPASS_DIR "${RENDERER_DIR}/RenderPass")
set(RENDERPASS_SRC
    "${RENDERPASS_DIR}/RenderPass.cpp"
    "${RENDERPASS_DIR}/RenderPass.h")

add_subdirectory(Common)
add_subdirectory(Clear)
add_subdirectory(Compositing)
add_subdirectory(Denoiser)
add_subdirectory(Final)
add_subdirectory(FSR2)
add_subdirectory(GBuffer)
add_subdirectory(GUI)
add_subdirectory(IndirectDiffuse)
add_subdirectory(LuminanceReduction)
add_subdirectory(Sky)
add_subdirectory(Sun)
add_subdirectory(TAA)

set(RENDERPASS_SRC ${RENDERPASS_SRC} 
    ${RP_CLEAR_SRC} 
    ${RP_COMMON_SRC} 
    ${RP_COMPOSITING_SRC} 
    ${RP_FINAL_SRC} 
    ${RP_FSR2_SRC} 
    ${RP_GBUFFER_SRC} 
    ${RP_GUI_SRC} 
    ${RP_IND_DIFF_SRC} 
    ${RP_LUM_REDUCTION_SRC} 
    ${RP_SKY_SRC} 
    ${RP_DENOISER_SRC} 
    ${RP_SUN_SRC} 
    ${RP_TAA_SRC} 
    PARENT_SCOPE)
        
file(GLOB_RECURSE ALL_SHADERS "${RENDERPASS_DIR}/*.hlsl")

foreach(HLSL ${ALL_SHADERS})
    CompileHLSL(${HLSL} "${RP_COMMON_SRC}" CSOS)
    set(ALL_CSOS ${ALL_CSOS} ${CSOS})
endforeach()

add_custom_target(CompileShaders ALL DEPENDS ${ALL_CSOS})
