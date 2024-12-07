set(COMPILED_SHADER_DIR DirectXTex/Shaders/Compiled)

set(SOURCES
    DirectXTex/DirectXTex.h
    DirectXTex/DirectXTex.inl
    DirectXTex/BC.h
    DirectXTex/DDS.h
    DirectXTex/DirectXTexP.h
    DirectXTex/filters.h
    DirectXTex/scoped.h
    DirectXTex/BC.cpp
    DirectXTex/BC4BC5.cpp
    DirectXTex/BC6HBC7.cpp
    DirectXTex/DirectXTexCompress.cpp
    DirectXTex/DirectXTexConvert.cpp
    DirectXTex/DirectXTexDDS.cpp
    DirectXTex/DirectXTexImage.cpp
    DirectXTex/DirectXTexMipmaps.cpp
    DirectXTex/DirectXTexMisc.cpp
    DirectXTex/DirectXTexResize.cpp
    DirectXTex/DirectXTexUtil.cpp
    DirectXTex/DirectXTexFlipRotate.cpp
    DirectXTex/DirectXTexWIC.cpp
    DirectXTex/BCDirectCompute.h
    DirectXTex/BCDirectCompute.cpp
    DirectXTex/DirectXTexCompressGPU.cpp
    DirectXTex/DirectXTexD3D11.cpp
    ${COMPILED_SHADER_DIR}/BC6HEncode_EncodeBlockCS.inc
    ${COMPILED_SHADER_DIR}/BC7Encode_EncodeBlockCS.inc   
    Texconv/texconv.cpp
    Texconv/texconv.h
    BCnCompressglTF.cpp)

# BCnCompressglTF executable
add_executable(BCnCompressglTF ${SOURCES})
target_include_directories(BCnCompressglTF BEFORE PRIVATE "${ZETA_CORE_DIR}" "${COMPILED_SHADER_DIR}" "${EXTERNAL_DIR}")
target_link_libraries(BCnCompressglTF ZetaCore ole32.lib shell32.lib version.lib d3d11.lib dxgi.lib)
set_target_properties(BCnCompressglTF PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

target_compile_options(BCnCompressglTF PRIVATE /fp:precise "$<$<NOT:$<CONFIG:DEBUG>>:/guard:cf>")
target_link_options(BCnCompressglTF PRIVATE /DYNAMICBASE /NXCOMPAT)

if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.24)
    target_compile_options(BCnCompressglTF PRIVATE /ZH:SHA_256)
endif()

if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.27)
    target_link_options(BCnCompressglTF PRIVATE /CETCOMPAT)
endif()

set(WarningsEXE "/wd4061" "/wd4062" "/wd4365" "/wd4514" "/wd4625" "/wd4626" "/wd4627" "/wd4668" "/wd4710" "/wd4751" "/wd4820" "/wd5026" "/wd5027" "/wd5039" "/wd5045" "/wd5219")
target_compile_options(BCnCompressglTF PRIVATE ${WarningsEXE})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "BCnCompressglTF" FILES ${SOURCES})

set_target_properties(BCnCompressglTF PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
set_target_properties(BCnCompressglTF PROPERTIES FOLDER "Tools")