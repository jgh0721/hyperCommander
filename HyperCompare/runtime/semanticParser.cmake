include_guard(GLOBAL)
enable_language(C)
include(FetchContent)
# Fixed source archives and digests; upstream build scripts are not executed.
FetchContent_Declare(hc_tree_sitter
    URL https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.27.1.tar.gz
    URL_HASH SHA256=982cd3d4d9eb7be18c243240a622423fd2e4ecb4bec2cd98832c2f3fa1f0f333
    SOURCE_SUBDIR hc-unused)
FetchContent_Declare(hc_tree_sitter_c
    URL https://github.com/tree-sitter/tree-sitter-c/releases/download/v0.24.2/tree-sitter-c.tar.gz
    URL_HASH SHA256=f3a2cdfbca39c79f60baf2ef62b42084c609782c76485de457ba36ff65d51baf
    SOURCE_SUBDIR hc-unused)
FetchContent_Declare(hc_tree_sitter_cpp
    URL https://github.com/tree-sitter/tree-sitter-cpp/archive/refs/tags/v0.23.4.tar.gz
    URL_HASH SHA256=7a2c55afe3028f4105f25762ea58cc16537d1f5a1dcd9cca90410b3cd5d46051
    SOURCE_SUBDIR hc-unused)
FetchContent_Declare(hc_tree_sitter_python
    URL https://github.com/tree-sitter/tree-sitter-python/releases/download/v0.25.0/tree-sitter-python.tar.gz
    URL_HASH SHA256=7bce887eb2f33e94bf74a69645cf5138d4096720e54fd3269a6124c06b93c584
    SOURCE_SUBDIR hc-unused)
FetchContent_MakeAvailable(hc_tree_sitter hc_tree_sitter_c hc_tree_sitter_cpp hc_tree_sitter_python)
qt_add_library(hc_compare_parser STATIC
    "${hc_tree_sitter_SOURCE_DIR}/lib/src/lib.c"
    "${hc_tree_sitter_c_SOURCE_DIR}/src/parser.c"
    "${hc_tree_sitter_cpp_SOURCE_DIR}/src/parser.c"
    "${hc_tree_sitter_cpp_SOURCE_DIR}/src/scanner.c"
    "${hc_tree_sitter_python_SOURCE_DIR}/src/parser.c"
    "${hc_tree_sitter_python_SOURCE_DIR}/src/scanner.c")
set_target_properties(hc_compare_parser PROPERTIES AUTOMOC OFF C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(hc_compare_parser PUBLIC "${hc_tree_sitter_SOURCE_DIR}/lib/include"
    PRIVATE "${hc_tree_sitter_SOURCE_DIR}/lib/src")
target_compile_definitions(hc_compare_parser PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(hc_compare_parser PRIVATE $<$<C_COMPILER_ID:MSVC>:/W0;/utf-8>)
foreach(name IN ITEMS hc_tree_sitter hc_tree_sitter_c hc_tree_sitter_cpp hc_tree_sitter_python)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/${name}.txt"
        INPUT "${${name}_SOURCE_DIR}/LICENSE")
endforeach()
qt_add_library(hc_compare_semantic_worker STATIC runtime/semanticParser.cpp runtime/workerMain.cpp runtime/pngWorker.cpp)
target_link_libraries(hc_compare_semantic_worker PRIVATE hc_compare_parser Qt6::Core Qt6::Gui)
target_include_directories(hc_compare_semantic_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(hc_compare_semantic_worker PRIVATE NOMINMAX UNICODE _UNICODE)
target_compile_options(hc_compare_semantic_worker PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_precompile_headers(hc_compare_semantic_worker PRIVATE stdafx.h)
