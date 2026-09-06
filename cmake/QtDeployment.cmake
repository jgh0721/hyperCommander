if(WIN32)
    # The Hybrid CRT kit's windeployqt misclassifies release plugins as debug.
    # Imported Qt targets provide the actual per-configuration plugin paths.
    get_property(HC_IMPORTED_TARGETS DIRECTORY PROPERTY IMPORTED_TARGETS)
    set(HC_PLUGIN_MANIFEST "set(HC_PLUGIN_FILES\n")
    foreach(HC_IMPORTED_TARGET IN LISTS HC_IMPORTED_TARGETS)
        get_target_property(HC_PLUGIN_TYPE ${HC_IMPORTED_TARGET} QT_PLUGIN_TYPE)
        if(HC_PLUGIN_TYPE MATCHES "^(platforms|imageformats|iconengines|styles|generic|networkinformation|tls|sqldrivers|printsupport)$")
            string(APPEND HC_PLUGIN_MANIFEST "    \"$<TARGET_FILE:${HC_IMPORTED_TARGET}>\"\n")
        endif()
    endforeach()
    string(APPEND HC_PLUGIN_MANIFEST ")\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/hc-qt-plugins-$<CONFIG>.cmake" CONTENT "${HC_PLUGIN_MANIFEST}")
endif()

function(hc_enable_qt_deployment Target)
    if(NOT WIN32)
        return()
    endif()
    cmake_parse_arguments(DEPLOY "TEST;PLUGINS" "RUNTIME_FILE" "" ${ARGN})
    # The VS generator records its selected installation, optionally followed by
    # comma-separated instance fields. Reuse it instead of probing another VS.
    string(REGEX REPLACE ",.*$" "" HC_VS_DIR "${CMAKE_GENERATOR_INSTANCE}")
    set(HC_ARGS
        "-DHC_DEPLOY_EXE=$<TARGET_FILE:${Target}>"
        "-DHC_QT_BIN=$<TARGET_FILE_DIR:Qt6::Core>"
        "-DHC_GUI_DLL=$<TARGET_FILE:Qt6::Gui>"
        "-DHC_CONFIG=$<CONFIG>"
        "-DHC_VS_DIR=${HC_VS_DIR}"
        "-DHC_WITH_TEST=${DEPLOY_TEST}"
        "-DHC_WITH_PLUGINS=${DEPLOY_PLUGINS}"
        "-DHC_PLUGIN_MANIFEST=${CMAKE_BINARY_DIR}/hc-qt-plugins-$<CONFIG>.cmake"
        "-DHC_RUNTIME_FILE=${DEPLOY_RUNTIME_FILE}"
        -P "${PROJECT_SOURCE_DIR}/cmake/DeployQt.cmake"
    )
    add_custom_command(TARGET ${Target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" ${HC_ARGS}
        COMMENT "Deploying the selected Qt runtime for ${Target}"
        VERBATIM
    )
    set_property(GLOBAL APPEND PROPERTY HC_DEPLOY_TARGETS ${Target})
    set_property(TARGET ${Target} PROPERTY HC_DEPLOY_ARGS "${HC_ARGS}")
endfunction()

function(hc_finalize_qt_deployment)
    if(NOT WIN32)
        return()
    endif()
    get_property(HC_TARGETS GLOBAL PROPERTY HC_DEPLOY_TARGETS)
    # Refresh runtime files even when an ordinary ALL build does not relink.
    add_custom_target(hc_deploy ALL DEPENDS ${HC_TARGETS})
    foreach(HC_TARGET IN LISTS HC_TARGETS)
        get_property(HC_ARGS TARGET ${HC_TARGET} PROPERTY HC_DEPLOY_ARGS)
        add_custom_command(TARGET hc_deploy POST_BUILD COMMAND "${CMAKE_COMMAND}" ${HC_ARGS} VERBATIM)
    endforeach()
    if(TARGET hc_check)
        add_dependencies(hc_check hc_deploy)
    endif()
endfunction()
