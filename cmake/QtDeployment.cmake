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
    if(TARGET hc_image_codecs_plugin)
        string(APPEND HC_PLUGIN_MANIFEST "    \"$<TARGET_FILE:hc_image_codecs_plugin>\"\n")
    endif()
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
        "-DHC_IMAGE_CODEC_PREFIX=${HC_IMAGE_CODEC_PREFIX}"
        -P "${PROJECT_SOURCE_DIR}/cmake/DeployQt.cmake"
    )
    if(TARGET hc_image_codecs_plugin)
        add_dependencies(${Target} hc_image_codecs_plugin)
    endif()
    add_custom_command(TARGET ${Target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" ${HC_ARGS}
        COMMENT "Deploying the selected Qt runtime for ${Target}"
        VERBATIM
    )
    set_property(GLOBAL APPEND PROPERTY HC_DEPLOY_TARGETS ${Target})
    set_property(TARGET ${Target} PROPERTY HC_DEPLOY_ARGS "${HC_ARGS}")
endfunction()

# Every Qt executable sharing the output directory finds Libs/ through an embedded
# :/qt/etc/qt.conf, which Qt reads before any qt.conf file beside the exe.
function(hc_collect_executables Directory Result)
    get_property(Targets DIRECTORY "${Directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(Children DIRECTORY "${Directory}" PROPERTY SUBDIRECTORIES)
    foreach(Child IN LISTS Children)
        if(NOT Child MATCHES "/externalLibs/")
            hc_collect_executables("${Child}" ChildTargets)
            list(APPEND Targets ${ChildTargets})
        endif()
    endforeach()
    set(${Result} ${Targets} PARENT_SCOPE)
endfunction()

function(hc_embed_qt_conf)
    hc_collect_executables("${PROJECT_SOURCE_DIR}" HC_ALL_TARGETS)
    foreach(HC_TARGET IN LISTS HC_ALL_TARGETS)
        get_target_property(HC_TYPE ${HC_TARGET} TYPE)
        get_target_property(HC_NATIVE ${HC_TARGET} HC_NATIVE_EXECUTABLE)
        # Native fixtures must not gain a Qt6Core import: they run from directories without Qt DLLs.
        if(NOT HC_TYPE STREQUAL "EXECUTABLE" OR HC_NATIVE)
            continue()
        endif()
        # Generated sources land in this directory, so each target needs its own resource name.
        string(MAKE_C_IDENTIFIER "hc_qt_conf_${HC_TARGET}" HC_RESOURCE)
        qt_add_resources(${HC_TARGET} ${HC_RESOURCE} PREFIX "/qt/etc"
            BASE "${PROJECT_SOURCE_DIR}/cmake/productQtConf" FILES "${PROJECT_SOURCE_DIR}/cmake/productQtConf/qt.conf")
    endforeach()
endfunction()

function(hc_finalize_qt_deployment)
    if(NOT WIN32)
        return()
    endif()
    hc_embed_qt_conf()
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
