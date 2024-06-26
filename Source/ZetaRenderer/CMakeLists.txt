add_subdirectory(Default)

# build ZetaRenderer as a static library
add_library(ZetaRenderer STATIC ${DEFAULT_RENDERER_SRC})
target_link_libraries(ZetaRenderer PUBLIC ZetaCore PRIVATE ZetaRenderPass)
target_include_directories(ZetaRenderer AFTER PUBLIC "${ZETA_CORE_DIR}" 
    PRIVATE "${EXTERNAL_DIR}" "${ZETA_RENDER_PASS_DIR}")
set_target_properties(ZetaRenderer PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

source_group(TREE "${ZETA_RENDERER_DIR}" FILES ${DEFAULT_RENDERER_SRC})