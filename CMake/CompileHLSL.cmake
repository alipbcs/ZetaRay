# Returns a list of included paths (if any)
function(ParseIncludes DATA DIR RET)
    string(REGEX MATCHALL "#include[ \t]*[\"]([^\"]+)[\"]" ALL_MATCHES "${DATA}")
    set(INCLUDES "")

    foreach(INCLUDE_LINE ${ALL_MATCHES})
        string(REGEX REPLACE "#include[ \t]*[\"]([^\"]+)[\"]" "\\1" INCLUDED_FILE "${INCLUDE_LINE}")

        # Skip if extension is not .hlsli, .hlsl or .h
        get_filename_component(EX "${INCLUDED_FILE}" LAST_EXT)
        if(NOT "${EX}" STREQUAL ".hlsli" AND NOT "${EX}" STREQUAL ".hlsl" AND NOT "${EX}" STREQUAL ".h")
            continue()
        endif()

        set(CURR_PATH "${DIR}/${INCLUDED_FILE}")

        # Necessary in order to detect duplicate paths
        get_filename_component(CURR_PATH_ABS "${CURR_PATH}" ABSOLUTE)

        set(INCLUDES ${INCLUDES} "${CURR_PATH_ABS}")
    endforeach()

    set(${RET} ${INCLUDES} PARENT_SCOPE)
endfunction()

# DFS search to recursively find all included paths
function(DFS FILE_PATH FILE_CONTENTS RET)
    get_filename_component(CURR_DIR "${FILE_PATH}" DIRECTORY)
    ParseIncludes("${FILE_CONTENTS}" "${CURR_DIR}" INCLUDES)
    set(ALL ${INCLUDES})

    foreach(INCLUDE ${INCLUDES})
        # Skip searching cpp headers that are not in render pass directory
        get_filename_component(FILE_NAME "${INCLUDE}" LAST_EXT)
        string(SUBSTRING "${INCLUDE}" 0 ${ZETA_RENDER_PASS_DIR_LEN} TEMP)

        if("${FILE_NAME}" STREQUAL ".h" AND NOT "${TEMP}" STREQUAL "${ZETA_RENDER_PASS_DIR}")
            continue()
        endif()

        file(STRINGS "${INCLUDE}" NEXT_FILE_CONTENTS NEWLINE_CONSUME)
        DFS("${INCLUDE}" "${NEXT_FILE_CONTENTS}" NEXT_INCLUDES)
        set(ALL ${ALL} ${NEXT_INCLUDES})
    endforeach()

    # Remove duplicate paths
    list(REMOVE_DUPLICATES ALL)

    set(${RET} ${ALL} PARENT_SCOPE)
endfunction()

