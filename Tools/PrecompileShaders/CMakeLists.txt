set(SOURCES PrecompileShaders.cpp)

# PrecompileShaders executable
add_executable(PrecompileShaders ${SOURCES})
target_include_directories(PrecompileShaders BEFORE PRIVATE "${ZETA_CORE_DIR}" "${ZETA_RENDER_PASS_DIR}" "${EXTERNAL_DIR}")
target_link_libraries(PrecompileShaders ZetaCore ZetaRenderPass)
set_target_properties(PrecompileShaders PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
target_compile_options(PrecompileShaders PRIVATE /fp:precise)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "PrecompileShaders" FILES ${SOURCES})

set_target_properties(PrecompileShaders PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
set_target_properties(PrecompileShaders PROPERTIES FOLDER "Tools")