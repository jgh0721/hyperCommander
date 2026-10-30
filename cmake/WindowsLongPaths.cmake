# Run after Qt's per-target finalizers, including targets in the root directory.
# The selected Hybrid CRT kit adds /MANIFEST:NO even with manifest sources.
# Keep the fix local to this repository and retain Qt's trust/compatibility XML.
function(hc_finalize_windows_long_paths)
    if(NOT WIN32 OR NOT MSVC)
        return()
    endif()
    hc_collect_executables("${PROJECT_SOURCE_DIR}" HC_ALL_TARGETS)
    set(HC_MANIFEST_TARGETS)
    set(HC_MANIFEST_PATHS "")
    foreach(HC_TARGET IN LISTS HC_ALL_TARGETS)
        get_target_property(HC_TYPE ${HC_TARGET} TYPE)
        if(NOT HC_TYPE STREQUAL "EXECUTABLE")
            continue()
        endif()
        get_target_property(HC_OPTIONS ${HC_TARGET} LINK_OPTIONS)
        if(HC_OPTIONS)
            list(FILTER HC_OPTIONS EXCLUDE REGEX "^/MANIFEST:NO$")
            set_property(TARGET ${HC_TARGET} PROPERTY LINK_OPTIONS "${HC_OPTIONS}")
        endif()
        target_link_options(${HC_TARGET} PRIVATE "/MANIFEST" "/MANIFEST:EMBED")
        get_target_property(HC_SOURCES ${HC_TARGET} SOURCES)
        set(HC_LONG_PATH_MANIFEST "${PROJECT_SOURCE_DIR}/cmake/updateLongPaths.manifest")
        if(NOT HC_LONG_PATH_MANIFEST IN_LIST HC_SOURCES)
            target_sources(${HC_TARGET} PRIVATE "${HC_LONG_PATH_MANIFEST}")
        endif()
        list(APPEND HC_MANIFEST_TARGETS ${HC_TARGET})
        string(APPEND HC_MANIFEST_PATHS "$<TARGET_FILE:${HC_TARGET}>\n")
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/hc-long-path-targets-$<CONFIG>.txt"
        CONTENT "${HC_MANIFEST_PATHS}")
    add_custom_target(hc_windows_manifest_check
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Test-WindowsLongPathManifest.ps1"
            -ListFile "${CMAKE_BINARY_DIR}/hc-long-path-targets-$<CONFIG>.txt"
        DEPENDS ${HC_MANIFEST_TARGETS} VERBATIM)
    if(TARGET hc_deploy)
        add_dependencies(hc_deploy hc_windows_manifest_check)
    endif()
endfunction()
