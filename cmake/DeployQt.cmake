if(NOT EXISTS "${HC_DEPLOY_EXE}" OR NOT EXISTS "${HC_QT_BIN}/windeployqt.exe")
    message(FATAL_ERROR "Qt deployment input missing: ${HC_DEPLOY_EXE} / ${HC_QT_BIN}")
endif()
get_filename_component(HC_OUTPUT "${HC_DEPLOY_EXE}" DIRECTORY)
# Multiple test/application targets share one configuration directory. Serialize
# deployment across MSBuild processes to avoid concurrently replacing Qt DLLs.
file(LOCK "${HC_OUTPUT}/.hc-qt-deploy.lock" GUARD PROCESS TIMEOUT 180)
if(HC_CONFIG STREQUAL "Debug")
    set(HC_MODE --debug)
    set(HC_TEST_DLL Qt6Testd.dll)
else()
    set(HC_MODE --release)
    set(HC_TEST_DLL Qt6Test.dll)
endif()
set(ENV{PATH} "${HC_QT_BIN};$ENV{PATH}")
if(HC_VS_DIR AND EXISTS "${HC_VS_DIR}/VC")
    set(ENV{VCINSTALLDIR} "${HC_VS_DIR}/VC/")
endif()
# Custom kits may omit the translation catalog needed by windeployqt.
set(HC_TRANSLATION_ARGS)
# Plugins, translations and WebEngine data live under Libs/; Qt DLLs stay beside the exe.
# Each exe finds them through its embedded qt.conf (cmake/productQtConf/qt.conf).
set(HC_PLUGIN_ROOT "${HC_OUTPUT}/Libs")
if(HC_IMAGE_CODEC_PREFIX)
    file(GLOB HC_CODEC_DLLS "${HC_IMAGE_CODEC_PREFIX}/bin/*.dll")
    foreach(HC_CODEC_DLL IN LISTS HC_CODEC_DLLS)
        file(COPY "${HC_CODEC_DLL}" DESTINATION "${HC_PLUGIN_ROOT}/imageformats")
    endforeach()
    file(COPY "${HC_IMAGE_CODEC_PREFIX}/Licenses/" DESTINATION "${HC_OUTPUT}/Licenses/ImageCodecs")
    file(COPY "${CMAKE_CURRENT_LIST_DIR}/../tools/Build-ImageCodecs.ps1"
        DESTINATION "${HC_OUTPUT}/Licenses/ImageCodecs/build/tools")
    file(COPY "${CMAKE_CURRENT_LIST_DIR}/ImageCodecDav1d.cmake"
        DESTINATION "${HC_OUTPUT}/Licenses/ImageCodecs/build/cmake")
endif()
if(NOT EXISTS "${HC_QT_BIN}/../translations/catalogs.json")
    list(APPEND HC_TRANSLATION_ARGS --no-translations)
else()
    list(APPEND HC_TRANSLATION_ARGS --translationdir "${HC_PLUGIN_ROOT}/translations")
endif()
set(HC_PLUGIN_FILES)
if(HC_WITH_PLUGINS)
    include("${HC_PLUGIN_MANIFEST}")
    if(NOT HC_PLUGIN_FILES)
        message(FATAL_ERROR "No imported Qt plugins available for ${HC_DEPLOY_EXE}")
    endif()
endif()
set(HC_DEPLOY_ARGS ${HC_MODE}
        --qtpaths "${HC_QT_BIN}/qtpaths.exe"
        --dir "${HC_OUTPUT}" --libdir "${HC_OUTPUT}" --plugindir "${HC_PLUGIN_ROOT}"
        ${HC_TRANSLATION_ARGS} --compiler-runtime --no-plugins "${HC_DEPLOY_EXE}" ${HC_PLUGIN_FILES}
)
execute_process(
    COMMAND "${HC_QT_BIN}/windeployqt.exe" ${HC_DEPLOY_ARGS}
    RESULT_VARIABLE HC_RESULT OUTPUT_VARIABLE HC_STDOUT ERROR_VARIABLE HC_STDERR
    TIMEOUT 120
)
get_filename_component(HC_NAME "${HC_DEPLOY_EXE}" NAME_WE)
file(WRITE "${HC_OUTPUT}/${HC_NAME}-windeployqt.log" "${HC_STDOUT}\n${HC_STDERR}")
if(NOT HC_RESULT STREQUAL "0" AND HC_STDERR MATCHES "Unable to find the platform plugin" AND HC_WITH_PLUGINS)
    # windeployqt checks its PE-based platform search even with --no-plugins.
    # Bypass only that module check; still scan all selected plugins for their
    # dependency closure, then copy Gui and plugins from imported Qt targets.
    execute_process(
        COMMAND "${HC_QT_BIN}/windeployqt.exe" --no-gui ${HC_DEPLOY_ARGS}
        RESULT_VARIABLE HC_RESULT OUTPUT_VARIABLE HC_STDOUT ERROR_VARIABLE HC_STDERR
        TIMEOUT 120
    )
    file(APPEND "${HC_OUTPUT}/${HC_NAME}-windeployqt.log" "\nImported-plugin fallback:\n${HC_STDOUT}\n${HC_STDERR}")
    if(HC_RESULT STREQUAL "0")
        file(COPY "${HC_GUI_DLL}" DESTINATION "${HC_OUTPUT}")
    endif()
endif()
if(NOT HC_RESULT STREQUAL "0")
    message(FATAL_ERROR "windeployqt failed for ${HC_NAME}: ${HC_STDOUT}\n${HC_STDERR}")
endif()
foreach(HC_PLUGIN IN LISTS HC_PLUGIN_FILES)
    get_filename_component(HC_PLUGIN_PARENT "${HC_PLUGIN}" DIRECTORY)
    get_filename_component(HC_PLUGIN_TYPE "${HC_PLUGIN_PARENT}" NAME)
    file(COPY "${HC_PLUGIN}" DESTINATION "${HC_PLUGIN_ROOT}/${HC_PLUGIN_TYPE}")
endforeach()
if(HC_WITH_TEST)
    file(COPY_FILE "${HC_QT_BIN}/${HC_TEST_DLL}" "${HC_OUTPUT}/${HC_TEST_DLL}" ONLY_IF_DIFFERENT)
endif()
if(HC_RUNTIME_FILE)
    file(COPY "${HC_RUNTIME_FILE}" DESTINATION "${HC_OUTPUT}")
endif()
# windeployqt always writes WebEngine data under --dir; the embedded qt.conf reads Data=Libs.
if(IS_DIRECTORY "${HC_OUTPUT}/resources")
    file(COPY "${HC_OUTPUT}/resources" DESTINATION "${HC_PLUGIN_ROOT}")
    file(REMOVE_RECURSE "${HC_OUTPUT}/resources")
endif()
# Qt always searches applicationDirPath() too: drop the old flat layout so it cannot shadow Libs/.
foreach(HC_OLD IN ITEMS platforms imageformats iconengines styles generic networkinformation tls sqldrivers printsupport translations qml)
    file(REMOVE_RECURSE "${HC_OUTPUT}/${HC_OLD}")
endforeach()
file(REMOVE "${HC_OUTPUT}/qt.conf")
message(STATUS "Qt runtime deployed: ${HC_NAME} (${HC_CONFIG})")
