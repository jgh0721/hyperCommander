# Rebuild the distributed Qt plugin from the sources under Licenses/JpegXl.
cmake_minimum_required(VERSION 4.2)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(HyperCommanderJpegXl LANGUAGES CXX)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Gui)
    qt_standard_project_setup(REQUIRES 6.12)
endif()

include(FetchContent)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
foreach(Feature IN ITEMS BENCHMARK DEVTOOLS DOXYGEN EXAMPLES FUZZERS JNI MANPAGES OPENEXR PLUGINS SJPEG TCMALLOC TOOLS TRANSCODE_JPEG VIEWERS)
    set(JPEGXL_ENABLE_${Feature} OFF CACHE BOOL "Unused libjxl build feature")
endforeach()
set(JPEGXL_ENABLE_SKCMS ON CACHE BOOL "Use libjxl's pinned color management")

FetchContent_Declare(hc_jxl
    URL https://codeload.github.com/libjxl/libjxl/tar.gz/refs/tags/v0.13.0
    DOWNLOAD_NAME libjxl-0.13.0.tar.gz
    URL_HASH SHA256=24d18c9af2d1f5a17730b3f8d3f70a5c16e5ac303f5d9d5fe9dc00cf7ca82af2
    SOURCE_SUBDIR hc-source-only)
FetchContent_Declare(hc_jxl_highway
    URL https://codeload.github.com/google/highway/tar.gz/457c891775a7397bdb0376bb1031e6e027af1c48
    URL_HASH SHA256=5124b0501c98d9930dbb065bfa1a5bbbd59ce0f12facb7e1e33aaef01a5f1f1a
    SOURCE_SUBDIR hc-source-only)
FetchContent_Declare(hc_jxl_brotli
    URL https://codeload.github.com/google/brotli/tar.gz/db24a98aed38a16de78cc05ca15cdd8dc2a0d92d
    URL_HASH SHA256=113b9685dfc14dd2f3360d077283266eec2fbdfb0fe3e5600f11be151da0d68e
    SOURCE_SUBDIR hc-source-only)
FetchContent_Declare(hc_jxl_skcms
    URL https://codeload.github.com/google/skcms/tar.gz/96d9171c94b937a1b5f0293de7309ac16311b722
    URL_HASH SHA256=9bb4b5bba0b7c04f6c2bce9ff713d61e23c9a20c4945161ae16290498ad74627
    SOURCE_SUBDIR hc-source-only)
FetchContent_MakeAvailable(hc_jxl hc_jxl_highway hc_jxl_brotli hc_jxl_skcms)
foreach(Dependency IN ITEMS highway brotli skcms)
    file(COPY "${hc_jxl_${Dependency}_SOURCE_DIR}/" DESTINATION "${hc_jxl_SOURCE_DIR}/third_party/${Dependency}")
endforeach()
add_subdirectory("${hc_jxl_SOURCE_DIR}" "${hc_jxl_BINARY_DIR}" EXCLUDE_FROM_ALL)

set(JxlSource "${CMAKE_CURRENT_SOURCE_DIR}/KImageFormats")
if(NOT EXISTS "${JxlSource}/jxl.cpp")
    FetchContent_GetProperties(hc_kimageformats SOURCE_DIR KImageFormatsRoot)
    set(JxlSource "${KImageFormatsRoot}/src/imageformats")
endif()
set(JxlSources "${JxlSource}/jxl.cpp" "${JxlSource}/jxl_p.h" "${JxlSource}/jxl.json"
    "${JxlSource}/microexif.cpp" "${JxlSource}/microexif_p.h" "${JxlSource}/util_p.h")
qt_add_plugin(hc_jpegxl_plugin CLASS_NAME QJpegXLPlugin PLUGIN_TYPE imageformats jxlPreview.cpp jxlPreview.hpp stdafx.h ${JxlSources})
target_compile_definitions(hc_jpegxl_plugin PRIVATE HC_JXL_PREVIEW_EXPORTS)
target_link_libraries(hc_jpegxl_plugin PRIVATE Qt6::Gui jxl jxl_threads jxl_cms)
set_target_properties(hc_jpegxl_plugin PROPERTIES
    OUTPUT_NAME kimg_jxl
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/Libs/imageformats"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/Libs/imageformats")

if(KImageFormatsRoot)
    set(NoticeDir "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/JpegXl")
    add_custom_command(TARGET hc_jpegxl_plugin POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${NoticeDir}/KImageFormats"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_SOURCE_DIR}/jxlPreview.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/jxlPreview.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/stdafx.h"
            "${hc_jxl_SOURCE_DIR}/LICENSE" "${hc_jxl_SOURCE_DIR}/PATENTS"
            "${hc_jxl_BINARY_DIR}/LICENSE.highway" "${hc_jxl_BINARY_DIR}/LICENSE.brotli" "${hc_jxl_BINARY_DIR}/LICENSE.skcms"
            "${KImageFormatsRoot}/LICENSES/BSD-2-Clause.txt" "${KImageFormatsRoot}/LICENSES/LGPL-2.1-or-later.txt"
            "${NoticeDir}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${JxlSources} "${NoticeDir}/KImageFormats"
        VERBATIM)
endif()
