# Find includes in the build directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)

add_compile_definitions( QT_NO_DEPRECATED_WARNINGS )

include( ../UniqueLibs/build.common.cmake )

include( ../commonLibs/commonLibs.cmake )
include( ../externalLibs/externalLibs.cmake )

include_directories( ${CMAKE_CURRENT_LIST_DIR}/Modules )
include_directories( ${CMAKE_CURRENT_LIST_DIR}/UIs )

set(PROJECT_SOURCES
        cmnConfig.hpp
        HyperCommander.cpp
        HyperCommander.hpp
        HyperCommander.qrc
        main.cpp
        resource.h
        stdafx.h
)

set(PROJECT_MODULE_SRC
        Modules/mdlFileEngine.cpp
        Modules/mdlFileEngine.hpp
        Modules/mdlFileModel.cpp
        Modules/mdlFileModel.hpp
        Modules/mdlFileOperation.cpp
        Modules/mdlFileOperation.hpp
        Modules/mdlFileSystem.cpp
        Modules/mdlFileSystem.hpp
)

set(PROJECT_UI_SRC
        UIs/itemSelectionController.cpp
        UIs/itemSelectionController.hpp
        ../UniqueLibs/itemSelection.hpp
        UIs/Components/cmpView.cpp
        UIs/Components/cmpViewGrouping.cpp
        UIs/Components/cmpView.hpp
        UIs/Components/thumbnailModel.cpp
        UIs/Components/thumbnailModel.hpp
        UIs/Components/thumbnailSettings.hpp
        UIs/keyboardSettingsPage.hpp
        UIs/themeSettingsPage.hpp
        UIs/menuBarSettingsPage.hpp
        UIs/commandSelector.hpp
        UIs/thumbnailSettingsPage.hpp
        UIs/Components/filePanelSettings.hpp
        UIs/fileToolsDialogs.cpp
        UIs/renameHistoryDialog.cpp
        UIs/fileToolsDialogs.hpp
        UIs/permissionDialogs.hpp
        ../UniqueLibs/permissionService.hpp
        ../UniqueLibs/archiveExtractJob.cpp
        ../UniqueLibs/archiveExtractJob.hpp
        ../UniqueLibs/fileBatchOperations.hpp
        UIs/dialogShell.cpp
        UIs/nativePathCompletion.cpp
        UIs/nativePathCompletion.hpp
        UIs/fileLockDialog.cpp
        UIs/fileLockDialog.hpp
        UIs/dialogShell.hpp
        UIs/settingsDraft.hpp
        ../UniqueLibs/settings/singleInstance.hpp
        UIs/fileColorRulesPage.hpp
        ../UniqueLibs/settings/fileColorRules.hpp
        UIs/cmnUiUtils.cpp
        UIs/cmnUiUtils.hpp
        UIs/cmpPanel.cpp
        UIs/cmpPanel.hpp
        UIs/cmpPanel.ui
        UIs/panelPathBar.cpp
        UIs/panelPathBar.hpp
        UIs/cmpViewer.cpp
        UIs/cmpViewer.hpp
        UIs/cmpViewer.ui
        UIs/dlgExternalConfigure.cpp
        UIs/dlgExternalConfigure.hpp
        UIs/dlgExternalConfigure.ui
        UIs/dlgExternalConfigure_Item.cpp
        UIs/dlgExternalConfigure_Item.hpp
        UIs/dlgExternalConfigure_Item.ui
        UIs/dlgFavoriteDir.cpp
        UIs/dlgFavoriteDir.hpp
        UIs/dlgFavoriteDir.ui
        UIs/dlgMain.cpp
        UIs/dlgMain.hpp
        UIs/dlgMain.ui
        UIs/dlgOpts.cpp
        UIs/dlgOpts.hpp
        UIs/dlgOpts.ui
        UniqueLibs/uiComboBoxWidget.cpp
        UniqueLibs/uiComboBoxWidget.hpp
)

