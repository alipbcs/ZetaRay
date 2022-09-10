set(ZETA_TEST_DIR ${CMAKE_SOURCE_DIR}/Tests)
set(ZETA_TEST_SRC 
    "${ZETA_TEST_DIR}/TestContainer.cpp"
    "${ZETA_TEST_DIR}/TestMath.cpp"
    "${ZETA_TEST_DIR}/TestAliasTable.cpp"
    "${ZETA_TEST_DIR}/main.cpp")

add_executable(Tests ${ZETA_TEST_SRC})
target_link_libraries(Tests ZetaRay)
target_include_directories(Tests BEFORE PRIVATE "${ZETA_RENDERER_DIR}")
# doctest requires exception handling
target_compile_options(Tests PRIVATE /EHsc)
set_target_properties(Tests PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
set_target_properties(Tests PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
