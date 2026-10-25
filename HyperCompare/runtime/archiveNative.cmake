include("${PROJECT_SOURCE_DIR}/cmake/ProductLayout.cmake")
if(NOT TARGET hc_sevenzip_core)
    include("${PROJECT_SOURCE_DIR}/HyperCommanderBuiltInPlugin/SevenZip/SevenZipCore.cmake")
endif()
get_target_property(HC_7ZIP_SOURCE hc_sevenzip_core HC_SOURCE_DIRECTORY)
get_target_property(HC_7ZIP_ARCHIVE hc_sevenzip_core HC_SOURCE_ARCHIVE)
if(NOT TARGET hc_sevenzip_backend)
    qt_add_library(hc_compare_archive_backend STATIC
        "${PROJECT_SOURCE_DIR}/HyperCommanderBuiltInPlugin/SevenZip/sevenZipBackend.cpp")
    target_include_directories(hc_compare_archive_backend PRIVATE "${HC_7ZIP_SOURCE}/CPP" "${PROJECT_SOURCE_DIR}")
    target_link_libraries(hc_compare_archive_backend PRIVATE Qt6::Core oleaut32)
    target_compile_options(hc_compare_archive_backend PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    set(HC_COMPARE_ARCHIVE_BACKEND hc_compare_archive_backend)
else()
    set(HC_COMPARE_ARCHIVE_BACKEND hc_sevenzip_backend)
endif()
target_link_libraries(hc_compare_semantic_worker PRIVATE ${HC_COMPARE_ARCHIVE_BACKEND})
target_include_directories(hc_compare_semantic_worker PRIVATE "${PROJECT_SOURCE_DIR}")
set(HC_COMPARE_7ZIP_PIN "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/hcCompareSevenZipManifest.hpp")
set(HC_COMPARE_7ZIP_RUNTIME_ROOT "" CACHE PATH "Hash-verified isolated snapshot of the shared product 7-Zip runtime")
set(HC_COMPARE_7ZIP_RUNTIME_SHA256 "" CACHE STRING "Approved shared 7-Zip DLL SHA-256")
set(HC_COMPARE_7ZIP_SIGN_ARGS)
if(HC_BUILD_HYPERCOMPARE_ONLY OR HC_PRODUCT_SIGNING)
    list(APPEND HC_COMPARE_7ZIP_SIGN_ARGS "-DCONFIG=$<CONFIG>"
        "-DHC_SIGNING_SCRIPT=${PROJECT_SOURCE_DIR}/tools/update/Sign-ProductFile.ps1"
        "-DHC_SIGNING_CACHE=${PROJECT_SOURCE_DIR}/_build/signing-cache")
endif()
if(HC_COMPARE_7ZIP_RUNTIME_ROOT)
    set(HC_COMPARE_SHARED_PACKAGE "${HC_COMPARE_7ZIP_RUNTIME_ROOT}/${HC_LAYOUT_SevenZipPackage}")
    file(SHA256 "${HC_COMPARE_SHARED_PACKAGE}/7z.dll" HC_COMPARE_SHARED_HASH)
    file(READ "${HC_COMPARE_SHARED_PACKAGE}/source-lock.json" HC_COMPARE_SHARED_SOURCE)
    string(JSON HC_COMPARE_SHARED_SOURCE_HASH GET "${HC_COMPARE_SHARED_SOURCE}" sha256)
    if(NOT HC_COMPARE_SHARED_HASH STREQUAL HC_COMPARE_7ZIP_RUNTIME_SHA256 OR NOT HC_COMPARE_SHARED_SOURCE_HASH STREQUAL "9cbde5099c6deb73691b0579063da5827522ccbbcba3f0020fd04e8c8c16c0d4")
        message(FATAL_ERROR "Shared 7-Zip runtime/source identity mismatch")
    endif()
    file(GENERATE OUTPUT "${HC_COMPARE_7ZIP_PIN}" CONTENT "#pragma once\ninline constexpr char HcBuiltinSevenZipManifest[] = R\"hc7z({\"sha256\":\"${HC_COMPARE_SHARED_HASH}\"})hc7z\";\n" NEWLINE_STYLE CRLF)
    add_custom_target(hc_compare_archive_package
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${HC_COMPARE_SHARED_PACKAGE}" "${CMAKE_BINARY_DIR}/$<CONFIG>/${HC_LAYOUT_SevenZipPackage}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${HC_COMPARE_7ZIP_RUNTIME_ROOT}/Licenses/SevenZip" "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/SevenZip"
        VERBATIM)
elseif(TARGET hc_sevenzip_package)
    get_target_property(HC_COMPARE_BUILTIN_BINARY_DIR hc_builtin_sevenzip BINARY_DIR)
    add_custom_command(OUTPUT "${HC_COMPARE_7ZIP_PIN}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${HC_COMPARE_BUILTIN_BINARY_DIR}/$<CONFIG>/hcSevenZipManifest.hpp" "${HC_COMPARE_7ZIP_PIN}"
        DEPENDS hc_sevenzip_package VERBATIM)
else()
    add_custom_command(OUTPUT "${HC_COMPARE_7ZIP_PIN}"
    COMMAND "${CMAKE_COMMAND}" "-DCORE=$<TARGET_FILE:hc_sevenzip_core>"
        "-DOUTPUT=$<TARGET_FILE_DIR:hc_sevenzip_core>/${HC_LAYOUT_SevenZipPackage}"
        "-DLICENSES=$<TARGET_FILE_DIR:hc_sevenzip_core>/Licenses/SevenZip"
        "-DHEADER=${HC_COMPARE_7ZIP_PIN}" "-DSOURCE=${HC_7ZIP_SOURCE}" "-DARCHIVE=${HC_7ZIP_ARCHIVE}"
        ${HC_COMPARE_7ZIP_SIGN_ARGS} -P "${PROJECT_SOURCE_DIR}/HyperCommanderBuiltInPlugin/SevenZip/GeneratePackage.cmake"
        DEPENDS hc_sevenzip_core "${PROJECT_SOURCE_DIR}/HyperCommanderBuiltInPlugin/SevenZip/GeneratePackage.cmake" VERBATIM)
endif()
if(NOT TARGET hc_compare_archive_package)
    add_custom_target(hc_compare_archive_package DEPENDS "${HC_COMPARE_7ZIP_PIN}")
endif()
add_dependencies(hc_compare_semantic_worker hc_compare_archive_package)
target_include_directories(hc_compare_semantic_worker PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
