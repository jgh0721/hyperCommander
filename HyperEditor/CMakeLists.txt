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
target_link_libraries(hc_document_broker PUBLIC hc_document_core Qt6::Core PRIVATE Qt6::ZlibPrivate Qt6::Widgets advapi32 shell32 ole32 bcrypt)
target_compile_options(hc_document_broker PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
qt_add_library(hc_document_sdi STATIC
    ../UniqueLibs/document/documentFocusRestore.hpp
    ../UniqueLibs/document/hwpHostSession.cpp ../UniqueLibs/document/hwpHostSession.hpp
    ../UniqueLibs/document/hwpOpenWindow.cpp ../UniqueLibs/document/hwpOpenWindow.hpp
    ../UniqueLibs/nativeMaterializer.cpp ../commonLibs/operations/jobScheduler.cpp
    ../UniqueLibs/document/hwpSdiWindow.cpp ../UniqueLibs/document/hwpSdiWindow.hpp
    ../UniqueLibs/document/hwpProcessBackend.cpp ../UniqueLibs/document/hwpProcessBackend.hpp
)
target_link_libraries(hc_document_sdi PUBLIC hc_document_core Qt6::Widgets PRIVATE hc_document_broker Qt6::Concurrent Qt6::PrintSupport)
target_compile_options(hc_document_sdi PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

qt_add_library(hc_document_widgets STATIC
    ../UniqueLibs/document/imageBook.cpp ../UniqueLibs/document/imageBook.hpp
    ../UniqueLibs/document/imageFramePlayer.cpp ../UniqueLibs/document/imageFramePlayer.hpp
    ../UniqueLibs/document/imageReadingView.cpp ../UniqueLibs/document/imageReadingView.hpp
    ../UniqueLibs/document/imageFullscreen.cpp ../UniqueLibs/document/imageFullscreen.hpp
    ../UniqueLibs/document/documentTextCodec.cpp ../UniqueLibs/document/documentTextCodec.hpp
    ../UniqueLibs/document/documentViewPane.cpp ../UniqueLibs/document/documentViewPane.hpp
    ../UniqueLibs/document/viewUiContribution.hpp
    ../UniqueLibs/document/workspaceChromeController.cpp ../UniqueLibs/document/workspaceChromeController.hpp
    ../UniqueLibs/document/documentThemeAdapter.cpp ../UniqueLibs/document/documentThemeAdapter.hpp
    ../UniqueLibs/document/documentRecoveryStore.cpp ../UniqueLibs/document/documentRecoveryStore.hpp
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
target_link_libraries(hc_document_widgets PUBLIC hc_document_core hc_document_broker hc_document_sdi hc_hypereditor Qt6::Widgets fmstyle PRIVATE Qt6::Concurrent advapi32 shell32 ole32)
target_link_libraries(hc_document_widgets PRIVATE hc_builtin_sevenzip hc_image_archive_transport hc_passwords)
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
hc_add_version_resource(HyperEditor "HyperEditor" ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app/hypereditor.ico")
target_include_directories(HyperEditor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(HyperEditor PRIVATE hc_document_widgets hc_document_sdi fmstyle Qt6::Network)
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
    target_link_libraries(HyperEditorWeb PRIVATE hc_document_web Qt6::Network shell32)
    target_compile_options(HyperEditorWeb PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    set_target_properties(HyperEditorWeb PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/rhwp-runtime/$<CONFIG>")
    hc_embed_rhwp_bundle(HyperEditorWeb)
    # Build staging only: tools/rhwp/stage-runtime.mjs seals this output into the pinned
    # runtime that the product inventory places in ROOT (Libs/ layout, embedded qt.conf).
    hc_enable_qt_deployment(HyperEditorWeb PLUGINS)
endif()