set(UNIQUELIBS_SRC
        ../UniqueLibs/qtPagedStorage.cpp
        ../UniqueLibs/qtPagedStorage.hpp
        ../commonLibs/storage/entryIndex.hpp
        ../UniqueLibs/qtItemOutcomeStore.cpp
        ../UniqueLibs/qtItemOutcomeStore.hpp
        ../commonLibs/operations/itemOutcomeStore.hpp
        ../UniqueLibs/columnNativeFields.cpp
        ../UniqueLibs/columnNativeFields.hpp
        ../UniqueLibs/columnPluginFields.cpp
        ../UniqueLibs/columnPluginFields.hpp
        ../commonLibs/storage/search.cpp
        ../commonLibs/storage/search.hpp
        ../UniqueLibs/qtSearch.cpp
        ../UniqueLibs/qtSearch.hpp
        ../commonLibs/commands/automation.cpp
        ../commonLibs/commands/automation.hpp
        ../commonLibs/scripting/scriptApi.hpp
        ../UniqueLibs/qtAutomation.cpp
        ../UniqueLibs/qtAutomationCompiler.cpp
        ../UniqueLibs/qtAutomationLibrary.cpp
        ../UniqueLibs/qtAutomation.hpp
        ../UniqueLibs/qtWorkspaceState.cpp
        ../UniqueLibs/qtWorkspaceState.hpp
        ../UniqueLibs/qtWorkspaceRestore.cpp
        ../UniqueLibs/qtWorkspaceRestore.hpp
        ../UniqueLibs/qtDirectoryTracker.cpp
        ../UniqueLibs/qtDirectoryTracker.hpp
        ../UniqueLibs/qtPluginHostSettings.cpp
        ../UniqueLibs/qtPluginHostSettings.hpp
        ../UniqueLibs/qtPluginManagementPage.cpp
        ../UniqueLibs/qtPluginManagementPage.hpp
        ../UniqueLibs/tcPluginPackage.hpp
        ../pluginHost/pluginApplicationRule.hpp
        ../commonLibs/operations/jobScheduler.cpp
        ../commonLibs/operations/jobScheduler.hpp
        ../commonLibs/operations/fileOperation.cpp
        ../commonLibs/operations/fileOperation.hpp
        ../commonLibs/storage/writableStorage.hpp
        ../UniqueLibs/nativeWritableStorage.cpp
        ../UniqueLibs/nativeWritableStorage.hpp
        ../UniqueLibs/nativeMaterializer.cpp
        ../UniqueLibs/nativeMaterializer.hpp
        ../UniqueLibs/wcxStorage.cpp
        ../UniqueLibs/wcxStorage.hpp
        ../UniqueLibs/wcxMutation.cpp
        ../UniqueLibs/wcxMutation.hpp
        ../commonLibs/storage/archiveMutation.hpp
        ../commonLibs/operations/archiveOperation.cpp
        ../commonLibs/operations/moveOperation.cpp
        ../commonLibs/operations/moveOperation.hpp
        ../commonLibs/operations/archiveOperation.hpp
        ../commonLibs/storage/materializedContent.hpp
        ../commonLibs/commands/commandBus.hpp
        ../UniqueLibs/commandUiBridge.cpp
        ../UniqueLibs/commandUiBridge.hpp
        ../UniqueLibs/nativeFileElevation.cpp
        ../UniqueLibs/nativeFileElevation.hpp
        ../UniqueLibs/fileJobUi.cpp
        UIs/fileAttributesDialog.cpp
        UIs/fileAttributesDialog.hpp
        ../UniqueLibs/fileAttributes.hpp
        ../UniqueLibs/fileJobUi.hpp
        ../commonLibs/columns/columnContracts.hpp
        ../commonLibs/columns/builtInColumns.hpp
        ../commonLibs/columns/columnProfileStorage.hpp
        ../commonLibs/view/columnWidthCodec.hpp
        ../commonLibs/view/columnWidthPolicy.hpp
        ../UniqueLibs/settings/appSettings.hpp
        ../UniqueLibs/columnWidthSettings.hpp
        ../UniqueLibs/qtitanColumnAdapter.hpp
        ../UniqueLibs/columnAutoSelection.hpp
        ../UniqueLibs/qtColumnProfileEditor.cpp
        ../UniqueLibs/qtColumnProfileEditor.hpp
        ../UniqueLibs/qtitanColumnAdapter.cpp
        ../commonLibs/storage/storage.hpp
        ../UniqueLibs/storageProviders.cpp
        ../UniqueLibs/storageProviders.hpp
        ../UniqueLibs/backgroundJobMgr.cpp
        ../UniqueLibs/backgroundJobMgr.hpp
        ../UniqueLibs/builtInFsModel.cpp
        ../UniqueLibs/builtInFsModel.hpp
        ../UniqueLibs/everythingSearchSession.hpp
        ../UniqueLibs/builtInVolumeModel.cpp
        ../UniqueLibs/builtInVolumeModel.hpp
        ../UniqueLibs/cmnHCModelUtils.cpp
        ../UniqueLibs/cmnHCModelUtils.hpp
        ../UniqueLibs/cmnHCUtils.cpp
        ../UniqueLibs/cmnHCUtils.hpp
        ../UniqueLibs/cmnTypeDefs.hpp
        ../UniqueLibs/cmnTypeDefs_Name.hpp
        ../UniqueLibs/cmnTypeDefs_Opts.cpp
        ../UniqueLibs/cmnTypeDefs_Opts.hpp
        ../UniqueLibs/colorSchemeMgr.cpp
        ../UniqueLibs/colorSchemeMgr.hpp
        ../UniqueLibs/column_Defs.hpp
        ../UniqueLibs/columnMgr.cpp
        ../UniqueLibs/columnMgr.hpp
        ../UniqueLibs/commandMgr.cpp
        ../UniqueLibs/commandMgr.hpp
        ../UniqueLibs/commandMgr_Defs.hpp
        ../UniqueLibs/externalEditorMgr.cpp
        ../UniqueLibs/externalEditorMgr.hpp
        ../UniqueLibs/fileSetMgr.cpp
        ../UniqueLibs/fileSetMgr.hpp
        ../UniqueLibs/globalCtx.hpp
        ../UniqueLibs/model_defs.cpp
        ../UniqueLibs/model_defs.hpp
        ../UniqueLibs/fileIconService.cpp
        ../UniqueLibs/fileIconService.hpp
        ../UniqueLibs/nativeIconExtractor.cpp
        ../UniqueLibs/nativeIconExtractor.hpp
        ../UniqueLibs/pixmapCacheEx.cpp
        ../UniqueLibs/pixmapCacheEx.hpp
        ../UniqueLibs/SHChangeNotify.cpp
        ../UniqueLibs/SHChangeNotify.hpp
        ../UniqueLibs/solAdminWIthIPC.hpp
        ../UniqueLibs/solFavoriteDirMgr.cpp
        ../UniqueLibs/solFavoriteDirMgr.hpp
        ../UniqueLibs/solMultiLang_Defs.hpp
        ../UniqueLibs/solTCPluginMgr.cpp
        ../UniqueLibs/solTCPluginMgr.hpp
)
qt_add_executable(HyperCommander
        ${PROJECT_SOURCES}
        ${COMMONLIBS_SRC} ${EXTERNAL_SRC}
        ${PROJECT_MODULE_SRC}
        ${PROJECT_UI_SRC}
        ${UNIQUELIBS_SRC}
)
hc_add_version_resource(HyperCommander "HyperCommander" ICON "${CMAKE_CURRENT_SOURCE_DIR}/Resources/HyperCommander.ico")

