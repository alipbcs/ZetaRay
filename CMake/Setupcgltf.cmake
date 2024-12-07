function(Setupcgltf)
    set(CGLTF_DIR "${EXTERNAL_DIR}/cgltf")
    file(GLOB_RECURSE HEADER_PATH "${CGLTF_DIR}/cgltf.h")
    set(CGLTF_VER "1.14")

    if(HEADER_PATH STREQUAL "")
        file(MAKE_DIRECTORY ${CGLTF_DIR})

        # download
        set(URL "https://github.com/jkuhlmann/cgltf/archive/refs/tags/v${CGLTF_VER}.zip")
        message(STATUS "Downloading cgltf ${CGLTF_VER} from ${URL}...")
        set(ARCHIVE_PATH "${CGLTF_DIR}/temp/cgltf.zip")
        file(DOWNLOAD "${URL}" "${ARCHIVE_PATH}" TIMEOUT 120)
        file(ARCHIVE_EXTRACT INPUT "${ARCHIVE_PATH}" DESTINATION "${CGLTF_DIR}/temp")

        # copy header
        file(GLOB_RECURSE HEADER "${CGLTF_DIR}/temp/*cgltf*.h")
        file(COPY ${HEADER} DESTINATION ${CGLTF_DIR})
            
        if(HEADER STREQUAL "")
            message(FATAL_ERROR "Setting up cgltf failed.")
        endif()

        # cleanup
        file(REMOVE_RECURSE "${CGLTF_DIR}/temp")
    endif()   
endfunction()