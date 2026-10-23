include_guard(GLOBAL)
include(FetchContent)
FetchContent_Declare(hc_miniz
    URL https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz
    URL_HASH SHA256=98468f8924934b723276680f85238b6c78bf1f8b49b4459cc9b7214a20e2e9fb
    SOURCE_SUBDIR hc-unused)
FetchContent_MakeAvailable(hc_miniz)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/miniz-config/miniz_export.h" CONTENT "#pragma once\n#define MINIZ_EXPORT\n" NEWLINE_STYLE CRLF)
qt_add_library(hc_compare_zip STATIC "${hc_miniz_SOURCE_DIR}/miniz.c" "${hc_miniz_SOURCE_DIR}/miniz_tinfl.c" "${hc_miniz_SOURCE_DIR}/miniz_zip.c")
set_target_properties(hc_compare_zip PROPERTIES AUTOMOC OFF C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(hc_compare_zip PUBLIC "${hc_miniz_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/miniz-config")
target_compile_definitions(hc_compare_zip PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_DEFLATE_APIS MINIZ_NO_ZLIB_APIS MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_compile_options(hc_compare_zip PRIVATE $<$<C_COMPILER_ID:MSVC>:/W0;/utf-8>)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/miniz.txt" INPUT "${hc_miniz_SOURCE_DIR}/LICENSE")
target_sources(hc_compare_semantic_worker PRIVATE runtime/zipArchive.cpp runtime/zipArchive.hpp runtime/archiveWorker.cpp)
target_link_libraries(hc_compare_semantic_worker PRIVATE hc_compare_zip)