function(CompileHLSL HLSL_PATH RET)
    # DXC compiler
    find_program(DXC dxc PATHS "${DXC_BIN_DIR}" REQUIRED NO_DEFAULT_PATH)

    get_filename_component(FILE_NAME_WO_EXT ${HLSL_PATH} NAME_WLE)
    file(STRINGS "${HLSL_PATH}" DATA NEWLINE_CONSUME)
    DFS("${HLSL_PATH}" ${DATA} ALL_INCLUDES)

    # Figure out if shader is a compute shader, DXIL lib, etc

    set(COMMON_DBG_ARGS "-Qembed_debug" "-Qstrip_reflect" "-nologo" "-Zi" "-all_resources_bound" "-enable-16bit-types" "-HV 2021" "-WX")
    set(COMMON_RLS_ARGS "-Qstrip_reflect" "-nologo" "-all_resources_bound" "-enable-16bit-types" "-HV 2021" "-WX")
    
    # Compute shader
    set(RE_CS "\\[numthreads.*\\][ \t\r\n]*void[ \t\r\n]+([a-zA-Z][A-Za-z0-9_]*)")
    string(REGEX MATCH ${RE_CS} MATCH ${DATA})

    if(${CMAKE_MATCH_COUNT} GREATER 0)
        set(MAIN_FUNC ${CMAKE_MATCH_1})
        set(CSO_PATH_DBG "${CSO_DIR_DEBUG}/${FILE_NAME_WO_EXT}_cs.cso")
        set(CSO_PATH_RLS "${CSO_DIR_RELEASE}/${FILE_NAME_WO_EXT}_cs.cso")

        if(COMPILE_SHADERS_WITH_DEBUG)
            add_custom_command(
                OUTPUT ${CSO_PATH_DBG} ${CSO_PATH_RLS}
                COMMAND ${DXC} ${COMMON_DBG_ARGS} -T cs_6_7 -E ${MAIN_FUNC} -Fo ${CSO_PATH_DBG} ${HLSL_PATH}
                COMMAND ${DXC} ${COMMON_RLS_ARGS} -T cs_6_7 -E ${MAIN_FUNC} -Fo ${CSO_PATH_RLS} ${HLSL_PATH}
                DEPENDS ${ALL_INCLUDES} "${HLSL_PATH}"
                COMMENT "Compiling HLSL source file ${FILE_NAME_WO_EXT}.hlsl..."
                VERBATIM)

            set(CSOS ${CSO_PATH_DBG} ${CSO_PATH_RLS})
        else()
            add_custom_command(
                OUTPUT ${CSO_PATH_RLS}
                COMMAND ${DXC} ${COMMON_RLS_ARGS} -T cs_6_7 -E ${MAIN_FUNC} -Fo ${CSO_PATH_RLS} ${HLSL_PATH}
                DEPENDS ${ALL_INCLUDES} "${HLSL_PATH}"
                COMMENT "Compiling HLSL source file ${FILE_NAME_WO_EXT}.hlsl..."
                VERBATIM)

            set(CSOS ${CSO_PATH_RLS})
        endif()
    else()
        # RT lib
        set(RE_DXIL "\\[shader(.*)\\][ \t\r\n]")
        string(REGEX MATCH ${RE_DXIL} MATCH ${DATA})
        
        if(${CMAKE_MATCH_COUNT} GREATER 0)
        set(CSO_PATH_DBG "${CSO_DIR_DEBUG}/${FILE_NAME_WO_EXT}_lib.cso")
            set(CSO_PATH_RLS "${CSO_DIR_RELEASE}/${FILE_NAME_WO_EXT}_lib.cso")
            
            if(COMPILE_SHADERS_WITH_DEBUG)
                add_custom_command(
                    OUTPUT ${CSO_PATH_DBG} ${CSO_PATH_RLS}
                    COMMAND ${DXC} ${COMMON_DBG_ARGS} -T lib_6_7 -Fo ${CSO_PATH_DBG} ${HLSL_PATH}
                    COMMAND ${DXC} ${COMMON_RLS_ARGS} -T lib_6_7 -Fo ${CSO_PATH_RLS} ${HLSL_PATH}
                    DEPENDS ${ALL_INCLUDES} "${HLSL_PATH}"
                    COMMENT "Compiling DXIL library ${FILE_NAME_WO_EXT}.hlsl..."
                    VERBATIM)

                set(CSOS ${CSO_PATH_DBG} ${CSO_PATH_RLS})
            else()
                add_custom_command(
                    OUTPUT ${CSO_PATH_RLS}
                    COMMAND ${DXC} ${COMMON_RLS_ARGS} -T lib_6_7 -Fo ${CSO_PATH_RLS} ${HLSL_PATH}
                    DEPENDS ${ALL_INCLUDES} "${HLSL_PATH}"
                    COMMENT "Compiling DXIL library ${FILE_NAME_WO_EXT}.hlsl..."
                    VERBATIM)

                set(CSOS ${CSO_PATH_RLS})
            endif()
        else()
            # Compute shader that includes another .hlsl. Further assumes included hlsl has a "main" entry point.
            set(RE_INCLUDE_HLSL "#include[ \t]*[\"<]([^\">]+)\\.hlsl[\">]")
            string(REGEX MATCH ${RE_INCLUDE_HLSL} MATCH ${DATA})
                    
            if(${CMAKE_MATCH_COUNT} GREATER 0)
                set(CSO_PATH_DBG "${CSO_DIR_DEBUG}/${FILE_NAME_WO_EXT}_cs.cso")
                set(CSO_PATH_RLS "${CSO_DIR_RELEASE}/${FILE_NAME_WO_EXT}_cs.cso")

                if(COMPILE_SHADERS_WITH_DEBUG)            
                    add_custom_command(
                        OUTPUT ${CSO_PATH_DBG} ${CSO_PATH_RLS}
                        COMMAND ${DXC} ${COMMON_DBG_ARGS} -T cs_6_7 -E main -Fo ${CSO_PATH_DBG} ${HLSL_PATH}
                        COMMAND ${DXC} ${COMMON_RLS_ARGS} -T cs_6_7 -E main -Fo ${CSO_PATH_RLS} ${HLSL_PATH}
                        DEPENDS ${ALL_INCLUDES} "${HLSL_PATH}"
                        COMMENT "Compiling HLSL source file ${FILE_NAME_WO_EXT}.hlsl..."
                        VERBATIM)
            
                    set(CSOS ${CSO_PATH_DBG} ${CSO_PATH_RLS})
                else()
                    add_custom_command(
                        OUTPUT ${CSO_PATH_RLS}
                        COMMAND ${DXC} ${COMMON_RLS_ARGS} -T cs_6_7 -E main -Fo ${CSO_PATH_RLS} ${HLSL_PATH}
                        DEPENDS ${ALL_INCLUDES} "${HLSL_PATH}"
                        COMMENT "Compiling HLSL source file ${FILE_NAME_WO_EXT}.hlsl..."
                        VERBATIM)
            
                    set(CSOS ${CSO_PATH_RLS})
                endif()
            # VS-PS
            else()
                # Vertex shader
                set(CSO_PATH_VS_DBG ${CSO_DIR_DEBUG}/${FILE_NAME_WO_EXT}_vs.cso)
                set(CSO_PATH_VS_RLS ${CSO_DIR_RELEASE}/${FILE_NAME_WO_EXT}_vs.cso)
                # Pixel shader
                set(CSO_PATH_PS_DBG ${CSO_DIR_DEBUG}/${FILE_NAME_WO_EXT}_ps.cso)
                set(CSO_PATH_PS_RLS ${CSO_DIR_RELEASE}/${FILE_NAME_WO_EXT}_ps.cso)
                
                if(COMPILE_SHADERS_WITH_DEBUG)
                    add_custom_command(
                        OUTPUT ${CSO_PATH_VS_DBG} ${CSO_PATH_PS_DBG} ${CSO_PATH_VS_RLS} ${CSO_PATH_PS_RLS}
                        COMMAND ${DXC} ${COMMON_DBG_ARGS} -T vs_6_6 -E mainVS -Fo ${CSO_PATH_VS_DBG} ${HLSL_PATH}
                        COMMAND ${DXC} ${COMMON_DBG_ARGS} -T ps_6_6 -E mainPS -Fo ${CSO_PATH_PS_DBG} ${HLSL_PATH}
                        COMMAND ${DXC} ${COMMON_RLS_ARGS} -T vs_6_6 -E mainVS -Fo ${CSO_PATH_VS_RLS} ${HLSL_PATH}
                        COMMAND ${DXC} ${COMMON_RLS_ARGS} -T ps_6_6 -E mainPS -Fo ${CSO_PATH_PS_RLS} ${HLSL_PATH}
                        DEPENDS ${ALL_INCLUDES} "${HLSL_PATH}"
                        COMMENT "Compiling HLSL source file ${FILE_NAME_WO_EXT}.hlsl..."
                        VERBATIM)

                    set(CSOS ${CSO_PATH_VS_DBG} ${CSO_PATH_VS_RLS} ${CSO_PATH_PS_DBG} ${CSO_PATH_PS_RLS})
                else()
                    add_custom_command(
                        OUTPUT ${CSO_PATH_VS_RLS} ${CSO_PATH_PS_RLS}
                        COMMAND ${DXC} ${COMMON_RLS_ARGS} -T vs_6_6 -E mainVS -Fo ${CSO_PATH_VS_RLS} ${HLSL_PATH}
                        COMMAND ${DXC} ${COMMON_RLS_ARGS} -T ps_6_6 -E mainPS -Fo ${CSO_PATH_PS_RLS} ${HLSL_PATH}
                        DEPENDS ${ALL_INCLUDES} "${HLSL_PATH}"
                        COMMENT "Compiling HLSL source file ${FILE_NAME_WO_EXT}.hlsl..."
                        VERBATIM)

                    set(CSOS ${CSO_PATH_VS_RLS} ${CSO_PATH_PS_RLS})
                endif()
            endif()
        endif()
    endif()

    set(${RET} ${CSOS} PARENT_SCOPE)
endfunction()