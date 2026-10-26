include_guard(GLOBAL)
set(HC_IMAGE_CODEC_ROOT "${PROJECT_SOURCE_DIR}/_build/image-codecs" CACHE PATH "Pinned image codec build directory")
set(HC_IMAGE_CODEC_PREFIX "${HC_IMAGE_CODEC_ROOT}/runtime")
if(NOT EXISTS "${HC_IMAGE_CODEC_ROOT}/complete.txt")
    message(FATAL_ERROR "Build the pinned image codecs first: powershell -ExecutionPolicy Bypass -File tools/Build-ImageCodecs.ps1 -BuildRoot ${HC_IMAGE_CODEC_ROOT}")
endif()
foreach(CODEC IN ITEMS heif avif)
    add_library(hc_codec_${CODEC} SHARED IMPORTED GLOBAL)
    set_target_properties(hc_codec_${CODEC} PROPERTIES
        IMPORTED_IMPLIB "${HC_IMAGE_CODEC_PREFIX}/lib/${CODEC}.lib"
        IMPORTED_LOCATION "${HC_IMAGE_CODEC_PREFIX}/bin/${CODEC}.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${HC_IMAGE_CODEC_PREFIX}/include")
endforeach()
qt_add_plugin(hc_image_codecs_plugin CLASS_NAME ModernImagePlugin
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/modernImagePlugin.cpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/modernImagePlugin.json")
set_target_properties(hc_image_codecs_plugin PROPERTIES
    QT_PLUGIN_TYPE imageformats
    OUTPUT_NAME qhcmodern
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/Libs/imageformats"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/Libs/imageformats")
target_link_libraries(hc_image_codecs_plugin PRIVATE Qt6::Gui hc_codec_heif hc_codec_avif)
target_compile_options(hc_image_codecs_plugin PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

function(hc_enable_preview_webp)
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    foreach(OPTION IN ITEMS ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS WEBP_JS FUZZTEST)
        set(WEBP_BUILD_${OPTION} OFF CACHE BOOL "Unused preview codec tool" FORCE)
    endforeach()
    set(WEBP_LINK_STATIC ON CACHE BOOL "Static preview decoder" FORCE)
    FetchContent_Declare(hc_preview_webp
        URL https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz
        URL_HASH SHA256=e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(hc_preview_webp)
    target_link_libraries(hc_image_codecs_plugin PRIVATE webpdecoder)
    add_custom_command(TARGET hc_image_codecs_plugin POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/libwebp"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${hc_preview_webp_SOURCE_DIR}/COPYING" "${hc_preview_webp_SOURCE_DIR}/PATENTS" "${hc_preview_webp_SOURCE_DIR}/AUTHORS"
            "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/libwebp"
        VERBATIM)
endfunction()
hc_enable_preview_webp()
