function(SetupImGui)
    set(IMGUI_VER "1.89.8")
    set(IMPLOT_VER "0.15")
    set(IMNODES_COMMIT "d88f99125bb72cdb71b4c27ff6eb7f318d89a4c5")
    set(IMGUI_DIR "${EXTERNAL_DIR}/ImGui")
    file(GLOB_RECURSE HEADER_PATH "${IMGUI_DIR}/*imgui.h")
    
    if(HEADER_PATH STREQUAL "")
        # download ImGui source code
        set(URL "https://github.com/ocornut/imgui/archive/refs/tags/v${IMGUI_VER}.zip")
        message(STATUS "Downloading ImGui ${IMGUI_VER} from ${URL}...")
        set(ARCHIVE_PATH "${IMGUI_DIR}/temp/imgui.zip")
        file(DOWNLOAD "${URL}" "${ARCHIVE_PATH}" TIMEOUT 120)       
        file(ARCHIVE_EXTRACT INPUT "${ARCHIVE_PATH}" DESTINATION "${IMGUI_DIR}/temp")

        # download ImPlot source code
        set(URL "https://github.com/epezent/implot/archive/refs/tags/v${IMPLOT_VER}.zip")
        message(STATUS "Downloading ImPlot ${IMPLOT_VER} from ${URL}...")
        set(ARCHIVE_PATH "${IMGUI_DIR}/temp/implot.zip")
        file(DOWNLOAD "${URL}" "${ARCHIVE_PATH}" TIMEOUT 120)       
        file(ARCHIVE_EXTRACT INPUT "${ARCHIVE_PATH}" DESTINATION "${IMGUI_DIR}/temp")

        # download imnodes source code
        set(URL "https://github.com/Nelarius/imnodes/archive/${IMNODES_COMMIT}.zip")
        message(STATUS "Downloading imnodes from ${URL}...")
        set(ARCHIVE_PATH "${IMGUI_DIR}/temp/imnodes.zip")
        file(DOWNLOAD "${URL}" "${ARCHIVE_PATH}" TIMEOUT 120)       
        file(ARCHIVE_EXTRACT INPUT "${ARCHIVE_PATH}" DESTINATION "${IMGUI_DIR}/temp")
        
        # copy headers & source files
        set(FILES 
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imgui_draw.cpp"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imgui_internal.h"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imgui_tables.cpp"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imgui_widgets.cpp"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imgui.cpp"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imgui.h"
            "${IMGUI_DIR}/temp/imnodes-${IMNODES_COMMIT}/imnodes_internal.h"
            "${IMGUI_DIR}/temp/imnodes-${IMNODES_COMMIT}/imnodes.cpp"
            "${IMGUI_DIR}/temp/imnodes-${IMNODES_COMMIT}/imnodes.h"
            "${IMGUI_DIR}/temp/implot-${IMPLOT_VER}/implot_internal.h"
            "${IMGUI_DIR}/temp/implot-${IMPLOT_VER}/implot_items.cpp"
            "${IMGUI_DIR}/temp/implot-${IMPLOT_VER}/implot.cpp"
            "${IMGUI_DIR}/temp/implot-${IMPLOT_VER}/implot.h"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imstb_rectpack.h"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imstb_textedit.h"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imstb_truetype.h")

        file(COPY ${FILES} DESTINATION ${IMGUI_DIR})

        # cleanup
        file(REMOVE_RECURSE "${IMGUI_DIR}/temp")
    endif()   
endfunction()