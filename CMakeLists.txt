cmake_minimum_required(VERSION 3.21)

set(CMAKE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/CMake")
include("${CMAKE_INCLUDE_DIR}/SetUpConfigurations.cmake")
include("${CMAKE_INCLUDE_DIR}/SetUpDXC.cmake")

project(ZetaRay
    LANGUAGES CXX
    DESCRIPTION "A Physically-Based Hybrid Raster-Raytraced Renderer in DirectX 12")

option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_TOOLS "Build tools" ON)

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

if(MSVC)
    # generate pdb files for the release build (off by default)
    add_compile_options("$<$<CONFIG:RELEASE>:/Zi>")
    add_link_options("$<$<CONFIG:RELEASE>:/DEBUG>")
endif()

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
    add_compile_options(/Ob1)
    # disable RTTI
    add_compile_options(/GR-)
    # fast floating point
    add_compile_options(/fp:fast)
    # standards conformance
    add_compile_options(/permissive-)
endif()

# 
# common paths
# 
set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/External")
set(TOOLS_DIR "${CMAKE_SOURCE_DIR}/Tools")
set(ZETA_CORE_DIR "${CMAKE_SOURCE_DIR}/ZetaCore")
set(ZETA_RENDER_PASS_DIR "${CMAKE_SOURCE_DIR}/ZetaRenderPass")
set(ZETA_RENDERER_DIR "${CMAKE_SOURCE_DIR}/ZetaRenderer")
set(ASSET_DIR "${CMAKE_SOURCE_DIR}/Assets")
set(CSO_DIR_DEBUG "${ASSET_DIR}/CSO/Debug")
set(CSO_DIR_RELEASE "${ASSET_DIR}/CSO/Release")

# 
# setup DXC
# 
SetupDXC(DXC_BIN_DIR)

add_subdirectory(ZetaCore)
add_subdirectory(ZetaRenderPass)
add_subdirectory(ZetaRenderer)
add_subdirectory(Samples)

if(BUILD_TESTS)
    add_subdirectory(Tests)
endif()

if(BUILD_TOOLS)
    add_subdirectory(Tools)
endif()