function(SetupWinPIX)
    set(PIX_DIR "${EXTERNAL_DIR}/WinPixEventRuntime")
    file(GLOB_RECURSE DLL_PATH "${PIX_DIR}/*WinPixEventRuntime.dll")
    
    if(DLL_PATH STREQUAL "")
        file(MAKE_DIRECTORY ${PIX_DIR})
    
        # download from nuget
        set(URL "https://www.nuget.org/api/v2/package/WinPixEventRuntime/1.0.230302001")
        message(STATUS "Downloading WinPixEventRuntime from ${URL}...")
        set(ARCHIVE_PATH "${PIX_DIR}/temp/pix.zip")
        file(DOWNLOAD "${URL}" "${ARCHIVE_PATH}" TIMEOUT 120)        
        file(ARCHIVE_EXTRACT INPUT "${ARCHIVE_PATH}" DESTINATION "${PIX_DIR}/temp")
        
        # copy headers
        file(GLOB_RECURSE PIX_HEADERS "${PIX_DIR}/temp/include/*.h")
        file(COPY ${PIX_HEADERS} DESTINATION ${PIX_DIR})
        
        if(PIX_HEADERS STREQUAL "")
            message(FATAL_ERROR "Setting up WinPixEventRuntime failed.")
        endif()      

        # copy binaries
        set(BINS
            "${PIX_DIR}/temp/bin/x64/WinPixEventRuntime.dll"
            "${PIX_DIR}/temp/bin/x64/WinPixEventRuntime.lib")

        file(COPY ${BINS} DESTINATION ${PIX_DIR})
    
        # cleanup
        file(REMOVE_RECURSE "${PIX_DIR}/temp")
    endif()   
endfunction()