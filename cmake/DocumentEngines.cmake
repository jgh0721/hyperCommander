include_guard(GLOBAL)
include(FetchContent)
# The two source libraries build with the selected Qt kit's Hybrid CRT.
set(SCI_ROOT "${PROJECT_SOURCE_DIR}/externalLibs/Scintilla")
set(LEX_ROOT "${PROJECT_SOURCE_DIR}/externalLibs/Lexilla")
file(GLOB SCI_QT CONFIGURE_DEPENDS "${SCI_ROOT}/qt/ScintillaEditBase/*.cpp")
set(MV_SCINTILLA_ROOT "${SCI_ROOT}")
include("${CMAKE_CURRENT_LIST_DIR}/ScintillaWrapEnd.cmake")
set(SCI_PATCH_ROOT "${PROJECT_SOURCE_DIR}/HyperEditor/src/thirdparty/scintilla-qt")
set(SCI_PLATQT_UPSTREAM "${SCI_ROOT}/qt/ScintillaEditBase/PlatQt.cpp")
list(FIND SCI_QT "${SCI_PLATQT_UPSTREAM}" SCI_PLATQT_INDEX)
if(SCI_PLATQT_INDEX EQUAL -1)
    message(FATAL_ERROR "Missing upstream Scintilla Qt platform source")
endif()
file(READ "${SCI_PLATQT_UPSTREAM}" SCI_PLATQT_TEXT)
string(REPLACE "\r\n" "\n" SCI_PLATQT_TEXT "${SCI_PLATQT_TEXT}")
# Compare upstream content without the repository's UTF-8 BOM.
string(ASCII 239 187 191 SCI_UTF8_BOM)
string(REPLACE "${SCI_UTF8_BOM}" "" SCI_PLATQT_TEXT "${SCI_PLATQT_TEXT}")
string(SHA256 SCI_PLATQT_HASH "${SCI_PLATQT_TEXT}")
if(NOT SCI_PLATQT_HASH STREQUAL "51392de4d3df3c6611c4074d9aed02652e2ece46bf703cd6efc6e9458b355f6f")
    message(FATAL_ERROR "Scintilla Qt platform changed; rebase the MultiRoot platform patch")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SCI_PLATQT_UPSTREAM}")
list(REMOVE_ITEM SCI_QT "${SCI_PLATQT_UPSTREAM}")
list(APPEND SCI_QT "${SCI_PATCH_ROOT}/PlatQt.cpp")
file(GLOB SCI_CORE CONFIGURE_DEPENDS "${MV_SCINTILLA_CORE_ROOT}/*.cxx")
qt_add_library(hc_scintilla STATIC ${SCI_QT} ${SCI_CORE})
target_include_directories(hc_scintilla BEFORE PUBLIC
    "${PROJECT_SOURCE_DIR}/HyperEditor/src/compat"
    "${SCI_ROOT}/include" "${MV_SCINTILLA_CORE_ROOT}" "${SCI_ROOT}/src" "${SCI_ROOT}/qt/ScintillaEditBase")
target_link_libraries(hc_scintilla PUBLIC Qt6::Widgets)
target_compile_definitions(hc_scintilla PUBLIC EXPORT_IMPORT_API=)
target_compile_definitions(hc_scintilla PRIVATE SCINTILLA_QT=1 MAKING_LIBRARY=1 EXPORT_IMPORT_API= _CRT_SECURE_NO_WARNINGS)
file(GLOB LEX_LIB CONFIGURE_DEPENDS "${LEX_ROOT}/lexlib/*.cxx")
file(GLOB LEXERS CONFIGURE_DEPENDS "${LEX_ROOT}/lexers/*.cxx")
qt_add_library(hc_lexilla STATIC "${LEX_ROOT}/src/Lexilla.cxx" ${LEX_LIB} ${LEXERS})
set_target_properties(hc_lexilla PROPERTIES AUTOMOC OFF)
target_include_directories(hc_lexilla PUBLIC "${LEX_ROOT}/include" "${LEX_ROOT}/lexlib" "${SCI_ROOT}/include")
target_compile_definitions(hc_lexilla PRIVATE _CRT_SECURE_NO_WARNINGS)
foreach(T IN ITEMS hc_scintilla hc_lexilla)
    target_compile_options(${T} PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8;/W0>)
endforeach()
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
