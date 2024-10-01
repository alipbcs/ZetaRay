cmake_minimum_required(VERSION 3.21)

# Enable building custom commands by Visual Studio in parallel. Introduced in CMake 3.27.
if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.27")
    cmake_policy(SET CMP0147 NEW)
endif()

set(CMAKE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/CMake")
include("${CMAKE_INCLUDE_DIR}/SetBuildConfigurations.cmake")
include("${CMAKE_INCLUDE_DIR}/SetupDXC.cmake")

project(ZetaRay
    LANGUAGES CXX
    DESCRIPTION "A Physically-Based Real-Time Renderer in Direct3D 12")

option(BUILD_TESTS "Build unit tests" OFF)
option(BUILD_TOOLS "Build tools" ON)
option(COMPILE_SHADERS_WITH_DEBUG "Additionally compile shaders with debug information included" OFF)

# set output directories
set(CMAKE_SUPPRESS_REGENERATION true)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
# append 'd' to debug targets
set(CMAKE_DEBUG_POSTFIX "d")

# enable folders for code organization
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# if(MSVC)
#     # generate pdb files for the release build (off by default)
#     add_compile_options("$<$<CONFIG:RELEASE>:/Zi>")
#     add_link_options("$<$<CONFIG:RELEASE>:/DEBUG>")
# endif()

# 
# compiler options
# 
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

if(MSVC)
    if(MSVC_TOOLSET_VERSION VERSION_LESS 142)
        message(FATAL_ERROR "MSVC toolset version 142 or greater is required.")
    endif()

    # disable exceptions
    string(REPLACE "/EHsc" "" NEW_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    set(CMAKE_CXX_FLAGS ${NEW_CXX_FLAGS} CACHE STRING "" FORCE) 
    # disable runtime checks
    string(REPLACE "/RTC1" "" NEW_CXX_DBG_FLAGS ${CMAKE_CXX_FLAGS_DEBUG})
    set(CMAKE_CXX_FLAGS_DEBUG ${NEW_CXX_DBG_FLAGS} CACHE STRING "" FORCE)
    # multi-processor compilation
    add_compile_options(/MP)
    # warning level
    add_compile_options(/W4)
    # enable intrinsic functions
    add_compile_options(/Oi)
    add_compile_options("$<$<CONFIG:DEBUG>:/Ob1>")
    # disable RTTI
    add_compile_options(/GR-)
    # fast floating point
    add_compile_options(/fp:fast)
    # standards conformance
    add_compile_options(/permissive-)
    add_compile_options(/arch:AVX2)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wno-reorder-ctor)
    add_compile_options(-Wno-unused-variable)
    add_compile_options(-Wno-unused-but-set-variable)
    add_compile_options(-Wno-sign-compare)
    add_compile_options(-Wno-missing-braces)
    add_compile_options(-Wno-unused-local-typedef)
    add_compile_options(-Wno-unused-function)
    
    # Echo the currently compiled filename
    if (${CMAKE_CXX_SIMULATE_ID} STREQUAL MSVC)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /showFilenames")
    endif()
endif()

# 
# common paths
# 
set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/External")
set(TOOLS_DIR "${CMAKE_SOURCE_DIR}/Tools")
set(ZETA_CORE_DIR "${CMAKE_SOURCE_DIR}/Source/ZetaCore")
set(ZETA_RENDER_PASS_DIR "${CMAKE_SOURCE_DIR}/Source/ZetaRenderPass")
string(LENGTH "${ZETA_RENDER_PASS_DIR}" ZETA_RENDER_PASS_DIR_LEN)
set(ZETA_RENDERER_DIR "${CMAKE_SOURCE_DIR}/Source/ZetaRenderer")
set(ASSET_DIR "${CMAKE_SOURCE_DIR}/Assets")
set(CSO_DIR_DEBUG "${ASSET_DIR}/CSO/Debug")
set(CSO_DIR_RELEASE "${ASSET_DIR}/CSO/Release")

# 
# setup DXC
# 
SetupDXC(DXC_BIN_DIR)

add_subdirectory(Source)
add_subdirectory(Samples)

if(BUILD_TESTS)
    add_subdirectory(Tests)
endif()

if(BUILD_TOOLS)
    add_subdirectory(Tools)
endif()