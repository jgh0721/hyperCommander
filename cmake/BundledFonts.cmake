include_guard(GLOBAL)

function(hc_add_bundled_fonts TARGET)
    set(FONT_RESOURCE "${CMAKE_BINARY_DIR}/hc-fonts/D2Coding.rcc")
    if(NOT TARGET hc_d2coding_fonts)
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/hc-fonts")
        qt_add_binary_resources(hc_d2coding_fonts
            "${PROJECT_SOURCE_DIR}/tools/d2coding/d2coding.qrc"
            DESTINATION "${FONT_RESOURCE}"
            OPTIONS --compress-algo zlib --compress 9 --threshold 0
        )
    endif()
    # Copy even when only the font data changes and the executable does not relink.
    add_custom_target(${TARGET}_fonts
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:${TARGET}>/Libs/Fonts"
            "$<TARGET_FILE_DIR:${TARGET}>/Licenses/D2Coding"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${FONT_RESOURCE}"
            "$<TARGET_FILE_DIR:${TARGET}>/Libs/Fonts/D2Coding.rcc"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/tools/d2coding/OFL.txt"
            "$<TARGET_FILE_DIR:${TARGET}>/Licenses/D2Coding/OFL.txt"
        DEPENDS hc_d2coding_fonts
        VERBATIM
    )
    add_dependencies(${TARGET} ${TARGET}_fonts)
endfunction()
