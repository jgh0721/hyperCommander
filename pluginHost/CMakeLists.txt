qt_add_library(hc_plugin_contract STATIC
    ../commonLibs/plugins/archiveBackend.hpp
    ../commonLibs/plugins/archiveCreation.hpp
    ../commonLibs/plugins/pluginManifest.cpp
    ../commonLibs/plugins/pluginManifest.hpp
    ../commonLibs/plugins/pluginWire.cpp
    ../commonLibs/plugins/pluginWire.hpp
)
set_target_properties(hc_plugin_contract PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(hc_plugin_contract PUBLIC ${PROJECT_SOURCE_DIR})
target_compile_options(hc_plugin_contract PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
qt_add_library(hc_plugin_manifest STATIC manifestFile.cpp manifestFile.hpp)
target_include_directories(hc_plugin_manifest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hc_plugin_manifest PUBLIC hc_plugin_contract Qt6::Core)
target_compile_options(hc_plugin_manifest PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
qt_add_executable(hc-plugin-inspect inspectMain.cpp)
target_link_libraries(hc-plugin-inspect PRIVATE hc_plugin_manifest)
target_compile_options(hc-plugin-inspect PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc-plugin-inspect)
qt_add_library(hc_plugin_process STATIC pipeChannel.cpp pipeChannel.hpp pluginProcess.cpp pluginProcess.hpp
    hostSession.cpp hostSession.hpp sessionClient.cpp sessionClient.hpp
    ../commonLibs/storage/providerSession.hpp ../commonLibs/storage/resourceQuota.hpp
)
target_sources(hc_plugin_process PRIVATE archiveCodec.cpp archiveCodec.hpp hostedArchiveBackend.cpp hostedArchiveBackend.hpp)
target_sources(hc_plugin_process PRIVATE approvedWcxBackend.cpp approvedWcxBackend.hpp)
target_sources(hc_plugin_process PRIVATE wlxDetection.cpp wlxDetection.hpp)
target_sources(hc_plugin_process PRIVATE hostedDataProviders.cpp hostedDataProviders.hpp ../commonLibs/plugins/metadataProvider.hpp ../commonLibs/plugins/contentConverter.hpp)
target_link_libraries(hc_plugin_process PUBLIC hc_plugin_manifest PRIVATE advapi32 user32)
if(HC_RHWP_PRODUCT_RUNTIME_DIRECTORY AND HC_RHWP_APPROVED_INVENTORY_SHA256)
    target_compile_definitions(hc_plugin_process PRIVATE HC_RHWP_HOST_RUNTIME=1)
endif()
if(HC_EPUB_PRODUCT_RUNTIME_DIRECTORY AND HC_EPUB_APPROVED_INVENTORY_SHA256)
    target_compile_definitions(hc_plugin_process PRIVATE HC_EPUB_HOST_RUNTIME=1)
endif()
target_compile_definitions(hc_plugin_process PRIVATE UNICODE _UNICODE)
target_compile_options(hc_plugin_process PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
qt_add_executable(hc-plugin-host-x64 hostMain.cpp)
qt_add_library(hc_wcx_backend STATIC
    ../UniqueLibs/wcxBackend.cpp
    ../UniqueLibs/wcxBackend.hpp
    ../commonLibs/plugins/archiveBackend.hpp
)
target_include_directories(hc_wcx_backend PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(hc_wcx_backend PRIVATE Qt6::Core)
target_compile_options(hc_wcx_backend PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_sources(hc-plugin-host-x64 PRIVATE wcxHostService.cpp wcxHostService.hpp)
target_sources(hc-plugin-host-x64 PRIVATE nativeViewerHostService.cpp nativeViewerHostService.hpp wlxWindowMessage.hpp wlxViewerBackend.cpp wlxViewerBackend.hpp)
target_sources(hc-plugin-host-x64 PRIVATE tcDataHostService.cpp tcDataHostService.hpp)
target_sources(hc-plugin-host-x64 PRIVATE dopusViewerBackend.cpp dopusViewerBackend.hpp dopusAbi.hpp dopusVfsHostService.cpp dopusVfsHostService.hpp)
target_link_libraries(hc-plugin-host-x64 PRIVATE hc_plugin_process hc_wcx_backend Qt6::Widgets advapi32 ole32 shlwapi gdi32)
target_compile_definitions(hc-plugin-host-x64 PRIVATE UNICODE _UNICODE)
target_compile_options(hc-plugin-host-x64 PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc-plugin-host-x64 PLUGINS)
qt_add_library(hc_plugin_viewer_sessions STATIC hostedViewerSession.cpp hostedViewerSession.hpp documentViewerClient.cpp documentViewerClient.hpp)
target_precompile_headers(hc_plugin_viewer_sessions PRIVATE stdafx.h)
target_link_libraries(hc_plugin_viewer_sessions PUBLIC hc_plugin_process hc_viewer_core PRIVATE hc_document_broker)
target_compile_options(hc_plugin_viewer_sessions PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
qt_add_library(hc_plugin_viewer_qt STATIC ../UniqueLibs/qtPluginViewerSurface.cpp ../UniqueLibs/qtPluginViewerSurface.hpp)
target_link_libraries(hc_plugin_viewer_qt PUBLIC hc_plugin_viewer_sessions Qt6::Widgets PRIVATE fmstyle)
target_compile_options(hc_plugin_viewer_qt PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

qt_add_library(hc_image_archive_transport STATIC imageArchiveTransport.cpp imageArchiveTransport.hpp)
target_link_libraries(hc_image_archive_transport PUBLIC hc_plugin_process Qt6::Network)
target_compile_options(hc_image_archive_transport PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
qt_add_library(hc_image_archive_broker STATIC imageArchiveBroker.cpp imageArchiveBroker.hpp)
target_link_libraries(hc_image_archive_broker PUBLIC hc_image_archive_transport PRIVATE hc_builtin_sevenzip hc_passwords Qt6::Widgets)
target_compile_options(hc_image_archive_broker PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_plugin_viewer_sessions PRIVATE hc_image_archive_broker)

hc_add_version_resource(hc-plugin-inspect "HyperCommander Plugin Inspector")
hc_add_version_resource(hc-plugin-host-x64 "HyperCommander Plugin Host")
