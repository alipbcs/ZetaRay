include("${CMAKE_INCLUDE_DIR}/SetupDoctest.cmake")

SetupDoctest()

set(TEST_DIR ${CMAKE_SOURCE_DIR}/Tests)
set(TEST_SRC 
    "${TEST_DIR}/TestContainer.cpp"
    "${TEST_DIR}/TestMath.cpp"
    "${TEST_DIR}/TestAliasTable.cpp"
    "${TEST_DIR}/TestOffsetAllocator.cpp"
    "${TEST_DIR}/TestOptional.cpp"
    "${TEST_DIR}/main.cpp")

add_executable(Tests ${TEST_SRC})
target_link_libraries(Tests ZetaCore)
target_include_directories(Tests BEFORE PRIVATE ${ZETA_CORE_DIR})
# doctest requires exception handling
target_compile_options(Tests PRIVATE /EHsc)
set_target_properties(Tests PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
set_target_properties(Tests PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
