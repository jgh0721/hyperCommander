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
