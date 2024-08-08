function(SetupDXC DXC_BIN_DIR)
    set(DXC_DIR "${TOOLS_DIR}/dxc")
    file(GLOB_RECURSE DXC_BIN_PATH "${DXC_DIR}/*dxc.exe")

    # if(NOT EXISTS "${DXC_BIN_DIR}")
    if(DXC_BIN_PATH STREQUAL "")
        set(URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2407/dxc_2024_07_31_clang_cl.zip")
        message(STATUS "Downloading DXC from ${URL}...")
        set(ARCHIVE_PATH "${TOOLS_DIR}/dxc.zip")
        file(DOWNLOAD "${URL}" "${ARCHIVE_PATH}" TIMEOUT 120)

        file(ARCHIVE_EXTRACT INPUT "${ARCHIVE_PATH}" DESTINATION "${DXC_DIR}")
        file(REMOVE_RECURSE "${DXC_DIR}/bin/arm64")
        file(REMOVE_RECURSE "${DXC_DIR}/bin/x86")
        file(REMOVE_RECURSE "${DXC_DIR}/lib")
        file(REMOVE_RECURSE "${DXC_DIR}/inc")
        file(REMOVE "${ARCHIVE_PATH}")

        file(GLOB_RECURSE DXC_BIN_PATH "${DXC_DIR}/*dxc.exe")

        if(DXC_BIN_PATH STREQUAL "")
            message(FATAL_ERROR "Setting up DXC failed.")
        endif()
    endif()

    get_filename_component(PARENT_DIR ${DXC_BIN_PATH} DIRECTORY)
    set(${DXC_BIN_DIR} ${PARENT_DIR} PARENT_SCOPE)
endfunction()