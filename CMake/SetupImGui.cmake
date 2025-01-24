function(SetupImGui)
    set(IMGUI_VER "1.91.6")
    set(IMPLOT_COMMIT "47522f47054d33178e7defa780042bd2a06b09f9")
    set(IMNODES_COMMIT "8563e1655bd9bb1f249e6552cc6274d506ee788b")
    set(IMGUI_DIR "${EXTERNAL_DIR}/ImGui")
    file(GLOB_RECURSE IMGUI_HEADER_PATH "${IMGUI_DIR}/*imgui.h")
    file(GLOB_RECURSE IMPLOT_HEADER_PATH "${IMGUI_DIR}/*implot.h")
    file(GLOB_RECURSE IMNODES_HEADER_PATH "${IMGUI_DIR}/*imnodes.h")

    if(IMGUI_HEADER_PATH STREQUAL "")
        # download ImGui source code
        set(URL "https://github.com/ocornut/imgui/archive/refs/tags/v${IMGUI_VER}.zip")
        message(STATUS "Downloading ImGui ${IMGUI_VER} from ${URL}...")
        set(ARCHIVE_PATH "${IMGUI_DIR}/temp/imgui.zip")
        file(DOWNLOAD "${URL}" "${ARCHIVE_PATH}" TIMEOUT 120)       
        file(ARCHIVE_EXTRACT INPUT "${ARCHIVE_PATH}" DESTINATION "${IMGUI_DIR}/temp")

        # copy to ImGui directory
        set(FILES 
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imgui_draw.cpp"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imgui_internal.h"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imgui_tables.cpp"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imgui_widgets.cpp"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imgui.cpp"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imgui.h"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imstb_rectpack.h"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imstb_textedit.h"
            "${IMGUI_DIR}/temp/imgui-${IMGUI_VER}/imstb_truetype.h")

        file(COPY ${FILES} DESTINATION ${IMGUI_DIR})

        # cleanup
        file(REMOVE_RECURSE "${IMGUI_DIR}/temp")
    endif()

    if(IMPLOT_HEADER_PATH STREQUAL "")
        # download ImPlot source code
        set(URL "https://github.com/epezent/implot/archive/${IMPLOT_COMMIT}.zip")
        message(STATUS "Downloading ImPlot from ${URL}...")
        set(ARCHIVE_PATH "${IMGUI_DIR}/temp/implot.zip")
        file(DOWNLOAD "${URL}" "${ARCHIVE_PATH}" TIMEOUT 120)       
        file(ARCHIVE_EXTRACT INPUT "${ARCHIVE_PATH}" DESTINATION "${IMGUI_DIR}/temp")

        # copy to ImGui directory
        set(FILES 
            "${IMGUI_DIR}/temp/implot-${IMPLOT_COMMIT}/implot_internal.h"
            "${IMGUI_DIR}/temp/implot-${IMPLOT_COMMIT}/implot_items.cpp"
            "${IMGUI_DIR}/temp/implot-${IMPLOT_COMMIT}/implot.cpp"
            "${IMGUI_DIR}/temp/implot-${IMPLOT_COMMIT}/implot.h")

        file(COPY ${FILES} DESTINATION ${IMGUI_DIR})

        # cleanup
        file(REMOVE_RECURSE "${IMGUI_DIR}/temp")
    endif()

    if(IMNODES_HEADER_PATH STREQUAL "")
        # download imnodes source code
        set(URL "https://github.com/Nelarius/imnodes/archive/${IMNODES_COMMIT}.zip")
        message(STATUS "Downloading imnodes from ${URL}...")
        set(ARCHIVE_PATH "${IMGUI_DIR}/temp/imnodes.zip")
        file(DOWNLOAD "${URL}" "${ARCHIVE_PATH}" TIMEOUT 120)       
        file(ARCHIVE_EXTRACT INPUT "${ARCHIVE_PATH}" DESTINATION "${IMGUI_DIR}/temp")

        # copy to ImGui directory
        set(FILES 
            "${IMGUI_DIR}/temp/imnodes-${IMNODES_COMMIT}/imnodes_internal.h"
            "${IMGUI_DIR}/temp/imnodes-${IMNODES_COMMIT}/imnodes.cpp"
            "${IMGUI_DIR}/temp/imnodes-${IMNODES_COMMIT}/imnodes.h")

        file(COPY ${FILES} DESTINATION ${IMGUI_DIR})

        # cleanup
        file(REMOVE_RECURSE "${IMGUI_DIR}/temp")
    endif()   
endfunction()