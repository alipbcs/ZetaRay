function(Copy FILES DEST TARGET_NAME)
    foreach(FULL_FILENAME ${FILES})
        get_filename_component(FILE ${FULL_FILENAME} NAME)
        add_custom_command(
            OUTPUT ${DEST}/${FILE}
            PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FULL_FILENAME} ${DEST}
            MAIN_DEPENDENCY ${FULL_FILENAME}
            COMMENT "Copying ${FILE} into ${DEST}..." )
        list(APPEND ALL_DESTS ${DEST}/${FILE})
    endforeach()

    add_custom_target(${TARGET_NAME} DEPENDS "${ALL_DESTS}")
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "CopyTargets")
endfunction()