target_precompile_headers( HyperCommander PRIVATE stdafx.h )

# Export the actual target inputs so the legacy VS project can be checked for drift.
set(HC_SOURCE_INVENTORY "")
set(HC_PROJECT_INVENTORY "")
foreach(HC_INVENTORY_TARGET IN ITEMS HyperCommander hc_archive_creation hc_passwords hc_script_runtime hc_builtin_iso hc_builtin_sevenzip hc_builtin_unegg hc_viewer_core hc_viewer_qt hc_plugin_viewer_qt hc_plugin_viewer_sessions hc_wcx_backend hc_plugin_contract hc_plugin_manifest hc_plugin_process hc_update_core hc_update_qt)
    get_target_property(HC_SOURCE_INPUTS ${HC_INVENTORY_TARGET} SOURCES)
    get_target_property(HC_SOURCE_DIRECTORY ${HC_INVENTORY_TARGET} SOURCE_DIR)
    get_target_property(HC_TARGET_BINARY_DIRECTORY ${HC_INVENTORY_TARGET} BINARY_DIR)
    string(APPEND HC_PROJECT_INVENTORY "${HC_TARGET_BINARY_DIRECTORY}/${HC_INVENTORY_TARGET}.vcxproj\n")
    foreach(HC_SOURCE IN LISTS HC_SOURCE_INPUTS)
        if(NOT HC_SOURCE MATCHES "\\$<")
            cmake_path(ABSOLUTE_PATH HC_SOURCE BASE_DIRECTORY "${HC_SOURCE_DIRECTORY}" NORMALIZE OUTPUT_VARIABLE HC_SOURCE_ABSOLUTE)
            string(APPEND HC_SOURCE_INVENTORY "${HC_SOURCE_ABSOLUTE}\n")
        endif()
    endforeach()
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/hc-product-sources.txt" "${HC_SOURCE_INVENTORY}")
file(WRITE "${CMAKE_BINARY_DIR}/hc-product-projects.txt" "${HC_PROJECT_INVENTORY}")

