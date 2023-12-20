function(SetupxxHash)
    set(XXHASH_DIR "${EXTERNAL_DIR}/xxHash")
    file(GLOB_RECURSE HEADER_PATH "${XXHASH_DIR}/xxHash.h")

    if(HEADER_PATH STREQUAL "")
        file(MAKE_DIRECTORY ${XXHASH_DIR})

        # download
        set(URL "https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.1.zip")
        message(STATUS "Downloading xxHash 0.8.1 from ${URL}...")
        set(ARCHIVE_PATH "${XXHASH_DIR}/temp/xxhash.zip")
        file(DOWNLOAD "${URL}" "${ARCHIVE_PATH}" TIMEOUT 120)        
        file(ARCHIVE_EXTRACT INPUT "${ARCHIVE_PATH}" DESTINATION "${XXHASH_DIR}/temp")

        # copy header
        file(GLOB_RECURSE HEADER "${XXHASH_DIR}/temp/*xxhash.h")
        file(COPY ${HEADER} DESTINATION ${XXHASH_DIR})

        if(HEADER STREQUAL "")
            message(FATAL_ERROR "Setting up xxHash failed.")
        endif()

        # cleanup
        file(REMOVE_RECURSE "${XXHASH_DIR}/temp")
    endif()   
endfunction()