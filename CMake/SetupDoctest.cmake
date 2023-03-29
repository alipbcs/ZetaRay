function(SetupDoctest)
    set(DOCTEST_DIR "${EXTERNAL_DIR}/doctest")
    file(GLOB_RECURSE HEADER_PATH "${DOCTEST_DIR}/doctest.h")
    
    if(HEADER_PATH STREQUAL "")
        file(MAKE_DIRECTORY ${DOCTEST_DIR})
        set(URL "https://github.com/doctest/doctest/releases/download/v2.4.11/doctest.h")
        message(STATUS "Downloading doctest 2.4.11 from ${URL}...")
        file(DOWNLOAD "${URL}" "${DOCTEST_DIR}/doctest.h" TIMEOUT 60)
    endif()   
endfunction()