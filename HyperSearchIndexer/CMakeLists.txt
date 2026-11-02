qt_add_library(hc_document_search_core STATIC
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/core/textSearchMatcher.cpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/core/textSearchMatcher.hpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/core/bodyProjection.cpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/core/bodyProjection.hpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/core/searchWorkerProtocol.cpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/core/searchWorkerProtocol.hpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/documentSearchWorkerClient.cpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/documentSearchWorkerClient.hpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/documentSearchEngine.cpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/documentSearchEngine.hpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/documentTextCodec.cpp"
)
target_include_directories(hc_document_search_core PUBLIC "${PROJECT_SOURCE_DIR}"
    PRIVATE "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/core")
target_link_libraries(hc_document_search_core PUBLIC Qt6::Core Qt6::Sql)
target_precompile_headers(hc_document_search_core PRIVATE "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/core/stdafx.h")
target_compile_options(hc_document_search_core PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

# Both Everything SDK clients are built from source. No third-party signed DLL
# ships, so a vendor certificate change cannot strand older updaters.
qt_add_executable(hc-search-index searchIndexMain.cpp searchIndexPath.hpp everythingQuery.hpp documentSearchMode.cpp documentSearchMode.hpp "${PROJECT_SOURCE_DIR}/externalLibs/Everything/sdk3/src/Everything3.c"
    "${PROJECT_SOURCE_DIR}/externalLibs/Everything/src/src/Everything.c")
# Keep the deployed filename and root output directory stable for existing callers.
set_target_properties(hc-search-index PROPERTIES
    OUTPUT_NAME "hc-search-index"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    FOLDER "HyperSearchIndexer"
)
target_compile_definitions(hc-search-index PRIVATE EVERYTHING3_LIB EVERYTHINGUSERAPI=)
target_link_libraries(hc-search-index PRIVATE ole32 user32)
target_link_libraries(hc-search-index PRIVATE Qt6::Core hc_document_search_core)
target_precompile_headers(hc-search-index PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/stdafx.h>")
target_compile_options(hc-search-index PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_custom_command(TARGET hc-search-index POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:hc-search-index>/Licenses/EverythingSDK3"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/externalLibs/Everything/sdk3/License.txt" "$<TARGET_FILE_DIR:hc-search-index>/Licenses/EverythingSDK3/License.txt"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:hc-search-index>/Licenses/EverythingSDK2"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/externalLibs/Everything/src/License.txt" "$<TARGET_FILE_DIR:hc-search-index>/Licenses/EverythingSDK2/License.txt"
    VERBATIM
)
hc_enable_qt_deployment(hc-search-index)
hc_add_version_resource(hc-search-index "HyperCommander Search Helper")
