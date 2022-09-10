include("${CMAKE_INCLUDE_DIR}/CompileHLSL.cmake")

set(ZETA_RENDERPASS_DIR "${ZETA_RENDERER_DIR}/RenderPass")
set(ZETA_RENDERPASS_SRC
    "${ZETA_RENDERPASS_DIR}/RenderPass.cpp"
    "${ZETA_RENDERPASS_DIR}/RenderPass.h")

add_subdirectory(Common)
add_subdirectory(Clear)
add_subdirectory(Compositing)
add_subdirectory(Final)
add_subdirectory(FSR2)
add_subdirectory(GBuffer)
add_subdirectory(GUI)
add_subdirectory(IndirectDiffuse)
add_subdirectory(LuminanceReduction)
add_subdirectory(Sky)
add_subdirectory(STAD)
add_subdirectory(Sun)
add_subdirectory(TAA)

set(ZETA_RENDERPASS_SRC ${ZETA_RENDERPASS_SRC} 
    ${ZETA_RP_CLEAR_SRC} 
    ${ZETA_RP_COMMON_SRC} 
    ${ZETA_RP_COMPOSITING_SRC} 
    ${ZETA_RP_FINAL_SRC} 
    ${ZETA_RP_FSR2_SRC} 
    ${ZETA_RP_GBUFFER_SRC} 
    ${ZETA_RP_GUI_SRC} 
    ${ZETA_RP_IND_DIFF_SRC} 
    ${ZETA_RP_LUM_REDUCTION_SRC} 
    ${ZETA_RP_SKY_SRC} 
    ${ZETA_RP_STAD_SRC} 
    ${ZETA_RP_SUN_SRC} 
    ${ZETA_RP_TAA_SRC} 
    PARENT_SCOPE)
        
file(GLOB_RECURSE ALL_SHADERS "${ZETA_RENDERPASS_DIR}/*.hlsl")

foreach(HLSL ${ALL_SHADERS})
    CompileHLSL(${HLSL} "${ZETA_RP_COMMON_SRC}" CSOS)
    set(ALL_CSOS ${ALL_CSOS} ${CSOS})
endforeach()

add_custom_target(CompileShaders ALL DEPENDS ${ALL_CSOS})
