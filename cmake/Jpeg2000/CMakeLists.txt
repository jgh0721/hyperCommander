# This recipe also rebuilds the distributed plugin from the sources under Licenses/Jpeg2000.
cmake_minimum_required(VERSION 4.2)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(HyperCommanderJpeg2000 LANGUAGES CXX)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Gui)
    qt_standard_project_setup(REQUIRES 6.12)
endif()

include(FetchContent)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_CODEC OFF)
set(BUILD_TESTING OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
FetchContent_Declare(hc_openjpeg
    URL https://codeload.github.com/uclouvain/openjpeg/tar.gz/refs/tags/v2.5.4
    DOWNLOAD_NAME openjpeg-2.5.4.tar.gz
    URL_HASH SHA256=a695fbe19c0165f295a8531b1e4e855cd94d0875d2f88ec4b61080677e27188a
    EXCLUDE_FROM_ALL)
# OpenJPEG caches legacy output paths. Remove only the defaults it adds so
# unrelated product targets continue to use their original build locations.
set(OpenJpegOutputDefaults)
foreach(OutputPath IN ITEMS EXECUTABLE_OUTPUT_PATH LIBRARY_OUTPUT_PATH)
    if(NOT DEFINED CACHE{${OutputPath}})
        list(APPEND OpenJpegOutputDefaults "${OutputPath}")
    endif()
endforeach()
FetchContent_MakeAvailable(hc_openjpeg)
foreach(OutputPath IN LISTS OpenJpegOutputDefaults)
    unset(${OutputPath} CACHE)
endforeach()

set(Jp2Source "${CMAKE_CURRENT_SOURCE_DIR}/KImageFormats")
if(NOT EXISTS "${Jp2Source}/jp2.cpp")
    FetchContent_Declare(hc_kimageformats
        URL https://codeload.github.com/KDE/kimageformats/tar.gz/refs/tags/v6.22.0
        DOWNLOAD_NAME kimageformats-6.22.0.tar.gz
        URL_HASH SHA256=2f0c6edfdb97ea71287353a8eb272525fae141e493d98b4ecc3e6cbea073e26e
        # Only the plugin sources are needed. Skip the KDE project's CMake files.
        SOURCE_SUBDIR src/imageformats/jp2
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(hc_kimageformats)
    set(KImageFormats "${hc_kimageformats_SOURCE_DIR}/src/imageformats")
    set(Jp2Source "${CMAKE_CURRENT_BINARY_DIR}/KImageFormats")
    foreach(Source IN ITEMS jp2_p.h jp2.json scanlineconverter.cpp scanlineconverter_p.h util_p.h)
        configure_file("${KImageFormats}/${Source}" "${Jp2Source}/${Source}" COPYONLY)
    endforeach()
    include(ScaledDecode.cmake)
    hc_enable_jp2_scaled_decode("${KImageFormats}/jp2.cpp" "${Jp2Source}/jp2.cpp")
endif()

set(Jp2Sources "${Jp2Source}/jp2.cpp" "${Jp2Source}/jp2_p.h" "${Jp2Source}/jp2.json"
    "${Jp2Source}/scanlineconverter.cpp" "${Jp2Source}/scanlineconverter_p.h" "${Jp2Source}/util_p.h")
qt_add_plugin(hc_jpeg2000_plugin CLASS_NAME JP2Plugin PLUGIN_TYPE imageformats ${Jp2Sources})
target_include_directories(hc_jpeg2000_plugin PRIVATE "${hc_openjpeg_SOURCE_DIR}/src/lib/openjp2"
    "${hc_openjpeg_BINARY_DIR}/src/lib/openjp2")
target_link_libraries(hc_jpeg2000_plugin PRIVATE Qt6::Gui openjp2)
set_target_properties(hc_jpeg2000_plugin PROPERTIES
    OUTPUT_NAME kimg_jp2
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/Libs/imageformats"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/Libs/imageformats")

if(DEFINED hc_kimageformats_SOURCE_DIR)
    set(NoticeDir "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/Jpeg2000")
    add_custom_command(TARGET hc_jpeg2000_plugin POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${NoticeDir}/KImageFormats"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
            "${hc_openjpeg_SOURCE_DIR}/LICENSE"
            "${hc_kimageformats_SOURCE_DIR}/LICENSES/LGPL-2.0-or-later.txt"
            "${hc_kimageformats_SOURCE_DIR}/LICENSES/LGPL-2.1-or-later.txt"
            "${NoticeDir}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${Jp2Sources} "${NoticeDir}/KImageFormats"
        VERBATIM)
endif()
