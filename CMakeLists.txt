cmake_minimum_required(VERSION 3.21)

set(CMAKE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/CMake")
include("${CMAKE_INCLUDE_DIR}/SetUpConfigurations.cmake")
include("${CMAKE_INCLUDE_DIR}/SetUpDXC.cmake")

project(ZetaRay
    LANGUAGES CXX
    DESCRIPTION "A Physically-Based Hybrid Raster-Raytraced Renderer in DirectX 12")

if(MSVC_TOOLSET_VERSION VERSION_LESS 142)
    message(FATAL_ERROR "MSVC toolset version 142 or greater was not found. Please make sure Visual Studio 2019 or newer installed")
endif()

# set output directories
set(CMAKE_SUPPRESS_REGENERATION true)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_DEBUG_POSTFIX "d")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# 
# compiler options
# 
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

if(MSVC)
    # disable exceptions
    string(REPLACE "/EHsc" "" NEW_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    set(CMAKE_CXX_FLAGS ${NEW_CXX_FLAGS} CACHE STRING "" FORCE) 
    # disable runtime checks
    string(REPLACE "/RTC1" "" NEW_CXX_DBG_FLAGS ${CMAKE_CXX_FLAGS_DEBUG})
    set(CMAKE_CXX_FLAGS_DEBUG ${NEW_CXX_DBG_FLAGS} CACHE STRING "" FORCE)
    # multi-processor compilation
    add_compile_options(/MP)
    # warning level
    add_compile_options(/W3)
    # enable intrinsic functions
    add_compile_options(/Oi)
    add_compile_options(/Ob1)
    # disable RTTI
    add_compile_options(/GR-)
    # fast floating point
    add_compile_options(/fp:fast)
endif()

# 
# common paths
# 
set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/External")
set(TOOLS_DIR "${CMAKE_SOURCE_DIR}/Tools")
set(RENDERER_DIR "${CMAKE_SOURCE_DIR}/ZetaRay")
set(ASSET_DIR "${CMAKE_SOURCE_DIR}/Assets")
set(CSO_DIR_DEBUG "${ASSET_DIR}/CSO/Debug")
set(CSO_DIR_RELEASE "${ASSET_DIR}/CSO/Release")

# 
# setup DXC
# 
SetupDXC(DXC_BIN_DIR)

add_subdirectory(ZetaRay)
add_subdirectory(Samples)
add_subdirectory(Tests)