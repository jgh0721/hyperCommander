include_guard(GLOBAL)
include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/TextEditorEngines.cmake)
# PDFium is deliberately a DLL. Only its documented C API crosses the CRT boundary.
FetchContent_Declare(hc_pdfium
    URL "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/8111/pdfium-win-x64.tgz"
    URL_HASH SHA256=3452d11031050ec65e57f180215b57b9e6d465e3fbedafa8a77ad90885c7d23e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(hc_pdfium)
add_library(hc_pdfium_runtime SHARED IMPORTED GLOBAL)
set_target_properties(hc_pdfium_runtime PROPERTIES
    IMPORTED_LOCATION "${hc_pdfium_SOURCE_DIR}/bin/pdfium.dll"
    IMPORTED_IMPLIB "${hc_pdfium_SOURCE_DIR}/lib/pdfium.dll.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${hc_pdfium_SOURCE_DIR}/include")
set(HC_PDFIUM_ROOT "${hc_pdfium_SOURCE_DIR}" CACHE INTERNAL "")
function(hc_deploy_document_engine Target)
    add_custom_command(TARGET ${Target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:hc_pdfium_runtime>" "$<TARGET_FILE_DIR:${Target}>"
        VERBATIM)
endfunction()
set(MV_ROOT "${PROJECT_SOURCE_DIR}/HyperEditor/src")
file(GLOB_RECURSE MV_VIEWS CONFIGURE_DEPENDS "${MV_ROOT}/*.cpp" "${MV_ROOT}/*.h")
list(REMOVE_ITEM MV_VIEWS "${SCI_PATCH_ROOT}/PlatQt.cpp")
qt_add_library(hc_hypereditor STATIC ${MV_VIEWS} "${PROJECT_SOURCE_DIR}/HyperEditor/resources/HyperEditor.qrc"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/portedViewAdapter.cpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/imageResampler.cpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/imageResampler.hpp")
target_include_directories(hc_hypereditor PUBLIC "${MV_ROOT}")
target_link_libraries(hc_hypereditor PUBLIC hc_scintilla hc_lexilla hc_pdfium_runtime Qt6::PrintSupport Qt6::Svg Qt6::Concurrent PRIVATE fmstyle dwmapi runtimeobject)
target_compile_definitions(hc_hypereditor PRIVATE MV_DIRECT_SCINTILLA_HAS_LEXILLA_LEXERS=1 NOMINMAX)
target_compile_options(hc_hypereditor PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

add_custom_target(hc_document_licenses ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/Scintilla"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SCI_PATCH_ROOT}/LICENSE.txt" "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/Scintilla/MultiRoot-LICENSE.txt"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/Scintilla" "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/Lexilla" "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/PDFium" "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/waterColorStyle"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/externalLibs/waterColorStyle/LICENSE" "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/waterColorStyle/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/externalLibs/Scintilla/License.txt" "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/Scintilla/License.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/externalLibs/Lexilla/License.txt" "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/Lexilla/License.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${HC_PDFIUM_ROOT}/LICENSE" "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/PDFium/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${HC_PDFIUM_ROOT}/licenses" "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/PDFium/dependencies"
    VERBATIM)
