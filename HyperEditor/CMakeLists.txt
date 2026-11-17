find_package(Qt6 6.12.0 REQUIRED COMPONENTS ZlibPrivate)
include(${PROJECT_SOURCE_DIR}/cmake/DocumentEngines.cmake)
qt_add_library(hc_document_core STATIC
    ../commonLibs/document/documentFormatSupport.hpp
    ../commonLibs/document/documentHandlerRegistry.hpp ../commonLibs/document/documentHandlerRegistry.cpp
    ../commonLibs/document/documentHandlerResolver.hpp ../commonLibs/document/documentHandlerResolver.cpp
    ../commonLibs/document/documentTypes.hpp
    ../commonLibs/document/documentSession.hpp ../commonLibs/document/documentSession.cpp
    ../commonLibs/document/documentAsyncSession.cpp
    ../commonLibs/document/documentWorkspace.hpp ../commonLibs/document/documentWorkspace.cpp
    ../commonLibs/document/documentRecovery.hpp
    ../commonLibs/document/documentHostProtocol.hpp
    ../commonLibs/document/documentSaveCoordinator.cpp
    ../commonLibs/document/documentSaveCoordinator.hpp
)
set_target_properties(hc_document_core PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(hc_document_core PUBLIC ${PROJECT_SOURCE_DIR})
target_compile_options(hc_document_core PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
qt_add_library(hc_document_broker STATIC
    ../UniqueLibs/document/documentHostBroker.cpp
    ../UniqueLibs/document/documentHostBroker.hpp
    ../UniqueLibs/document/archiveDraftDirectory.hpp
    ../UniqueLibs/document/hwpInputInspection.cpp ../UniqueLibs/document/hwpInputInspection.hpp
    ../UniqueLibs/document/hwpPrintSnapshot.cpp ../UniqueLibs/document/hwpPrintSnapshot.hpp
    ../UniqueLibs/document/hwpPrintBridge.cpp ../UniqueLibs/document/hwpPrintBridge.hpp
    ../UniqueLibs/document/hwpAsyncSave.cpp ../UniqueLibs/document/hwpAsyncSave.hpp
    ../UniqueLibs/document/hwpEditBridge.cpp ../UniqueLibs/document/hwpEditBridge.hpp
    ../UniqueLibs/document/hwpExportTransfer.cpp ../UniqueLibs/document/hwpExportTransfer.hpp
    ../UniqueLibs/document/hwpInputSender.cpp ../UniqueLibs/document/hwpInputSender.hpp
    ../UniqueLibs/document/documentWebProcess.cpp ../UniqueLibs/document/documentWebProcess.hpp
    ../UniqueLibs/document/hwpRuntimeSha256.cpp ../UniqueLibs/document/hwpRuntimeSha256.hpp
    ../UniqueLibs/nativeWritableStorage.cpp
    ../UniqueLibs/storageProviders.cpp
)
target_include_directories(hc_document_broker PRIVATE
    ${PROJECT_SOURCE_DIR}/HyperCommander ${PROJECT_SOURCE_DIR}/commonLibs
    ${PROJECT_SOURCE_DIR}/commonLibs/fmt-10.2.0/include)
# Native authority is configured independently of the adjacent runtime inventory.
include("${PROJECT_SOURCE_DIR}/cmake/EpubProductPolicy.cmake")
configure_file(epubProductPolicy.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/epubProductPolicy.hpp" @ONLY NEWLINE_STYLE CRLF)
target_include_directories(hc_document_broker PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(hc_document_broker PUBLIC hc_document_core Qt6::Core PRIVATE Qt6::ZlibPrivate Qt6::Widgets advapi32 shell32 ole32 bcrypt)
target_compile_options(hc_document_broker PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
find_package(Qt6 6.12.0 REQUIRED COMPONENTS ZlibPrivate)
qt_add_library(hc_epub_package STATIC
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubInputSender.cpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubInputSender.hpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubPackage.hpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubPackage.cpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubEditModel.cpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubEditModel.hpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubEditStructure.cpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubSourceMap.cpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubSourceMap.hpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubSourceLines.cpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubSourceLines.hpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubDraftStore.cpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubDraftStore.hpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubArchive.hpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubArchive.cpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubTemplate.hpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/epubTemplate.cpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/stdafx.h"
)
target_precompile_headers(hc_epub_package PRIVATE "${PROJECT_SOURCE_DIR}/commonLibs/document/epub/stdafx.h")
target_include_directories(hc_epub_package PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(hc_epub_package PUBLIC Qt6::Core PRIVATE Qt6::Gui Qt6::Xml Qt6::ZlibPrivate)
target_compile_options(hc_epub_package PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
qt_add_library(hc_epub_host STATIC
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/epubProcessView.cpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/epubProcessView.hpp")
target_precompile_headers(hc_epub_host PRIVATE "${PROJECT_SOURCE_DIR}/tools/epub/render/stdafx.h")
target_include_directories(hc_epub_host PRIVATE "${PROJECT_SOURCE_DIR}/tools/epub/render")
target_link_libraries(hc_epub_host PUBLIC hc_epub_package hc_document_broker Qt6::Widgets PRIVATE Fm::style)
target_compile_options(hc_epub_host PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

qt_add_library(hc_document_sdi STATIC
    ../UniqueLibs/document/documentFocusRestore.hpp
    ../UniqueLibs/document/hwpHostSession.cpp ../UniqueLibs/document/hwpHostSession.hpp
    ../UniqueLibs/document/hwpOpenWindow.cpp ../UniqueLibs/document/hwpOpenWindow.hpp
    ../UniqueLibs/nativeMaterializer.cpp ../commonLibs/operations/jobScheduler.cpp
    ../UniqueLibs/document/hwpSdiWindow.cpp ../UniqueLibs/document/hwpSdiWindow.hpp
    ../UniqueLibs/document/hwpProcessBackend.cpp ../UniqueLibs/document/hwpProcessBackend.hpp
)
target_link_libraries(hc_document_sdi PUBLIC hc_document_core Qt6::Widgets PRIVATE hc_document_broker Qt6::Concurrent Qt6::PrintSupport Fm::widgets)
target_compile_options(hc_document_sdi PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

# 자동완성 목록 · 상세 창 · 일치 규칙(reST 편집기와 SQLite 쿼리 편집기 공용, SQLite 설계 261113 5.2). reST 에서 옮겼다.
file(GLOB HC_DOCUMENT_COMPLETION_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/completion/*.cpp" "${PROJECT_SOURCE_DIR}/UniqueLibs/document/completion/*.hpp")
qt_add_library(hc_document_completion STATIC ${HC_DOCUMENT_COMPLETION_SOURCES})
target_include_directories(hc_document_completion PUBLIC ${PROJECT_SOURCE_DIR} PRIVATE
    ${PROJECT_SOURCE_DIR}/HyperCommander
    ${PROJECT_SOURCE_DIR}/commonLibs
    ${PROJECT_SOURCE_DIR}/commonLibs/fmt-10.2.0/include)
target_link_libraries(hc_document_completion PUBLIC hc_hypereditor Qt6::Widgets)
target_precompile_headers(hc_document_completion PRIVATE "${PROJECT_SOURCE_DIR}/HyperCommander/stdafx.h")
target_compile_options(hc_document_completion PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

# reStructuredText services without document-workspace widgets: Sphinx project resolution, the Python
# environment (uv), Esbonio, preview builds, outline, diagnostics and completion. Ported from MultiRoot reST Editor.
file(GLOB HC_RST_CORE_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/rst/*.cpp" "${PROJECT_SOURCE_DIR}/UniqueLibs/document/rst/*.hpp")
qt_add_library(hc_rst_core STATIC ${HC_RST_CORE_SOURCES})
# The environment definition and the preview builder are extracted into the runtime folder when it is configured.
qt_add_resources(hc_rst_core "hc_rst_runtime" PREFIX "/rst/runtime" BASE "${PROJECT_SOURCE_DIR}/tools/rst/runtime" FILES
    "${PROJECT_SOURCE_DIR}/tools/rst/runtime/pyproject.toml"
    "${PROJECT_SOURCE_DIR}/tools/rst/runtime/uv.lock"
    "${PROJECT_SOURCE_DIR}/tools/rst/runtime/mrr_sphinx_preview_build.py")
target_include_directories(hc_rst_core PUBLIC "${PROJECT_SOURCE_DIR}/UniqueLibs/document/rst" PRIVATE
    ${PROJECT_SOURCE_DIR}/HyperCommander
    ${PROJECT_SOURCE_DIR}/commonLibs
    ${PROJECT_SOURCE_DIR}/commonLibs/fmt-10.2.0/include)
target_link_libraries(hc_rst_core PUBLIC hc_document_core hc_hypereditor hc_document_completion Qt6::Widgets Qt6::Network Qt6::Concurrent PRIVATE Qt6::ZlibPrivate)
target_precompile_headers(hc_rst_core PRIVATE "${PROJECT_SOURCE_DIR}/HyperCommander/stdafx.h")
target_compile_options(hc_rst_core PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

# 탐색기 · 워크스페이스 모드의 화면 없는 핵심(MultiRoot reST Editor 에서 옮김): 탐색기 모델 · 필터 · 바깥 묶음,
# 필터 걷기, 이름 검사, 워크스페이스 상태 파일 · MRE 가져오기 · 상위 찾기 · 창 등록부. 시험이 문서 위젯 없이 빌드된다.
file(GLOB HC_DOCUMENT_EXPLORER_CORE_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/explorer/*.cpp" "${PROJECT_SOURCE_DIR}/UniqueLibs/document/explorer/*.hpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/workspace/*.cpp" "${PROJECT_SOURCE_DIR}/UniqueLibs/document/workspace/*.hpp")
qt_add_library(hc_document_explorer_core STATIC ${HC_DOCUMENT_EXPLORER_CORE_SOURCES})
target_include_directories(hc_document_explorer_core PUBLIC ${PROJECT_SOURCE_DIR} PRIVATE
    ${PROJECT_SOURCE_DIR}/HyperCommander
    ${PROJECT_SOURCE_DIR}/commonLibs
    ${PROJECT_SOURCE_DIR}/commonLibs/fmt-10.2.0/include)
target_link_libraries(hc_document_explorer_core PUBLIC Qt6::Widgets Qt6::Concurrent PRIVATE netapi32)
target_precompile_headers(hc_document_explorer_core PRIVATE "${PROJECT_SOURCE_DIR}/HyperCommander/stdafx.h")
target_compile_options(hc_document_explorer_core PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

# SQLite 문서(설계 261113 8.4 · 9절). 제품은 Qt 키트의 sqlite3.dll(SEE 포함, SQLITE_DLL=1) 하나를 C API 로 쓴다.
# 그 DLL 은 exe 옆이 아니라 Libs\sqldrivers 에 있으므로 지연 로드로 링크하고, hc::sqlite::Runtime::Load() 가 그 경로로
# 먼저 불러 둔다(문서 검색의 QSQLITE 도 같은 함수). SEE 활성화 문구는 로컬 전용 UniqueLibs/sqlite_see_activate.hpp
# (.gitignore)에만 있고, 없으면 SEE 없이 빌드된다.
qt_add_library(hc_sqlite_runtime STATIC
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/sqlite/sqliteRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/sqlite/sqliteRuntime.hpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/sqlite/sqliteApi.hpp")
# QtCore 만 쓰므로(문서 검색 코어 · hc-search-index 도 링크한다) 검색 코어의 가벼운 stdafx.h 를 쓴다.
target_include_directories(hc_sqlite_runtime PUBLIC ${PROJECT_SOURCE_DIR} PRIVATE
    ${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/core)
target_link_libraries(hc_sqlite_runtime PUBLIC Qt6::Core "$<TARGET_LINKER_FILE_DIR:Qt6::Core>/sqlite3$<$<CONFIG:Debug>:d>.lib" delayimp)
# hc-search-index 처럼 SQLite C API 를 부르지 않는 대상은 지연 로드할 것이 없다(LNK4199).
target_link_options(hc_sqlite_runtime INTERFACE "/DELAYLOAD:sqlite3$<$<CONFIG:Debug>:d>.dll" "/IGNORE:4199")
target_precompile_headers(hc_sqlite_runtime PRIVATE "${PROJECT_SOURCE_DIR}/UniqueLibs/document/search/core/stdafx.h")
target_compile_options(hc_sqlite_runtime PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

file(GLOB HC_SQLITE_CORE_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/sqlite/*.cpp" "${PROJECT_SOURCE_DIR}/UniqueLibs/document/sqlite/*.hpp")
list(FILTER HC_SQLITE_CORE_SOURCES EXCLUDE REGEX "/sqlite(Runtime|Api)\\.(cpp|hpp)$")
qt_add_library(hc_sqlite_core STATIC ${HC_SQLITE_CORE_SOURCES})
target_include_directories(hc_sqlite_core PUBLIC ${PROJECT_SOURCE_DIR} PRIVATE
    ${PROJECT_SOURCE_DIR}/HyperCommander
    ${PROJECT_SOURCE_DIR}/commonLibs
    ${PROJECT_SOURCE_DIR}/commonLibs/fmt-10.2.0/include)
# crypt32: 키 저장소의 DPAPI(CryptProtectData, 설계 8.3). credui · secur32: 키 보기 전 Windows 로그인 확인.
target_link_libraries(hc_sqlite_core PUBLIC hc_sqlite_runtime hc_hypereditor hc_document_completion Qt6::Widgets Qt6::Svg Qt6::Concurrent fmstyle Fm::widgets
    PRIVATE crypt32 credui secur32 advapi32 ole32)
target_precompile_headers(hc_sqlite_core PRIVATE "${PROJECT_SOURCE_DIR}/HyperCommander/stdafx.h")
target_compile_options(hc_sqlite_core PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

add_subdirectory("${PROJECT_SOURCE_DIR}/tools/markdown" "${CMAKE_BINARY_DIR}/markdown")
add_subdirectory("${PROJECT_SOURCE_DIR}/tools/rst" "${CMAKE_BINARY_DIR}/rst-preview")
qt_add_library(hc_document_widgets STATIC
    ../UniqueLibs/ProductDefaults_Edit.hpp
    ../UniqueLibs/document/markdownDocumentView.cpp ../UniqueLibs/document/markdownDocumentView.hpp
    ../UniqueLibs/document/markdownProcessView.cpp ../UniqueLibs/document/markdownProcessView.hpp
    ../UniqueLibs/document/rstDocumentView.cpp ../UniqueLibs/document/rstDocumentView.hpp
    ../UniqueLibs/document/rstPreviewProcess.cpp ../UniqueLibs/document/rstPreviewProcess.hpp
    ../UniqueLibs/document/rstProjectDialogs.cpp ../UniqueLibs/document/rstProjectDialogs.hpp
    ../tools/epub/render/readingSettings.cpp ../tools/epub/render/readingSettings.hpp
    ../tools/epub/render/readingStore.cpp ../tools/epub/render/readingAssets.cpp
    ../tools/epub/render/readingProfiles.cpp ../tools/epub/render/readingProfiles.hpp
    ../tools/epub/render/readingProfilePanel.cpp
    ../UniqueLibs/document/epubSourceEditor.cpp ../UniqueLibs/document/epubSourceEditor.hpp
    ../UniqueLibs/document/epubSourceRecovery.cpp
    ../UniqueLibs/document/epubSourcePreview.cpp
    ../UniqueLibs/document/epubSourceScroll.cpp ../UniqueLibs/document/epubLineShifts.hpp
    ../UniqueLibs/document/epubSourceValidation.cpp
    ../UniqueLibs/document/epubSourceStructure.cpp
    ../UniqueLibs/document/epubSourceNavigation.cpp
    ../UniqueLibs/document/epubSourceEditing.cpp
    ../UniqueLibs/document/epubSourceProperties.cpp
    ../UniqueLibs/document/epubSourceLayout.cpp
    ../UniqueLibs/document/epubSourceTabs.cpp ../UniqueLibs/document/epubSourceTabs.hpp
    ../UniqueLibs/document/epubSourceMenus.cpp
    ../UniqueLibs/document/epubReaderMenus.cpp ../UniqueLibs/document/epubReaderMenus.hpp
    ../UniqueLibs/document/epubSourceTools.cpp
    ../UniqueLibs/document/epubResourceTree.cpp ../UniqueLibs/document/epubResourceTree.hpp
    ../UniqueLibs/document/imageBook.cpp ../UniqueLibs/document/imageBook.hpp
    ../UniqueLibs/document/imageFramePlayer.cpp ../UniqueLibs/document/imageFramePlayer.hpp
    ../UniqueLibs/document/imageReadingView.cpp ../UniqueLibs/document/imageReadingView.hpp
    ../UniqueLibs/document/imageReadingNavigator.cpp ../UniqueLibs/document/imageReadingNavigator.hpp
    ../UniqueLibs/document/imageFullscreen.cpp ../UniqueLibs/document/imageFullscreen.hpp
    ../UniqueLibs/document/documentTextCodec.hpp
    ../UniqueLibs/document/textDocumentMetrics.cpp ../UniqueLibs/document/textDocumentMetrics.hpp
    ../UniqueLibs/document/search/documentSearchPanel.cpp ../UniqueLibs/document/search/documentSearchPanel.hpp
    ../UniqueLibs/document/documentViewPane.cpp ../UniqueLibs/document/documentViewPane.hpp
    ../UniqueLibs/document/documentViewKind.hpp
    ../UniqueLibs/document/documentViewMenus.cpp
    ../UniqueLibs/document/documentStatusPickers.cpp
    ../UniqueLibs/document/documentStatusPopup.cpp ../UniqueLibs/document/documentStatusPopup.hpp
    ../UniqueLibs/document/documentProgressOverlay.cpp ../UniqueLibs/document/documentProgressOverlay.hpp
    ../UniqueLibs/document/documentStallTrace.hpp
    ../UniqueLibs/document/recentDocuments.cpp ../UniqueLibs/document/recentDocuments.hpp
    ../UniqueLibs/document/recentDocumentsDialog.cpp ../UniqueLibs/document/recentDocumentsDialog.hpp
    ../UniqueLibs/document/documentStartPage.cpp ../UniqueLibs/document/documentStartPage.hpp
    ../UniqueLibs/document/newDocumentChooser.cpp ../UniqueLibs/document/newDocumentChooser.hpp
    ../UniqueLibs/document/newEpubDialog.cpp ../UniqueLibs/document/newEpubDialog.hpp
    ../UniqueLibs/document/newSqliteDialog.cpp ../UniqueLibs/document/newSqliteDialog.hpp
    ../UniqueLibs/document/epubRestoreConflictDialog.cpp ../UniqueLibs/document/epubRestoreConflictDialog.hpp
    ../UniqueLibs/document/documentRecentMenu.cpp ../UniqueLibs/document/documentRecentMenu.hpp
    ../UniqueLibs/document/textContextMenu.cpp ../UniqueLibs/document/textContextMenu.hpp
    ../UniqueLibs/document/viewUiContribution.hpp
    ../UniqueLibs/document/workspaceChromeController.cpp ../UniqueLibs/document/workspaceChromeController.hpp
    ../UniqueLibs/document/documentThemeAdapter.cpp ../UniqueLibs/document/documentThemeAdapter.hpp
    ../UniqueLibs/document/documentRecoveryStore.cpp ../UniqueLibs/document/documentRecoveryStore.hpp
    ../UniqueLibs/document/documentRecoveryMaintenance.cpp ../UniqueLibs/document/documentRecoveryMaintenance.hpp
    ../UniqueLibs/document/recoveryPromptDialog.cpp ../UniqueLibs/document/recoveryPromptDialog.hpp
    ../UniqueLibs/document/recoveryManagerDialog.cpp ../UniqueLibs/document/recoveryManagerDialog.hpp
    ../UniqueLibs/document/documentToolPanel.cpp
    ../UniqueLibs/document/documentToolPanel.hpp
    ../UniqueLibs/document/explorerFileOps.cpp
    ../UniqueLibs/document/explorerFileOps.hpp
    ../UniqueLibs/document/explorerPanel.cpp
    ../UniqueLibs/document/explorerPanel.hpp
    ../UniqueLibs/document/documentWorkspaceWidget.cpp
    ../UniqueLibs/document/documentWorkspaceWidget.hpp
    ../UniqueLibs/document/documentSettingsDialog.cpp
    ../UniqueLibs/document/documentSettingsDialog.hpp
    ../UniqueLibs/document/documentShortcutRegistry.hpp
    ../UniqueLibs/document/documentShortcutRouter.hpp
    ../UniqueLibs/document/documentStorageAdapter.cpp
    ../UniqueLibs/document/documentStorageAdapter.hpp
)
target_include_directories(hc_document_widgets PRIVATE
    ${PROJECT_SOURCE_DIR}/HyperCommander
    ${PROJECT_SOURCE_DIR}/commonLibs
    ${PROJECT_SOURCE_DIR}/commonLibs/fmt-10.2.0/include
)
add_dependencies(hc_document_widgets hc-markdown-render hc-rst-preview)
target_link_libraries(hc_document_widgets PRIVATE Qt6::Network hc_document_search_core)
# Preview helpers that an older updater delivered under Libs/RootFiles are copied beside the program
# (updatePromotion) when a preview starts before HyperCommander did it.
target_link_libraries(hc_document_widgets PRIVATE hc_update_core)
target_link_libraries(hc_document_widgets PUBLIC hc_rst_core)
target_link_libraries(hc_document_widgets PUBLIC hc_document_core hc_document_broker hc_document_sdi hc_hypereditor Qt6::Widgets fmstyle PRIVATE Qt6::Concurrent advapi32 shell32 ole32)
target_link_libraries(hc_document_widgets PRIVATE Qt6::ZlibPrivate hc_epub_host hc_builtin_sevenzip hc_image_archive_transport hc_passwords Fm::widgets)
# Document area and tool windows dock with fmdock 2 (waterColorStyle).
target_link_libraries(hc_document_widgets PUBLIC Fm::dock)
# 탐색기 도구 창 · 워크스페이스 모드.
target_link_libraries(hc_document_widgets PUBLIC hc_document_explorer_core)
# SQLite 데이터베이스 작업 창(설계 261113).
target_link_libraries(hc_document_widgets PUBLIC hc_sqlite_core)
target_compile_options(hc_document_widgets PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
set(HC_RHWP_PRODUCT_RUNTIME_DIRECTORY "" CACHE PATH "Qualified shared RHWP runtime to include in product installation")
if(HC_RHWP_PRODUCT_RUNTIME_DIRECTORY AND NOT HC_RHWP_APPROVED_INVENTORY_SHA256)
    message(FATAL_ERROR "Product RHWP runtime requires the native approved inventory pin")
endif()
set(HC_RHWP_APPROVED_INVENTORY_SHA256 "" CACHE STRING "Native product runtime inventory pin; empty denies RHWP open")
if(NOT HC_RHWP_APPROVED_INVENTORY_SHA256 STREQUAL "")
    string(LENGTH "${HC_RHWP_APPROVED_INVENTORY_SHA256}" HC_RHWP_INVENTORY_PIN_LENGTH)
    if(NOT HC_RHWP_INVENTORY_PIN_LENGTH EQUAL 64 OR NOT HC_RHWP_APPROVED_INVENTORY_SHA256 MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "RHWP runtime inventory policy requires 64 lowercase hex digits")
    endif()
endif()
configure_file(hwpProductPolicy.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/hwpProductPolicy.hpp" @ONLY NEWLINE_STYLE CRLF)
qt_add_executable(HyperEditor WIN32 main.cpp runtimeVerification.cpp runtimeVerification.hpp)
include("${PROJECT_SOURCE_DIR}/cmake/BundledFonts.cmake")
hc_add_bundled_fonts(HyperEditor)
target_precompile_headers(HyperEditor PRIVATE stdafx.h)
target_link_libraries(HyperEditor PRIVATE hc_startup_fonts)
hc_add_version_resource(HyperEditor "HyperEditor" ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app/hypereditor.ico")
target_include_directories(HyperEditor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(HyperEditor PRIVATE hc_document_widgets hc_document_sdi fmstyle Fm::widgets Qt6::Network)
target_compile_options(HyperEditor PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(HyperEditor PLUGINS)
hc_deploy_document_engine(HyperEditor)

# No WebEngine discovery or linkage on the default product path.
option(HC_ENABLE_RHWP "Build the isolated RHWP web surface (product routing remains gated)" OFF)
if(HC_ENABLE_RHWP)
    set(HC_RHWP_BUNDLE_DIRECTORY "" CACHE PATH "Bundle embedded in the common RHWP helper")
    include("${PROJECT_SOURCE_DIR}/cmake/EmbedRhwpBundle.cmake")
    find_package(Qt6 6.12.0 REQUIRED COMPONENTS WebEngineWidgets WebChannel GLOBAL)
    qt_add_library(hc_document_web STATIC
        ../UniqueLibs/document/hwpHelperDocument.cpp ../UniqueLibs/document/hwpHelperDocument.hpp
        ../UniqueLibs/document/hwpInputTransfer.cpp ../UniqueLibs/document/hwpInputTransfer.hpp
        ../UniqueLibs/document/hwpLoadRetirement.cpp ../UniqueLibs/document/hwpLoadRetirement.hpp
        ../UniqueLibs/document/hwpLoadBridge.cpp ../UniqueLibs/document/hwpLoadBridge.hpp
        ../UniqueLibs/document/hwpBundleHost.cpp ../UniqueLibs/document/hwpBundleHost.hpp
        ../UniqueLibs/document/hwpPackageAdmission.cpp ../UniqueLibs/document/hwpPackageAdmission.hpp)
    target_include_directories(hc_document_web PUBLIC ${PROJECT_SOURCE_DIR})
    target_link_libraries(hc_document_web PUBLIC Qt6::Widgets hc_document_sdi PRIVATE Qt6::WebEngineWidgets Qt6::WebChannel)
    target_compile_options(hc_document_web PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    set(HC_RHWP_APPROVED_MANIFEST_SHA256 "" CACHE STRING "Native release policy manifest pin; empty denies packages")
    if(NOT HC_RHWP_APPROVED_MANIFEST_SHA256 STREQUAL "")
        string(LENGTH "${HC_RHWP_APPROVED_MANIFEST_SHA256}" HC_RHWP_PIN_LENGTH)
        if(NOT HC_RHWP_PIN_LENGTH EQUAL 64 OR NOT HC_RHWP_APPROVED_MANIFEST_SHA256 MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR "RHWP manifest policy requires 64 lowercase hex digits")
        endif()
    endif()
    configure_file(hwpRuntimePolicy.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/hwpRuntimePolicy.hpp" @ONLY NEWLINE_STYLE CRLF)
    qt_add_executable(HyperEditorWeb WIN32 webMain.cpp
        ../UniqueLibs/document/documentWebPeer.cpp ../UniqueLibs/document/documentWebPeer.hpp)
    target_include_directories(HyperEditorWeb PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(HyperEditorWeb PRIVATE hc_startup_fonts)
    target_link_libraries(HyperEditorWeb PRIVATE hc_document_web Qt6::Network shell32)
    target_compile_options(HyperEditorWeb PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    set_target_properties(HyperEditorWeb PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/rhwp-runtime/$<CONFIG>")
    hc_embed_rhwp_bundle(HyperEditorWeb)
    # Build staging only: tools/rhwp/stage-runtime.mjs seals this output into the pinned
    # runtime that the product inventory places in ROOT (Libs/ layout, embedded qt.conf).
    hc_enable_qt_deployment(HyperEditorWeb PLUGINS)
endif()

option(HC_BUILD_MARKDOWN_TESTS "Build focused Markdown integration tests" OFF)
if(HC_BUILD_MARKDOWN_TESTS)
    find_package(Qt6 6.12.0 REQUIRED COMPONENTS Test)
    qt_add_executable(hc_markdown_tests "${PROJECT_SOURCE_DIR}/tests/document/tstMarkdown.cpp")
    target_include_directories(hc_markdown_tests BEFORE PRIVATE "${PROJECT_SOURCE_DIR}/tools/markdown/render" "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/HyperCommander")
    target_link_libraries(hc_markdown_tests PRIVATE hc_document_widgets Qt6::Test Qt6::Network)
    target_compile_options(hc_markdown_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    hc_enable_qt_deployment(hc_markdown_tests TEST PLUGINS)
    add_test(NAME hc.markdown COMMAND hc_markdown_tests)
    set_tests_properties(hc.markdown PROPERTIES TIMEOUT 120)
endif()
