set(HC_DOPUS_SDK_DIRECTORY "" CACHE PATH "External extracted Opus SDK directory (contains headers)")
if(NOT HC_DOPUS_SDK_DIRECTORY)
    set(HC_OPUS_ARCHIVE "${CMAKE_BINARY_DIR}/_deps/opus_sdk.zip")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")
    if(NOT EXISTS "${HC_OPUS_ARCHIVE}")
        file(DOWNLOAD "https://cdn2.gpsoft.com.au/files/Misc/opus_sdk.zip" "${HC_OPUS_ARCHIVE}"
            EXPECTED_HASH SHA256=b908d0f9cf34cc93e2e89ea48856bd192cc5d2e45abb42c1d581500f928d59b9 TLS_VERIFY ON)
    endif()
    file(SHA256 "${HC_OPUS_ARCHIVE}" HC_OPUS_HASH)
    if(NOT HC_OPUS_HASH STREQUAL "b908d0f9cf34cc93e2e89ea48856bd192cc5d2e45abb42c1d581500f928d59b9")
        message(FATAL_ERROR "Opus SDK source identity mismatch")
    endif()
    set(HC_DOPUS_SDK_DIRECTORY "${CMAKE_BINARY_DIR}/_deps/opus-sdk" CACHE PATH "External Opus SDK" FORCE)
    file(ARCHIVE_EXTRACT INPUT "${HC_OPUS_ARCHIVE}" DESTINATION "${HC_DOPUS_SDK_DIRECTORY}")
endif()
if(NOT EXISTS "${HC_DOPUS_SDK_DIRECTORY}/headers/viewer plugins.h")
    message(FATAL_ERROR "HC_DOPUS_SDK_DIRECTORY must contain the official SDK headers")
endif()
# A native Directory Opus DLL owns no Qt application and is not a Qt plugin.
qt_add_library(HyperViewer SHARED dopusViewerEntry.cpp viewerHostBridge.cpp viewerHostBridge.hpp HyperViewer.def)
set_target_properties(HyperViewer PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(HyperViewer PRIVATE ${PROJECT_SOURCE_DIR} "${HC_DOPUS_SDK_DIRECTORY}/headers")
target_compile_definitions(HyperViewer PRIVATE UNICODE _UNICODE)
# Qualified RHWP deployment needs room for the dedicated WebEngine process Job.
# Default and diagnostic builds retain the legacy single-process allowance.
if(HC_RHWP_PRODUCT_RUNTIME_DIRECTORY AND HC_RHWP_APPROVED_INVENTORY_SHA256)
    execute_process(COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tools/rhwp/Test-ProductRuntime.ps1"
        -RuntimeRoot "${HC_RHWP_PRODUCT_RUNTIME_DIRECTORY}"
        -ExpectedSha256 "${HC_RHWP_APPROVED_INVENTORY_SHA256}" -AsJson
        RESULT_VARIABLE HC_RHWP_HOST_APPROVAL OUTPUT_QUIET ERROR_VARIABLE HC_RHWP_HOST_ERROR)
    if(NOT HC_RHWP_HOST_APPROVAL EQUAL 0)
        message(FATAL_ERROR "RHWP host process allowance requires a qualified runtime: ${HC_RHWP_HOST_ERROR}")
    endif()
    target_compile_definitions(HyperViewer PRIVATE HC_RHWP_HOST_RUNTIME=1)
endif()
target_compile_options(HyperViewer PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(HyperViewer PRIVATE user32 gdi32 advapi32 bcrypt)
set(HC_HYPERVIEWER_PACKAGE_COMMAND "${CMAKE_COMMAND}"
    "-DVIEWER=$<TARGET_FILE:HyperViewer>"
    "-DRUNTIME=$<TARGET_FILE_DIR:HyperEditor>"
    "-DENGINE_LOCK=${PROJECT_SOURCE_DIR}/HyperEditor/source-lock.json"
    "-DPACKAGE=${HC_LAYOUT_HyperViewerPackage}"
    "-DOUTPUT=$<TARGET_FILE_DIR:HyperEditor>/${HC_LAYOUT_HyperViewerPackage}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/GeneratePackage.cmake")
add_custom_target(hc_hyperviewer_package ALL
    COMMAND ${HC_HYPERVIEWER_PACKAGE_COMMAND}
    DEPENDS HyperViewer HyperEditor hc_document_licenses
    VERBATIM
)