target_include_directories(HyperCommander PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/UniqueLibs
)

target_compile_options(HyperCommander PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/bigobj>
)

set_target_properties(HyperCommander
        PROPERTIES
        WIN32_EXECUTABLE TRUE
)

target_link_libraries(HyperCommander
        PRIVATE
        hc_script_runtime
        Qtitan::Grid
        Fm::style
        Fm::widgets
        Fm::filelist
        Qt::Core
        Qt::Concurrent
        Qt::Gui
        Qt::Sql
        Qt::Network
        Qt::UiTools
        Qt::PrintSupport
        Qt::Widgets
        Qt6::CorePrivate
        Qt6::GuiPrivate
        Qt6::WidgetsPrivate
        hc_viewer_qt hc_document_open
        hc_file_locks
        hc_wcx_backend
        hc_plugin_process
        hc_builtin_iso
        hc_archive_creation
        hc_passwords
        hc_builtin_sevenzip hc_builtin_unegg hc_update_qt
)

# Keep the application executable immediately runnable from its build directory.
add_custom_command(TARGET HyperCommander POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:HyperCommander>/Licenses/Lua"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/commonLibs/scripting/ThirdPartyNotices.txt"
        "$<TARGET_FILE_DIR:HyperCommander>/Licenses/Lua/ThirdPartyNotices.txt"
    VERBATIM)
hc_enable_qt_deployment(HyperCommander PLUGINS RUNTIME_FILE "${PROJECT_SOURCE_DIR}/externalLibs/Everything/src/dll/Everything64.dll")
add_dependencies(HyperCommander hc-plugin-host-x64 hc_hyperviewer_package HyperCommanderUpdater)

# Opt-in live integration tests compile the same UI/model sources with a test entry
# point. They must never run implicitly on developer or CI machines without a NAS.
option(HC_BUILD_SMB_TESTS "Build the live SMB application integration tests" OFF)
if(HC_BUILD_SMB_TESTS)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Test)
    qt_add_library(hc_wcx_fixture SHARED ../tests/wcxFixture.cpp)
    target_include_directories(hc_wcx_fixture PRIVATE ${PROJECT_SOURCE_DIR})
    target_compile_options(hc_wcx_fixture PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    set_target_properties(hc_wcx_fixture PROPERTIES SUFFIX ".wcx64")
    set(HC_SMB_SOURCES ${PROJECT_SOURCES})
    list(REMOVE_ITEM HC_SMB_SOURCES main.cpp)
    qt_add_executable(hc_smb_tests
        ../tests/tstSmb.cpp
        ../tests/driveProbeFixture.hpp
        ${HC_SMB_SOURCES}
        ${COMMONLIBS_SRC} ${EXTERNAL_SRC}
        ${PROJECT_MODULE_SRC} ${PROJECT_UI_SRC} ${UNIQUELIBS_SRC}
    )
    target_precompile_headers(hc_smb_tests PRIVATE stdafx.h)
    target_include_directories(hc_smb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/UniqueLibs)
    target_compile_options(hc_smb_tests PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/bigobj>
    )
    get_target_property(HC_PRODUCT_LIBRARIES HyperCommander LINK_LIBRARIES)
    target_link_libraries(hc_smb_tests PRIVATE ${HC_PRODUCT_LIBRARIES} Qt6::Test)
    add_dependencies(hc_smb_tests hc_wcx_fixture hc-plugin-host-x64)
    hc_enable_qt_deployment(hc_smb_tests TEST PLUGINS RUNTIME_FILE "${PROJECT_SOURCE_DIR}/externalLibs/Everything/src/dll/Everything64.dll")
endif()

add_custom_command(TARGET HyperCommander POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:HyperCommander>/Licenses/waterColorStyle"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/externalLibs/waterColorStyle/LICENSE"
        "$<TARGET_FILE_DIR:HyperCommander>/Licenses/waterColorStyle/LICENSE"
    VERBATIM)
