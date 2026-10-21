option(HC_ENABLE_LOCAL_TEST_TOOLS "Register tests using untracked local PowerShell tools" ON)
qt_add_executable(hc_update_tests tstPortableUpdate.cpp)
target_sources(hc_update_tests PRIVATE "${PROJECT_SOURCE_DIR}/cmake/updateLongPaths.manifest")
target_link_libraries(hc_update_tests PRIVATE hc_update_qt)
target_compile_options(hc_update_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME hc.update.contract COMMAND hc_update_tests contract)
set_tests_properties(hc.update.contract PROPERTIES TIMEOUT 30)
hc_enable_qt_deployment(hc_update_tests TEST)
qt_add_executable(hc_update_service_tests tstUpdateService.cpp)
target_sources(hc_update_service_tests PRIVATE "${PROJECT_SOURCE_DIR}/cmake/updateLongPaths.manifest")
target_link_libraries(hc_update_service_tests PRIVATE hc_update_qt Qt6::Network)
target_compile_options(hc_update_service_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
hc_enable_qt_deployment(hc_update_service_tests TEST PLUGINS)

find_package(Qt6 6.12 REQUIRED COMPONENTS Test)
qt_add_executable(hc_plugin_management_tests tstPluginManagement.cpp
    ../UniqueLibs/qtPluginManagementPage.cpp ../UniqueLibs/qtPluginHostSettings.cpp ../UniqueLibs/nativeMaterializer.cpp)
target_include_directories(hc_plugin_management_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(hc_plugin_management_tests PRIVATE hc_plugin_viewer_qt hc_archive_creation Qt6::Widgets Qt6::Concurrent Qt6::Test Qt6::CorePrivate)
target_compile_options(hc_plugin_management_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.plugin_management COMMAND hc_plugin_management_tests)
add_dependencies(hc_plugin_management_tests hc_wcx_fixture hc_wlx_fixture hc_sevenzip_package hc-plugin-host-x64)
set_tests_properties(hc.plugin_management PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
hc_enable_qt_deployment(hc_plugin_management_tests TEST PLUGINS)
qt_add_executable(hc_batch_rename_tests tstBatchRename.cpp)
target_link_libraries(hc_batch_rename_tests PRIVATE hc_viewer_qt Qt6::Test)
target_compile_options(hc_batch_rename_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.batch_rename COMMAND hc_batch_rename_tests)
set_tests_properties(hc.batch_rename PROPERTIES TIMEOUT 180)
hc_enable_qt_deployment(hc_batch_rename_tests TEST)
qt_add_executable(hc_paged_storage_tests tstPagedStorage.cpp ../UniqueLibs/qtPagedStorage.cpp ../UniqueLibs/qtItemOutcomeStore.cpp ../UniqueLibs/qtWorkspaceState.cpp)
target_include_directories(hc_paged_storage_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(hc_paged_storage_tests PRIVATE Qt6::Test psapi)
target_compile_options(hc_paged_storage_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.paged_storage COMMAND hc_paged_storage_tests -o "${CMAKE_BINARY_DIR}/paged-storage-$<CONFIG>.txt,txt")
set_tests_properties(hc.paged_storage PROPERTIES TIMEOUT 600 LABELS "storage;large-list")
hc_enable_qt_deployment(hc_paged_storage_tests TEST)
qt_add_executable(hc_operation_recovery_tests tstOperationRecovery.cpp
    ../UniqueLibs/qtJobJournal.cpp ../UniqueLibs/qtDurableOutcomes.cpp ../UniqueLibs/qtPagedStorage.cpp ../UniqueLibs/qtItemOutcomeStore.cpp
    ../UniqueLibs/nativeWritableStorage.cpp ../UniqueLibs/storageProviders.cpp ../commonLibs/operations/fileOperation.cpp)
target_link_libraries(hc_operation_recovery_tests PRIVATE hc_viewer_core Qt6::Test)
target_include_directories(hc_operation_recovery_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_compile_options(hc_operation_recovery_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.operation_recovery COMMAND hc_operation_recovery_tests -o "${CMAKE_BINARY_DIR}/operation-recovery-$<CONFIG>.txt,txt")
set_tests_properties(hc.operation_recovery PROPERTIES TIMEOUT 180)
hc_enable_qt_deployment(hc_operation_recovery_tests TEST)
qt_add_executable(hc_lua_tests tstLuaRuntime.cpp)
target_link_libraries(hc_lua_tests PRIVATE hc_script_runtime Qt6::Test)
target_compile_options(hc_lua_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.lua_runtime COMMAND hc_lua_tests)
set_tests_properties(hc.lua_runtime PROPERTIES TIMEOUT 30 LABELS "script;selected-runtime")
hc_enable_qt_deployment(hc_lua_tests TEST)
qt_add_executable(hc_script_api_tests tstScriptApi.cpp ../commonLibs/scripting/scriptApi.cpp)
target_link_libraries(hc_script_api_tests PRIVATE Qt6::Test)
target_compile_options(hc_script_api_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.script_api COMMAND hc_script_api_tests)
set_tests_properties(hc.script_api PROPERTIES TIMEOUT 30)
hc_enable_qt_deployment(hc_script_api_tests TEST)
qt_add_executable(hc_automation_tests tstAutomation.cpp
    ../commonLibs/commands/automation.cpp ../UniqueLibs/qtAutomation.cpp ../UniqueLibs/qtAutomationCompiler.cpp ../UniqueLibs/qtAutomationLibrary.cpp)
target_include_directories(hc_automation_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(hc_automation_tests PRIVATE Qt6::Test Qt6::Widgets hc_script_runtime)
target_compile_options(hc_automation_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.automation COMMAND hc_automation_tests)
set_tests_properties(hc.automation PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
hc_enable_qt_deployment(hc_automation_tests TEST PLUGINS)
qt_add_executable(hc_search_tests
    tstSearch.cpp
    ../commonLibs/storage/search.cpp
    ../UniqueLibs/qtSearch.cpp
    ../UniqueLibs/storageProviders.cpp
)
target_include_directories(hc_search_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/commonLibs" "${PROJECT_SOURCE_DIR}/HyperCommander")
target_link_libraries(hc_search_tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Concurrent shell32 ole32 netapi32)
target_compile_options(hc_search_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_search_tests hc-search-index)
add_test(NAME hc.search COMMAND hc_search_tests)
set_tests_properties(hc.search PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
hc_enable_qt_deployment(hc_search_tests TEST PLUGINS)
qt_add_executable(hc_workspace_tests
    tstWorkspace.cpp
    ../UniqueLibs/qtWorkspaceState.cpp
    ../UniqueLibs/qtWorkspaceState.hpp
    ../UniqueLibs/qtDirectoryTracker.cpp
    ../UniqueLibs/qtDirectoryTracker.hpp
)
target_link_libraries(hc_workspace_tests PRIVATE Qt6::Test)
target_include_directories(hc_workspace_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_compile_options(hc_workspace_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.workspace COMMAND hc_workspace_tests)
set_tests_properties(hc.workspace PROPERTIES TIMEOUT 30)
hc_enable_qt_deployment(hc_workspace_tests TEST)
qt_add_executable(hc_iso_engine_tests ../HyperCommanderBuiltInPlugin/IsoImage/tests/tstIsoEngine.cpp)
target_link_libraries(hc_iso_engine_tests PRIVATE hc_iso_core Qt6::Test)
target_compile_options(hc_iso_engine_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.iso_engine COMMAND hc_iso_engine_tests)
set_tests_properties(hc.iso_engine PROPERTIES TIMEOUT 60)
hc_enable_qt_deployment(hc_iso_engine_tests TEST)
qt_add_executable(hc_iso_provider_tests ../HyperCommanderBuiltInPlugin/IsoImage/tests/tstIsoProvider.cpp)
target_link_libraries(hc_iso_provider_tests PRIVATE hc_builtin_iso Qt6::Test)
target_compile_options(hc_iso_provider_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_iso_provider_tests hc-plugin-host-x64)
add_test(NAME hc.iso_provider COMMAND hc_iso_provider_tests -o "${CMAKE_BINARY_DIR}/iso-provider-$<CONFIG>.txt,txt")
set_tests_properties(hc.iso_provider PROPERTIES TIMEOUT 120)
hc_enable_qt_deployment(hc_iso_provider_tests TEST)
qt_add_executable(hc_iso_wcx_tests ../HyperCommanderBuiltInPlugin/IsoImage/tests/tstIsoWcx.cpp)
target_link_libraries(hc_iso_wcx_tests PRIVATE hc_builtin_iso Qt6::Test)
target_compile_options(hc_iso_wcx_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_iso_wcx_tests hc-plugin-host-x64)
add_test(NAME hc.iso_wcx COMMAND hc_iso_wcx_tests)
set_tests_properties(hc.iso_wcx PROPERTIES TIMEOUT 120)
hc_enable_qt_deployment(hc_iso_wcx_tests TEST)
qt_add_executable(hc_iso_job_tests
    ../HyperCommanderBuiltInPlugin/IsoImage/tests/tstIsoJob.cpp
    ../UniqueLibs/storageProviders.cpp
    ../UniqueLibs/nativeWritableStorage.cpp
    ../commonLibs/operations/fileOperation.cpp
)
target_include_directories(hc_iso_job_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_link_libraries(hc_iso_job_tests PRIVATE hc_builtin_iso Qt6::Test)
target_compile_options(hc_iso_job_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_iso_job_tests hc-plugin-host-x64)
add_test(NAME hc.iso_job COMMAND hc_iso_job_tests)
set_tests_properties(hc.iso_job PROPERTIES TIMEOUT 120)
hc_enable_qt_deployment(hc_iso_job_tests TEST)
find_program(HC_POWERSHELL NAMES powershell REQUIRED)
qt_add_library(hc_dopus_fixture SHARED dopusFixture.cpp)
target_include_directories(hc_dopus_fixture PRIVATE "${PROJECT_SOURCE_DIR}")
target_compile_options(hc_dopus_fixture PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_dopus_fixture PRIVATE ole32 gdi32 user32)
qt_add_executable(hc_hosted_dopus_tests tstHostedDopus.cpp ../UniqueLibs/nativeMaterializer.cpp ../UniqueLibs/storageProviders.cpp)
target_include_directories(hc_hosted_dopus_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/commonLibs" "${PROJECT_SOURCE_DIR}/HyperCommander")
target_link_libraries(hc_hosted_dopus_tests PRIVATE hc_plugin_process hc_viewer_core Qt6::Test)
target_compile_options(hc_hosted_dopus_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_hosted_dopus_tests hc-plugin-host-x64 hc_dopus_fixture)
add_test(NAME hc.hosted_dopus COMMAND hc_hosted_dopus_tests)
set_tests_properties(hc.hosted_dopus PROPERTIES TIMEOUT 60)
hc_enable_qt_deployment(hc_hosted_dopus_tests TEST)
qt_add_executable(hc_dopus_viewer_tests tstDopusViewer.cpp ../UniqueLibs/nativeMaterializer.cpp)
target_link_libraries(hc_dopus_viewer_tests PRIVATE hc_viewer_qt Qt6::Test)
target_compile_options(hc_dopus_viewer_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_dopus_viewer_tests hc-plugin-host-x64 hc_dopus_fixture)
add_test(NAME hc.dopus_viewer COMMAND hc_dopus_viewer_tests)
set_tests_properties(hc.dopus_viewer PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
hc_enable_qt_deployment(hc_dopus_viewer_tests TEST PLUGINS)
set(HC_DOPUS_SDK_DIRECTORY "" CACHE PATH "External extracted Opus SDK directory (contains headers)")
if(HC_DOPUS_SDK_DIRECTORY)
    qt_add_executable(hc_dopus_sdk_abi_tests tstDopusSdkAbi.cpp)
    set_target_properties(hc_dopus_sdk_abi_tests PROPERTIES HC_NATIVE_EXECUTABLE ON)
    target_include_directories(hc_dopus_sdk_abi_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${HC_DOPUS_SDK_DIRECTORY}/headers")
    target_compile_options(hc_dopus_sdk_abi_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    add_test(NAME hc.dopus_sdk_abi COMMAND hc_dopus_sdk_abi_tests)
endif()
qt_add_library(hc_tc_data_fixture SHARED tcDataFixture.cpp)
target_compile_options(hc_tc_data_fixture PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
qt_add_executable(hc_hosted_data_tests tstHostedData.cpp ../UniqueLibs/nativeMaterializer.cpp ../UniqueLibs/storageProviders.cpp ../UniqueLibs/qtJobJournal.cpp
    ../UniqueLibs/qtDurableOutcomes.cpp ../UniqueLibs/qtPagedStorage.cpp ../UniqueLibs/qtItemOutcomeStore.cpp)
target_include_directories(hc_hosted_data_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_link_libraries(hc_hosted_data_tests PRIVATE hc_plugin_process hc_viewer_core Qt6::Test)
target_compile_options(hc_hosted_data_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_hosted_data_tests hc-plugin-host-x64 hc_tc_data_fixture)
add_test(NAME hc.hosted_data COMMAND hc_hosted_data_tests)
set_tests_properties(hc.hosted_data PROPERTIES TIMEOUT 60)
hc_enable_qt_deployment(hc_hosted_data_tests TEST)
qt_add_executable(hc_wlx_detection_tests tstWlxDetection.cpp)
target_link_libraries(hc_wlx_detection_tests PRIVATE hc_plugin_process Qt6::Test)
target_compile_options(hc_wlx_detection_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.wlx_detection COMMAND hc_wlx_detection_tests)
set_tests_properties(hc.wlx_detection PROPERTIES TIMEOUT 30)
hc_enable_qt_deployment(hc_wlx_detection_tests TEST)
qt_add_executable(hc_viewer_tests
    tstViewer.cpp
    ../UniqueLibs/storageProviders.cpp
    ../UniqueLibs/wcxStorage.cpp
    ../UniqueLibs/nativeMaterializer.cpp
)
target_include_directories(hc_viewer_tests PRIVATE ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_compile_options(hc_viewer_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_viewer_tests PRIVATE Qt6::Test hc_viewer_qt hc_wcx_backend)
add_test(NAME hc.viewer COMMAND hc_viewer_tests)
set_tests_properties(hc.viewer PROPERTIES TIMEOUT 60)
hc_enable_qt_deployment(hc_viewer_tests TEST PLUGINS)
qt_add_executable(hc_viewer_surface_tests tstViewerSurface.cpp)
target_include_directories(hc_viewer_surface_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(hc_viewer_surface_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_viewer_surface_tests PRIVATE hc_viewer_qt Qt6::Test)
if(HC_ENABLE_LOCAL_TEST_TOOLS AND EXISTS "${PROJECT_SOURCE_DIR}/tools/Test-ViewerSurface.ps1")
    add_test(NAME hc.viewer_surface COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tools/Test-ViewerSurface.ps1"
        -Executable "$<TARGET_FILE:hc_viewer_surface_tests>" -Dpr 1)
    set_tests_properties(hc.viewer_surface PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    add_test(NAME hc.viewer_surface150 COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tools/Test-ViewerSurface.ps1"
        -Executable "$<TARGET_FILE:hc_viewer_surface_tests>" -Dpr 1.5)
    set_tests_properties(hc.viewer_surface150 PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()
hc_enable_qt_deployment(hc_viewer_surface_tests TEST PLUGINS)
if(NOT TARGET hc_wcx_fixture)
    qt_add_library(hc_wcx_fixture SHARED wcxFixture.cpp)
    target_include_directories(hc_wcx_fixture PRIVATE ${PROJECT_SOURCE_DIR})
    target_compile_options(hc_wcx_fixture PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    set_target_properties(hc_wcx_fixture PROPERTIES SUFFIX ".wcx64")
endif()
add_dependencies(hc_viewer_tests hc_wcx_fixture)
qt_add_executable(hc_plugin_tests tstPluginManifest.cpp)
target_link_libraries(hc_plugin_tests PRIVATE hc_plugin_manifest Qt6::Test)
target_compile_options(hc_plugin_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_plugin_tests hc_wcx_fixture hc-plugin-inspect)
add_test(NAME hc.plugin_manifest COMMAND hc_plugin_tests)
set_tests_properties(hc.plugin_manifest PROPERTIES TIMEOUT 30)
hc_enable_qt_deployment(hc_plugin_tests TEST)
foreach(HC_FAULT IN ITEMS crash hang child detach slowstart attach)
    qt_add_library(hc_plugin_fault_${HC_FAULT} SHARED pluginHostFaultFixture.cpp)
    target_include_directories(hc_plugin_fault_${HC_FAULT} PRIVATE ${PROJECT_SOURCE_DIR})
    target_compile_options(hc_plugin_fault_${HC_FAULT} PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
endforeach()
target_compile_definitions(hc_plugin_fault_crash PRIVATE HC_HOST_FAULT=1)
target_compile_definitions(hc_plugin_fault_hang PRIVATE HC_HOST_FAULT=2)
target_compile_definitions(hc_plugin_fault_child PRIVATE HC_HOST_FAULT=3)
target_compile_definitions(hc_plugin_fault_detach PRIVATE HC_HOST_FAULT=4)
target_compile_definitions(hc_plugin_fault_slowstart PRIVATE HC_HOST_FAULT=5)
target_compile_definitions(hc_plugin_fault_attach PRIVATE HC_HOST_FAULT=6)
qt_add_executable(hc_plugin_process_tests tstPluginProcess.cpp)
qt_add_executable(hc_plugin_owner_fixture pluginOwnerFixture.cpp)
target_link_libraries(hc_plugin_owner_fixture PRIVATE hc_plugin_process)
target_compile_options(hc_plugin_owner_fixture PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc_plugin_owner_fixture)
target_link_libraries(hc_plugin_process_tests PRIVATE hc_plugin_process Qt6::Test)
target_compile_options(hc_plugin_process_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_plugin_process_tests hc-plugin-host-x64 hc_plugin_owner_fixture hc_wcx_fixture hc_dopus_fixture hc_plugin_fault_crash hc_plugin_fault_hang hc_plugin_fault_child hc_plugin_fault_detach hc_plugin_fault_slowstart hc_plugin_fault_attach)
add_test(NAME hc.plugin_process COMMAND hc_plugin_process_tests)
set_tests_properties(hc.plugin_process PROPERTIES TIMEOUT 45)
hc_enable_qt_deployment(hc_plugin_process_tests TEST)
qt_add_library(hc_wlx_fixture SHARED wlxFixture.cpp)
target_link_libraries(hc_wlx_fixture PRIVATE user32 gdi32)
target_compile_options(hc_wlx_fixture PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
set_target_properties(hc_wlx_fixture PROPERTIES SUFFIX ".wlx64")
qt_add_library(hc_wlx_sdk_fixture SHARED wlxSdkFixture.cpp wlxSdkFixture.def)
target_include_directories(hc_wlx_sdk_fixture PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(hc_wlx_sdk_fixture PRIVATE user32)
target_compile_options(hc_wlx_sdk_fixture PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
set_target_properties(hc_wlx_sdk_fixture PROPERTIES SUFFIX ".wlx64")
if(TARGET hc_smb_tests AND HC_ENABLE_LOCAL_TEST_TOOLS AND EXISTS "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1")
    foreach(HC_SELECTION_DPR 1 1.5)
        string(REPLACE "." "_" HC_SELECTION_SUFFIX "${HC_SELECTION_DPR}")
        add_test(NAME hc.selection_ui_${HC_SELECTION_SUFFIX}
            COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
                -Executable "$<TARGET_FILE:hc_smb_tests>" -SelectionOnly -Dpr ${HC_SELECTION_DPR} -TimeoutSeconds 90)
        set_tests_properties(hc.selection_ui_${HC_SELECTION_SUFFIX} PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE LABELS "interactive;selection")
    endforeach()
    add_test(NAME hc.automation_ui
        COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
            -Executable "$<TARGET_FILE:hc_smb_tests>" -AutomationOnly -Dpr 1)
    set_tests_properties(hc.automation_ui PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    foreach(HC_WORKSPACE_DPR IN ITEMS 1 1.5)
        add_test(NAME hc.wlx_ui_${HC_WORKSPACE_DPR}
            COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
                -Executable "$<TARGET_FILE:hc_smb_tests>" -WlxOnly -Dpr ${HC_WORKSPACE_DPR})
        set_tests_properties(hc.wlx_ui_${HC_WORKSPACE_DPR} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE LABELS "interactive;wlx")
        add_test(NAME hc.workspace_ui_${HC_WORKSPACE_DPR}
            COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
                -Executable "$<TARGET_FILE:hc_smb_tests>" -WorkspaceOnly -Dpr ${HC_WORKSPACE_DPR})
        set_tests_properties(hc.workspace_ui_${HC_WORKSPACE_DPR} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
        add_test(NAME hc.qtitan_ui_${HC_WORKSPACE_DPR}
            COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
                -Executable "$<TARGET_FILE:hc_smb_tests>" -QtitanOnly -Dpr ${HC_WORKSPACE_DPR} -TimeoutSeconds 240)
        set_tests_properties(hc.qtitan_ui_${HC_WORKSPACE_DPR} PROPERTIES TIMEOUT 270 RUN_SERIAL TRUE)
    endforeach()
endif()
if(TARGET hc_smb_tests)
    add_dependencies(hc_smb_tests hc_wlx_fixture hc_tc_data_fixture hc_dopus_fixture)
endif()
qt_add_executable(hc_hosted_wlx_tests tstHostedWlx.cpp)
target_sources(hc_hosted_wlx_tests PRIVATE ../UniqueLibs/nativeMaterializer.cpp)
target_link_libraries(hc_hosted_wlx_tests PRIVATE hc_viewer_qt Qt6::Test)
target_compile_options(hc_hosted_wlx_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_hosted_wlx_tests hc-plugin-host-x64 hc_wlx_fixture hc_wlx_sdk_fixture)
hc_enable_qt_deployment(hc_hosted_wlx_tests TEST PLUGINS)
if(HC_ENABLE_LOCAL_TEST_TOOLS AND EXISTS "${PROJECT_SOURCE_DIR}/tools/Test-ViewerSurface.ps1")
    foreach(HC_WLX_DPR IN ITEMS 1 1.5)
        add_test(NAME hc.hosted_wlx${HC_WLX_DPR} COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Test-ViewerSurface.ps1"
            -Executable "$<TARGET_FILE:hc_hosted_wlx_tests>" -Dpr ${HC_WLX_DPR})
        set_tests_properties(hc.hosted_wlx${HC_WLX_DPR} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    endforeach()
endif()
qt_add_executable(hc_wlx_selection_tests tstWlxSelection.cpp ../UniqueLibs/nativeMaterializer.cpp)
target_include_directories(hc_wlx_selection_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hc_wlx_selection_tests PRIVATE hc_plugin_viewer_sessions Qt6::Widgets Qt6::Test)
target_compile_options(hc_wlx_selection_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_wlx_selection_tests hc-plugin-host-x64 hc_wlx_fixture hc_wlx_sdk_fixture)
hc_enable_qt_deployment(hc_wlx_selection_tests TEST PLUGINS)
if(HC_ENABLE_LOCAL_TEST_TOOLS AND EXISTS "${PROJECT_SOURCE_DIR}/tools/Test-ViewerSurface.ps1")
    foreach(HC_SELECTION_DPR IN ITEMS 1 1.5)
        add_test(NAME hc.wlx_selection${HC_SELECTION_DPR} COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Test-ViewerSurface.ps1"
            -Executable "$<TARGET_FILE:hc_wlx_selection_tests>" -Dpr ${HC_SELECTION_DPR})
        set_tests_properties(hc.wlx_selection${HC_SELECTION_DPR} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    endforeach()
endif()
qt_add_executable(hc_wcx_tests
    tstWcx.cpp
    ../UniqueLibs/wcxStorage.cpp
    ../UniqueLibs/wcxMutation.cpp
    ../UniqueLibs/nativeMaterializer.cpp
    ../UniqueLibs/storageProviders.cpp
    ../UniqueLibs/nativeWritableStorage.cpp
    ../commonLibs/operations/fileOperation.cpp
    ../commonLibs/operations/archiveOperation.cpp
    ../commonLibs/operations/moveOperation.cpp
    ../commonLibs/operations/jobScheduler.cpp
)
target_include_directories(hc_wcx_tests PRIVATE ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_compile_options(hc_wcx_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_wcx_tests PRIVATE Qt6::Test hc_wcx_backend)
add_dependencies(hc_wcx_tests hc_wcx_fixture)
add_test(NAME hc.wcx COMMAND hc_wcx_tests)
set_tests_properties(hc.wcx PROPERTIES TIMEOUT 45 ENVIRONMENT "HC_WCX_PLUGIN=")
add_custom_command(TARGET hc_wcx_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:hc_wcx_fixture>" "$<TARGET_FILE_DIR:hc_wcx_tests>/hc_wcx_fixture.wcx64" VERBATIM)
get_target_property(HC_WCX_TEST_SOURCES hc_wcx_tests SOURCES)
list(REMOVE_ITEM HC_WCX_TEST_SOURCES tstWcx.cpp)
qt_add_executable(hc_plugin_helper_fixture pluginHelperFixture.cpp)
set_target_properties(hc_plugin_helper_fixture PROPERTIES AUTOMOC OFF AUTOUIC OFF HC_NATIVE_EXECUTABLE ON)
target_compile_options(hc_plugin_helper_fixture PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
qt_add_executable(hc_hosted_wcx_tests tstHostedWcx.cpp ${HC_WCX_TEST_SOURCES})
target_include_directories(hc_hosted_wcx_tests PRIVATE ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_link_libraries(hc_hosted_wcx_tests PRIVATE hc_plugin_process hc_wcx_backend Qt6::Test)
target_compile_options(hc_hosted_wcx_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_hosted_wcx_tests hc-plugin-host-x64 hc_wcx_fixture hc_plugin_helper_fixture hc_plugin_owner_fixture)
add_test(NAME hc.hosted_wcx COMMAND hc_hosted_wcx_tests)
set_tests_properties(hc.hosted_wcx PROPERTIES TIMEOUT 45)
hc_enable_qt_deployment(hc_hosted_wcx_tests TEST)
set(HC_SEVENZIP_CORPUS "${CMAKE_BINARY_DIR}/sevenzip-corpus/corpus.json" CACHE FILEPATH "Generated 7-Zip and libarchive verification corpus")
get_filename_component(HC_SEVENZIP_CORPUS_DIR "${HC_SEVENZIP_CORPUS}" DIRECTORY)
find_program(HC_CORPUS_POWERSHELL NAMES powershell REQUIRED)
add_custom_command(OUTPUT "${HC_SEVENZIP_CORPUS}"
    COMMAND "${HC_CORPUS_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${PROJECT_SOURCE_DIR}/tools/New-SevenZipCorpus.ps1"
        -OutputDirectory "${HC_SEVENZIP_CORPUS_DIR}" -ToolsDirectory "${CMAKE_BINARY_DIR}/sevenzip-tools"
    VERBATIM)
add_custom_target(hc_sevenzip_corpus DEPENDS "${HC_SEVENZIP_CORPUS}")
qt_add_executable(hc_sevenzip_tests tstSevenZip.cpp ${HC_WCX_TEST_SOURCES})
target_include_directories(hc_sevenzip_tests PRIVATE ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_link_libraries(hc_sevenzip_tests PRIVATE hc_builtin_sevenzip hc_sevenzip_backend hc_wcx_backend Qt6::Test)
target_compile_options(hc_sevenzip_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_sevenzip_tests hc-plugin-host-x64 hc_sevenzip_corpus)
add_test(NAME hc.sevenzip COMMAND hc_sevenzip_tests -o "${CMAKE_BINARY_DIR}/sevenzip-$<CONFIG>.txt,txt")
set_tests_properties(hc.sevenzip PROPERTIES TIMEOUT 180 ENVIRONMENT "HC_SEVENZIP_CORPUS=${HC_SEVENZIP_CORPUS}")
hc_enable_qt_deployment(hc_sevenzip_tests TEST)
qt_add_executable(hc_archive_creation_tests tstArchiveCreation.cpp ${HC_WCX_TEST_SOURCES})
target_include_directories(hc_archive_creation_tests PRIVATE ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_link_libraries(hc_archive_creation_tests PRIVATE hc_archive_creation hc_wcx_backend Qt6::Test)
target_compile_options(hc_archive_creation_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_archive_creation_tests hc-plugin-host-x64 hc_dopus_fixture hc_sevenzip_corpus)
add_test(NAME hc.archive_creation COMMAND hc_archive_creation_tests)
set_tests_properties(hc.archive_creation PROPERTIES TIMEOUT 180)
hc_enable_qt_deployment(hc_archive_creation_tests TEST PLUGINS)
option(HC_ARCHIVE_STRESS_TESTS "Run archive acceptance tests exceeding 4 GiB and 65535 entries" OFF)
if(HC_ARCHIVE_STRESS_TESTS)
    add_test(NAME hc.archive_stress COMMAND hc_sevenzip_tests largeFile64Bit zip64EntryCount zip64OffsetMetadata
        -o "${CMAKE_BINARY_DIR}/archive-stress-$<CONFIG>.txt,txt")
    set_tests_properties(hc.archive_stress PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE LABELS "archive;large-data"
        ENVIRONMENT "HC_SEVENZIP_CORPUS=${HC_SEVENZIP_CORPUS};HC_ARCHIVE_STRESS=1")
endif()
if(NOT HC_UNEGG_USE STREQUAL "NONE")
    set(HC_UNEGG_CORPUS "${CMAKE_BINARY_DIR}/unegg-corpus/corpus.json" CACHE FILEPATH "ALZ/EGG verification corpus")
    get_filename_component(HC_UNEGG_CORPUS_DIR "${HC_UNEGG_CORPUS}" DIRECTORY)
    add_custom_command(OUTPUT "${HC_UNEGG_CORPUS}"
        COMMAND "${HC_CORPUS_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${PROJECT_SOURCE_DIR}/tools/New-UnEggCorpus.ps1"
            -OutputDirectory "${HC_UNEGG_CORPUS_DIR}"
        VERBATIM)
    add_custom_target(hc_unegg_corpus DEPENDS "${HC_UNEGG_CORPUS}")
    qt_add_executable(hc_unegg_tests tstUnEgg.cpp ${HC_WCX_TEST_SOURCES})
    target_include_directories(hc_unegg_tests PRIVATE ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander "${CMAKE_BINARY_DIR}/_deps/UnEGG/include")
    target_link_libraries(hc_unegg_tests PRIVATE hc_builtin_unegg hc_unegg_backend hc_wcx_backend Qt6::Test)
    target_compile_definitions(hc_unegg_tests PRIVATE UNICODE _UNICODE)
    target_compile_options(hc_unegg_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    add_dependencies(hc_unegg_tests hc-plugin-host-x64 hc_unegg_corpus)
    add_test(NAME hc.unegg COMMAND hc_unegg_tests -o "${CMAKE_BINARY_DIR}/unegg-$<CONFIG>.txt,txt")
    set_tests_properties(hc.unegg PROPERTIES TIMEOUT 180 ENVIRONMENT "HC_UNEGG_CORPUS=${HC_UNEGG_CORPUS}")
    hc_enable_qt_deployment(hc_unegg_tests TEST)
endif()
qt_add_executable(hc_materialization_tests
    tstMaterialization.cpp
    ../UniqueLibs/nativeMaterializer.cpp
    ../UniqueLibs/nativeMaterializer.hpp
    ../UniqueLibs/storageProviders.cpp
    ../commonLibs/storage/materializedContent.hpp
    ../commonLibs/operations/jobScheduler.cpp
)
target_include_directories(hc_materialization_tests PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/commonLibs
    ${PROJECT_SOURCE_DIR}/HyperCommander
)
target_compile_options(hc_materialization_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>
)
target_link_libraries(hc_materialization_tests PRIVATE Qt6::Test)
add_test(NAME hc.materialization COMMAND hc_materialization_tests)
set_tests_properties(hc.materialization PROPERTIES TIMEOUT 45)
qt_add_executable(hc_file_lock_tests
    tstFileLocks.cpp
    ../UniqueLibs/nativeWritableStorage.cpp
    ../UniqueLibs/storageProviders.cpp
    ../commonLibs/operations/fileOperation.cpp
)
target_include_directories(hc_file_lock_tests PRIVATE
    ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_link_libraries(hc_file_lock_tests PRIVATE hc_file_locks hc_viewer_core Qt6::Test advapi32 shell32 ole32)
target_compile_options(hc_file_lock_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.file_locks COMMAND hc_file_lock_tests)
set_tests_properties(hc.file_locks PROPERTIES TIMEOUT 180)
hc_enable_qt_deployment(hc_file_lock_tests TEST)

qt_add_executable(hc_write_tests
    tstWrites.cpp
    ../UniqueLibs/nativeFileElevation.cpp
    ../UniqueLibs/nativeWritableStorage.cpp
    ../UniqueLibs/nativeWritableStorage.hpp
    ../UniqueLibs/storageProviders.cpp
    ../commonLibs/storage/writableStorage.hpp
    ../commonLibs/operations/fileOperation.cpp
    ../commonLibs/operations/fileOperation.hpp
    ../commonLibs/operations/jobScheduler.cpp
)
target_include_directories(hc_write_tests PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/commonLibs
    ${PROJECT_SOURCE_DIR}/HyperCommander
)
target_compile_options(hc_write_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_write_tests PRIVATE Qt6::Test Advapi32 Shell32 Ole32)
add_test(NAME hc.writes COMMAND hc_write_tests)
set_tests_properties(hc.writes PROPERTIES TIMEOUT 45 ENVIRONMENT "HC_MUTATION_ROOT=")
qt_add_executable(hc_attribute_tests tstFileAttributes.cpp
    ../UniqueLibs/nativeFileElevation.cpp ../UniqueLibs/nativeWritableStorage.cpp
    ../UniqueLibs/storageProviders.cpp ../commonLibs/operations/fileOperation.cpp
    ../commonLibs/operations/jobScheduler.cpp)
target_include_directories(hc_attribute_tests PRIVATE ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_compile_options(hc_attribute_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_attribute_tests PRIVATE Qt6::Test Advapi32 Shell32 Ole32)
add_test(NAME hc.attributes COMMAND hc_attribute_tests)
set_tests_properties(hc.attributes PROPERTIES TIMEOUT 90)
hc_enable_qt_deployment(hc_attribute_tests TEST)
qt_add_executable(hc_attribute_ui_tests tstFileAttributesUi.cpp
    ../HyperCommander/UIs/fileAttributesDialog.cpp ../HyperCommander/UIs/dialogShell.cpp ../HyperCommander/UIs/nativePathCompletion.cpp
    ../UniqueLibs/nativeFileElevation.cpp ../UniqueLibs/nativeWritableStorage.cpp
    ../UniqueLibs/storageProviders.cpp ../commonLibs/operations/fileOperation.cpp
    ../commonLibs/operations/jobScheduler.cpp)
target_include_directories(hc_attribute_ui_tests PRIVATE ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_compile_options(hc_attribute_ui_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_attribute_ui_tests PRIVATE Qt6::Test Qt6::Concurrent Fm::widgets Advapi32 Shell32 Ole32)
add_test(NAME hc.attributes_ui COMMAND hc_attribute_ui_tests)
set_tests_properties(hc.attributes_ui PROPERTIES TIMEOUT 45)
hc_enable_qt_deployment(hc_attribute_ui_tests TEST PLUGINS)
qt_add_executable(hc_job_tests tstJobs.cpp ../commonLibs/operations/jobScheduler.cpp ../commonLibs/operations/jobScheduler.hpp)
target_include_directories(hc_job_tests PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_options(hc_job_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_job_tests PRIVATE Qt6::Test)
add_test(NAME hc.jobs COMMAND hc_job_tests)
set_tests_properties(hc.jobs PROPERTIES TIMEOUT 30)
if(HC_ENABLE_LOCAL_TEST_TOOLS AND EXISTS "${PROJECT_SOURCE_DIR}/tools/Test-QtitanCompatibility.ps1")
    add_test(NAME hc.qtitan COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${PROJECT_SOURCE_DIR}/tools/Test-QtitanCompatibility.ps1"
        -GeneratedRoot "${CMAKE_BINARY_DIR}/watercolor/third_party/QtitanDataGrid/src")
    set_tests_properties(hc.qtitan PROPERTIES TIMEOUT 30)
endif()

qt_add_executable(hc_column_tests tstColumnWidths.cpp
    ../commonLibs/columns/columnContracts.hpp
    ../commonLibs/columns/builtInColumns.hpp
    ../commonLibs/columns/columnProfileStorage.hpp
    ../commonLibs/view/columnWidthCodec.hpp ../commonLibs/view/columnWidthPolicy.hpp ../commonLibs/view/listIconSize.hpp ../UniqueLibs/columnWidthSettings.hpp)
target_include_directories(hc_column_tests PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_options(hc_column_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_column_tests PRIVATE hc_script_runtime Qt6::Test Qt6::Concurrent)
add_test(NAME hc.columns COMMAND hc_column_tests)
set_tests_properties(hc.columns PROPERTIES TIMEOUT 30)

qt_add_executable(hc_command_tests tstCommands.cpp ../commonLibs/commands/commandBus.hpp)
target_include_directories(hc_command_tests PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_options(hc_command_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_command_tests PRIVATE Qt6::Test)
add_test(NAME hc.commands COMMAND hc_command_tests)
set_tests_properties(hc.commands PROPERTIES
    TIMEOUT 30
)

qt_add_executable(hc_storage_tests tstStorage.cpp memoryStorage.hpp
    ../UniqueLibs/storageProviders.cpp
    ../UniqueLibs/storageProviders.hpp
    ../commonLibs/storage/storage.hpp)
target_include_directories(hc_storage_tests PRIVATE ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_compile_options(hc_storage_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>
)
target_link_libraries(hc_storage_tests PRIVATE Qt6::Test)
add_test(NAME hc.storage COMMAND hc_storage_tests)
set_tests_properties(hc.storage PROPERTIES
    TIMEOUT 30
)

qt_add_executable(hc_path_tests
    tstPath.cpp
    stdafx.h
    ../commonLibs/cmnPath.cpp
)
target_include_directories(hc_path_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/HyperCommander
    ${PROJECT_SOURCE_DIR}/commonLibs
)
target_compile_definitions(hc_path_tests PRIVATE UNICODE _UNICODE)
target_compile_options(hc_path_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>
)
target_link_libraries(hc_path_tests PRIVATE Qt6::Test advapi32)
add_test(NAME hc.path COMMAND hc_path_tests)
set_tests_properties(hc.path PROPERTIES
    TIMEOUT 30
)

if(HC_ENABLE_LOCAL_TEST_TOOLS AND EXISTS "${PROJECT_SOURCE_DIR}/tools/Test-SourceInventory.ps1")
    add_test(NAME hc.sources
        COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Test-SourceInventory.ps1"
            -SourceRoot "${PROJECT_SOURCE_DIR}"
            -SourceList "${CMAKE_BINARY_DIR}/hc-product-sources.txt"
    )
    set_tests_properties(hc.sources PROPERTIES TIMEOUT 30)
endif()
if(HC_ENABLE_LOCAL_TEST_TOOLS AND EXISTS "${PROJECT_SOURCE_DIR}/tools/Test-AppStartup.ps1")
    add_test(NAME hc.startup
        COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Test-AppStartup.ps1"
            -Executable "$<TARGET_FILE:HyperCommander>"
            -QtBin "$<TARGET_FILE_DIR:Qt6::Core>"
            -OutputRoot "${CMAKE_BINARY_DIR}/startup/$<CONFIG>"
    )
    set_tests_properties(hc.startup PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()

if(TARGET hc_smb_tests AND HC_ENABLE_LOCAL_TEST_TOOLS AND EXISTS "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1")
    foreach(HC_ISO_DPR IN ITEMS 1 1.5)
        add_test(NAME hc.iso_ui_${HC_ISO_DPR}
            COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
                -Executable "$<TARGET_FILE:hc_smb_tests>" -IsoOnly -Dpr ${HC_ISO_DPR})
        set_tests_properties(hc.iso_ui_${HC_ISO_DPR} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    endforeach()
    add_test(NAME hc.settings_ui
        COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
            -Executable "$<TARGET_FILE:hc_smb_tests>" -SettingsOnly -Dpr 1 -TimeoutSeconds 60)
    set_tests_properties(hc.settings_ui PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    add_test(NAME hc.archive_ui
        COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
            -Executable "$<TARGET_FILE:hc_smb_tests>" -ArchiveOnly -Dpr 1 -TimeoutSeconds 120)
    set_tests_properties(hc.archive_ui PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE ENVIRONMENT "HC_SEVENZIP_CORPUS=${HC_SEVENZIP_CORPUS};HC_UNEGG_CORPUS=${HC_UNEGG_CORPUS}")
    add_test(NAME hc.data_plugin_ui
        COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
            -Executable "$<TARGET_FILE:hc_smb_tests>" -DataOnly -Dpr 1)
    set_tests_properties(hc.data_plugin_ui PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    add_test(NAME hc.network_startup
        COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
            -Executable "$<TARGET_FILE:hc_smb_tests>" -NetworkStartupOnly -Dpr 1)
    set_tests_properties(hc.network_startup PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()

if(HC_ENABLE_LOCAL_TEST_TOOLS AND EXISTS "${PROJECT_SOURCE_DIR}/tools/Test-DeployedRuntime.ps1")
    add_test(NAME hc.deployment
        COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Test-DeployedRuntime.ps1"
            -Executable "$<TARGET_FILE:HyperCommander>"
            -TestExecutables "$<TARGET_FILE:hc_settings_tests>|$<TARGET_FILE:hc_automation_tests>|$<TARGET_FILE:hc_search_tests>|$<TARGET_FILE:hc_workspace_tests>|$<TARGET_FILE:hc_path_tests>|$<TARGET_FILE:hc_storage_tests>|$<TARGET_FILE:hc_command_tests>|$<TARGET_FILE:hc_column_tests>|$<TARGET_FILE:hc_job_tests>|$<TARGET_FILE:hc_write_tests>|$<TARGET_FILE:hc_materialization_tests>|$<TARGET_FILE:hc_wcx_tests>|$<TARGET_FILE:hc_hosted_wcx_tests>|$<TARGET_FILE:hc_viewer_tests>|$<TARGET_FILE:hc_viewer_surface_tests>|$<TARGET_FILE:hc_plugin_tests>|$<TARGET_FILE:hc_plugin_process_tests>|$<TARGET_FILE:hc_hosted_wlx_tests>|$<TARGET_FILE:hc_wlx_detection_tests>|$<TARGET_FILE:hc_wlx_selection_tests>|$<TARGET_FILE:hc_hosted_data_tests>|$<TARGET_FILE:hc_hosted_dopus_tests>|$<TARGET_FILE:hc_dopus_viewer_tests>|$<TARGET_FILE:hc_iso_engine_tests>|$<TARGET_FILE:hc_iso_provider_tests>|$<TARGET_FILE:hc_iso_wcx_tests>|$<TARGET_FILE:hc_iso_job_tests>"
            -OutputRoot "${CMAKE_BINARY_DIR}/deployment/$<CONFIG>"
    )
    # This runs the product and every deployed test executable sequentially. The
    # nested archive suites bring the measured Debug total to 87 seconds; keep
    # process-level limits while allowing deployment/copy and startup overhead.
    set_tests_properties(hc.deployment PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()

# CTest comes from this configure's CMake installation, never an unrelated PATH entry.
option(HC_LIVE_SMB_ARCHIVE_TESTS "Validate nested archives on HC_SMB_SERVER/HC_SMB_SHARE" OFF)
if(HC_LIVE_SMB_ARCHIVE_TESTS)
    if(NOT TARGET hc_smb_tests OR HC_UNEGG_USE STREQUAL "NONE")
        message(FATAL_ERROR "Live archive matrix requires HC_BUILD_SMB_TESTS and an approved HC_UNEGG_USE")
    endif()
    add_test(NAME hc.archive_nested_smb COMMAND hc_smb_tests builtinNestedArchives passwordAndNestedArchivesUi
        missingDirectoryAndRecovery cancelledAndLateNavigation -o "${CMAKE_BINARY_DIR}/archive-smb-$<CONFIG>.txt,txt")
    set_tests_properties(hc.archive_nested_smb PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE LABELS "archive;smb;live"
        ENVIRONMENT "HC_UI_LOCAL_ROOT=;HC_SEVENZIP_CORPUS=${HC_SEVENZIP_CORPUS};HC_UNEGG_CORPUS=${HC_UNEGG_CORPUS}")
endif()

add_custom_target(hc_check
    COMMAND "${CMAKE_CTEST_COMMAND}" -C "$<CONFIG>" --output-on-failure --no-tests=error
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS hc_dopus_viewer_tests hc_hosted_dopus_tests hc_hosted_data_tests hc_wlx_selection_tests hc_wlx_detection_tests hc_path_tests hc_storage_tests hc_command_tests hc_column_tests hc_job_tests hc_write_tests hc_materialization_tests hc_wcx_tests hc_hosted_wcx_tests hc_hosted_wlx_tests hc_viewer_tests hc_viewer_surface_tests hc_plugin_tests hc_plugin_process_tests HyperCommander
    USES_TERMINAL
    VERBATIM
)
add_dependencies(hc_check hc_iso_engine_tests hc_iso_provider_tests hc_iso_wcx_tests hc_iso_job_tests)
add_dependencies(hc_check hc_archive_creation_tests hc_plugin_management_tests)
add_dependencies(hc_check hc_operation_recovery_tests)
add_dependencies(hc_check hc_file_lock_tests)
qt_add_executable(hc_file_lock_ui_tests tstFileLockUi.cpp
    ../HyperCommander/UIs/fileLockDialog.cpp
    ../HyperCommander/UIs/dialogShell.cpp
    ../HyperCommander/UIs/nativePathCompletion.cpp
)
target_include_directories(hc_file_lock_ui_tests PRIVATE
    ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_link_libraries(hc_file_lock_ui_tests PRIVATE hc_file_locks hc_viewer_qt Qt6::Test Qt6::Concurrent Fm::widgets advapi32 shell32)
target_compile_options(hc_file_lock_ui_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.file_lock_ui COMMAND hc_file_lock_ui_tests)
set_tests_properties(hc.file_lock_ui PROPERTIES TIMEOUT 120)
hc_enable_qt_deployment(hc_file_lock_ui_tests TEST PLUGINS)
add_dependencies(hc_check hc_file_lock_ui_tests)
add_dependencies(hc_check hc_workspace_tests hc_search_tests hc_automation_tests hc_script_api_tests)
add_dependencies(hc_check hc_lua_tests hc_sevenzip_tests)
if(TARGET hc_unegg_tests)
    add_dependencies(hc_check hc_unegg_tests)
endif()
if(TARGET hc_dopus_sdk_abi_tests)
    add_dependencies(hc_check hc_dopus_sdk_abi_tests)
endif()

if(TARGET hc_smb_tests AND HC_ENABLE_LOCAL_TEST_TOOLS AND EXISTS "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1")
    add_dependencies(hc_check hc_smb_tests)
endif()

foreach(HC_TEST_TARGET IN ITEMS hc_path_tests hc_storage_tests hc_command_tests hc_column_tests hc_job_tests hc_write_tests hc_materialization_tests hc_wcx_tests)
    hc_enable_qt_deployment(${HC_TEST_TARGET} TEST)
endforeach()


qt_add_executable(hc_settings_tests tstSettings.cpp)
target_include_directories(hc_settings_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/commonLibs" "${PROJECT_SOURCE_DIR}/HyperCommander")
target_link_libraries(hc_settings_tests PRIVATE Qt6::Test)
target_compile_options(hc_settings_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.settings COMMAND hc_settings_tests)
set_tests_properties(hc.settings PROPERTIES TIMEOUT 30)
hc_enable_qt_deployment(hc_settings_tests TEST)
add_dependencies(hc_check hc_settings_tests)

# Product-owned shell: no Fm::dialogs or Fm::settings dependency.
qt_add_executable(hc_command_ui_tests tstCommandUi.cpp
    ../UniqueLibs/commandUiConfig.hpp ../UniqueLibs/commandUiWidgets.hpp ../UniqueLibs/userMenu.hpp
    ../HyperCommander/HyperCommander.qrc)
target_include_directories(hc_command_ui_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(hc_command_ui_tests PRIVATE Qt6::Test Fm::widgets)
target_compile_options(hc_command_ui_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.command_ui COMMAND hc_command_ui_tests)
set_tests_properties(hc.command_ui PROPERTIES TIMEOUT 30)
hc_enable_qt_deployment(hc_command_ui_tests TEST PLUGINS)
add_dependencies(hc_check hc_command_ui_tests)

qt_add_executable(hc_dialog_foundation_tests
    ../HyperCommander/UIs/renameHistoryDialog.cpp
    tstDialogFoundation.cpp
    ../HyperCommander/UIs/fileToolsDialogs.cpp
    ../HyperCommander/UIs/fileToolsDialogs.hpp
    ../HyperCommander/UIs/dialogShell.cpp
    ../HyperCommander/UIs/dialogShell.hpp
    ../HyperCommander/UIs/nativePathCompletion.cpp
    ../HyperCommander/UIs/settingsDraft.hpp
)
target_include_directories(hc_dialog_foundation_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/commonLibs" "${PROJECT_SOURCE_DIR}/HyperCommander")
target_link_libraries(hc_dialog_foundation_tests PRIVATE hc_viewer_qt Qt6::Test Qt6::Concurrent Fm::widgets Advapi32 Shell32)
target_compile_options(hc_dialog_foundation_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.dialog_foundation COMMAND hc_dialog_foundation_tests)
set_tests_properties(hc.dialog_foundation PROPERTIES TIMEOUT 60)
hc_enable_qt_deployment(hc_dialog_foundation_tests TEST PLUGINS)
add_dependencies(hc_check hc_dialog_foundation_tests)

qt_add_executable(hc_document_p0_tests document/tstDocumentP0.cpp)
if(HC_RHWP_PRODUCT_RUNTIME_DIRECTORY AND HC_RHWP_APPROVED_INVENTORY_SHA256)
    target_compile_definitions(hc_document_p0_tests PRIVATE HC_TEST_RHWP_HOST_RUNTIME=1)
endif()
target_link_libraries(hc_document_p0_tests PRIVATE hc_document_widgets hc_plugin_process Qt6::Test Qt6::Concurrent)
target_compile_options(hc_document_p0_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_document_p0_tests hc_hyperviewer_package hc-plugin-host-x64)
hc_enable_qt_deployment(hc_document_p0_tests TEST PLUGINS)
add_test(NAME hc.document.p0 COMMAND hc_document_p0_tests -o "${CMAKE_BINARY_DIR}/document-p0-$<CONFIG>.txt,txt")
set_tests_properties(hc.document.p0 PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

foreach(HC_DOCUMENT_DPR IN ITEMS 1 1.5 2)
    add_test(NAME hc.document.p0_dpr${HC_DOCUMENT_DPR} COMMAND "${HC_POWERSHELL}"
        -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/document/Run-DocumentP0.ps1"
        -Executable "$<TARGET_FILE:hc_document_p0_tests>" -Dpr "${HC_DOCUMENT_DPR}"
        -Log "${CMAKE_BINARY_DIR}/document-p0-dpr${HC_DOCUMENT_DPR}-$<CONFIG>.txt")
    set_tests_properties(hc.document.p0_dpr${HC_DOCUMENT_DPR} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endforeach()
add_dependencies(hc_check hc_document_p0_tests)

qt_add_executable(hc_document_p1_tests document/tstDocumentP1.cpp)
target_link_libraries(hc_document_p1_tests PRIVATE hc_document_widgets Qt6::Test Qt6::Concurrent)
target_compile_options(hc_document_p1_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_document_p1_tests hc_hyperviewer_package)
hc_enable_qt_deployment(hc_document_p1_tests TEST PLUGINS)
add_test(NAME hc.document.p1 COMMAND hc_document_p1_tests -o "${CMAKE_BINARY_DIR}/document-p1-$<CONFIG>.txt,txt")
set_tests_properties(hc.document.p1 PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
add_dependencies(hc_check hc_document_p1_tests)

foreach(HC_DOCUMENT_DPR IN ITEMS 1 1.5 2)
    add_test(NAME hc.document.p1_dpr${HC_DOCUMENT_DPR} COMMAND "${HC_POWERSHELL}"
        -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/document/Run-DocumentP0.ps1"
        -Executable "$<TARGET_FILE:hc_document_p1_tests>" -Dpr "${HC_DOCUMENT_DPR}"
        -Log "${CMAKE_BINARY_DIR}/document-p1-dpr${HC_DOCUMENT_DPR}-$<CONFIG>.txt")
    set_tests_properties(hc.document.p1_dpr${HC_DOCUMENT_DPR} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endforeach()

hc_deploy_document_engine(hc_document_p0_tests)
hc_deploy_document_engine(hc_document_p1_tests)

qt_add_executable(hc_document_p2_tests document/tstDocumentP2.cpp)
target_link_libraries(hc_document_p2_tests PRIVATE hc_document_widgets Qt6::Test Qt6::Concurrent)
target_compile_options(hc_document_p2_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_document_p2_tests hc_hyperviewer_package)
hc_enable_qt_deployment(hc_document_p2_tests TEST PLUGINS)
hc_deploy_document_engine(hc_document_p2_tests)
add_dependencies(hc_check hc_document_p2_tests)
add_test(NAME hc.document.p2 COMMAND hc_document_p2_tests -o "${CMAKE_BINARY_DIR}/document-p2-$<CONFIG>.txt,txt")
set_tests_properties(hc.document.p2 PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
foreach(HC_DOCUMENT_DPR IN ITEMS 1 1.5 2)
    add_test(NAME hc.document.p2_dpr${HC_DOCUMENT_DPR} COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/document/Run-DocumentP0.ps1" -Executable "$<TARGET_FILE:hc_document_p2_tests>" -Dpr "${HC_DOCUMENT_DPR}" -Log "${CMAKE_BINARY_DIR}/document-p2-dpr${HC_DOCUMENT_DPR}-$<CONFIG>.txt")
    set_tests_properties(hc.document.p2_dpr${HC_DOCUMENT_DPR} PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
endforeach()

qt_add_executable(hc_document_p3_tests document/tstDocumentP3.cpp)
target_link_libraries(hc_document_p3_tests PRIVATE hc_document_widgets hc_plugin_viewer_qt hc_viewer_qt Qt6::Test Qt6::Concurrent)
target_compile_options(hc_document_p3_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_compile_definitions(hc_document_p3_tests PRIVATE HC_DOCUMENT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/document/fixtures")
add_dependencies(hc_document_p3_tests hc_hyperviewer_package hc-plugin-host-x64 hc_sevenzip_corpus)
hc_enable_qt_deployment(hc_document_p3_tests TEST PLUGINS)
hc_deploy_document_engine(hc_document_p3_tests)
add_dependencies(hc_check hc_document_p3_tests)
add_test(NAME hc.document.p3 COMMAND hc_document_p3_tests -o "${CMAKE_BINARY_DIR}/document-p3-$<CONFIG>.txt,txt")
set_tests_properties(hc.document.p3 PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
foreach(HC_DOCUMENT_DPR IN ITEMS 1 1.5 2)
    add_test(NAME hc.document.p3_dpr${HC_DOCUMENT_DPR} COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/document/Run-DocumentP0.ps1" -Executable "$<TARGET_FILE:hc_document_p3_tests>" -Dpr "${HC_DOCUMENT_DPR}" -Log "${CMAKE_BINARY_DIR}/document-p3-dpr${HC_DOCUMENT_DPR}-$<CONFIG>.txt")
    set_tests_properties(hc.document.p3_dpr${HC_DOCUMENT_DPR} PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
endforeach()

qt_add_executable(hc_document_p4_tests document/tstDocumentP4.cpp)
target_link_libraries(hc_document_p4_tests PRIVATE hc_document_open hc_document_broker Qt6::Test Qt6::Concurrent)
target_compile_options(hc_document_p4_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_document_p4_tests hc_hyperviewer_package hc-plugin-host-x64)
hc_enable_qt_deployment(hc_document_p4_tests TEST PLUGINS)
add_dependencies(hc_check hc_document_p4_tests)
add_test(NAME hc.document.p4 COMMAND hc_document_p4_tests -o "${CMAKE_BINARY_DIR}/document-p4-$<CONFIG>.txt,txt")
set_tests_properties(hc.document.p4 PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

if(TARGET hc_smb_tests)
    target_link_libraries(hc_smb_tests PRIVATE hc_pdfium_runtime Qt6::PrintSupport)
    add_dependencies(hc_smb_tests hc_hyperviewer_package)
    foreach(HC_DOCUMENT_DPR IN ITEMS 1 1.5)
        add_test(NAME hc.document.commands_dpr${HC_DOCUMENT_DPR} COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/document/Run-DocumentCommands.ps1"
            -Executable "$<TARGET_FILE:hc_smb_tests>" -Dpr "${HC_DOCUMENT_DPR}"
            -Log "${CMAKE_BINARY_DIR}/document-commands-dpr${HC_DOCUMENT_DPR}-$<CONFIG>.txt")
        set_tests_properties(hc.document.commands_dpr${HC_DOCUMENT_DPR} PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endforeach()
endif()

add_test(NAME hc.document.p5 COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/document/Run-DocumentP5.ps1"
    -BuildDirectory "${CMAKE_BINARY_DIR}" -Configuration "$<CONFIG>"
    -ReportDirectory "${CMAKE_BINARY_DIR}/document-p5-$<CONFIG>")
set_tests_properties(hc.document.p5 PROPERTIES TIMEOUT 360 RUN_SERIAL TRUE)

qt_add_executable(hc_web_process_fixture WIN32 document/webProcessFixture.cpp)
target_include_directories(hc_web_process_fixture PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(hc_web_process_fixture PRIVATE shell32)
set_target_properties(hc_web_process_fixture PROPERTIES HC_NATIVE_EXECUTABLE ON)
target_compile_options(hc_web_process_fixture PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

qt_add_executable(hc_document_web_process_tests document/tstDocumentWebProcess.cpp)
target_link_libraries(hc_document_web_process_tests PRIVATE hc_document_broker Qt6::Test)
target_compile_options(hc_document_web_process_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_document_web_process_tests hc_web_process_fixture)
hc_enable_qt_deployment(hc_document_web_process_tests TEST)
add_dependencies(hc_check hc_document_web_process_tests)
add_test(NAME hc.document.web_process COMMAND hc_document_web_process_tests
    -o "${CMAKE_CURRENT_BINARY_DIR}/web-process-$<CONFIG>.xml,xml" -o "-,txt")
set_tests_properties(hc.document.web_process PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

qt_add_executable(hc_hwp_runtime_sha_tests document/tstHwpRuntimeSha256.cpp)
target_include_directories(hc_hwp_runtime_sha_tests PRIVATE "${PROJECT_SOURCE_DIR}/UniqueLibs/document")
target_link_libraries(hc_hwp_runtime_sha_tests PRIVATE hc_document_broker Qt6::Test)
target_compile_options(hc_hwp_runtime_sha_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc_hwp_runtime_sha_tests TEST)
add_dependencies(hc_check hc_hwp_runtime_sha_tests)
add_test(NAME hc.document.hwp_runtime_sha COMMAND hc_hwp_runtime_sha_tests
    -o "${CMAKE_CURRENT_BINARY_DIR}/hwp-runtime-sha-$<CONFIG>.xml,xml" -o "-,txt")
set_tests_properties(hc.document.hwp_runtime_sha PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

qt_add_executable(hc_document_web_peer_tests document/tstDocumentWebPeer.cpp
    ../UniqueLibs/document/documentWebPeer.cpp ../UniqueLibs/document/documentWebPeer.hpp)
target_include_directories(hc_document_web_peer_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(hc_document_web_peer_tests PRIVATE Qt6::Test Qt6::Network)
target_compile_options(hc_document_web_peer_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc_document_web_peer_tests TEST)
add_dependencies(hc_document_web_peer_tests hc_web_process_fixture)
add_dependencies(hc_check hc_document_web_peer_tests)
add_test(NAME hc.document.web_peer COMMAND hc_document_web_peer_tests)
set_tests_properties(hc.document.web_peer PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

qt_add_executable(hc_document_async_tests document/tstDocumentAsync.cpp)
target_link_libraries(hc_document_async_tests PRIVATE hc_document_core)
target_compile_options(hc_document_async_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_check hc_document_async_tests)
add_test(NAME hc.document.async_contract COMMAND hc_document_async_tests)
set_tests_properties(hc.document.async_contract PROPERTIES TIMEOUT 30)

qt_add_executable(hc_hwp_process_tests document/tstHwpProcessBackend.cpp)
target_link_libraries(hc_hwp_process_tests PRIVATE hc_document_sdi Qt6::Test)
target_compile_options(hc_hwp_process_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_hwp_process_tests hc_web_process_fixture)
hc_enable_qt_deployment(hc_hwp_process_tests TEST PLUGINS)
add_dependencies(hc_check hc_hwp_process_tests)
add_test(NAME hc.document.hwp_process COMMAND hc_hwp_process_tests)
set_tests_properties(hc.document.hwp_process PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

qt_add_executable(hc_hwp_sdi_tests document/tstHwpSdiWindow.cpp)
target_link_libraries(hc_hwp_sdi_tests PRIVATE hc_document_sdi hc_document_widgets Qt6::Test)
target_compile_options(hc_hwp_sdi_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc_hwp_sdi_tests TEST PLUGINS)
add_dependencies(hc_check hc_hwp_sdi_tests)
add_test(NAME hc.document.hwp_sdi COMMAND hc_hwp_sdi_tests)
set_tests_properties(hc.document.hwp_sdi PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

if(HC_ENABLE_RHWP)
    qt_add_executable(hc_hwp_load_fixture WIN32 document/hwpLoadFixture.cpp
        ../UniqueLibs/document/documentWebPeer.cpp ../UniqueLibs/document/documentWebPeer.hpp)
    target_link_libraries(hc_hwp_load_fixture PRIVATE hc_document_web Qt6::Network)
    target_compile_options(hc_hwp_load_fixture PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    set_target_properties(hc_hwp_load_fixture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/rhwp-fixture/$<CONFIG>")
    hc_enable_qt_deployment(hc_hwp_load_fixture TEST PLUGINS)
    qt_add_executable(hc_hwp_bundle_tests document/tstHwpBundle.cpp)
    hc_embed_rhwp_bundle(hc_hwp_bundle_tests)
    add_dependencies(hc_hwp_bundle_tests hc_web_process_fixture hc_hwp_load_fixture)
    target_compile_definitions(hc_hwp_bundle_tests PRIVATE HC_RETIREMENT_FIXTURE="$<TARGET_FILE:hc_web_process_fixture>" HC_LOAD_FIXTURE_DIR="$<TARGET_FILE_DIR:hc_hwp_load_fixture>")
    target_link_libraries(hc_hwp_bundle_tests PRIVATE hc_document_web Qt6::WebEngineWidgets Qt6::CorePrivate Qt6::Test)
    target_compile_options(hc_hwp_bundle_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    # Keep WebEngine runtime and resources outside the ordinary product inventory.
    set_target_properties(hc_hwp_bundle_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/rhwp-tests/$<CONFIG>")
    hc_enable_qt_deployment(hc_hwp_bundle_tests TEST PLUGINS)
    add_dependencies(hc_check hc_hwp_bundle_tests)
    add_test(NAME hc.document.hwp_bundle COMMAND hc_hwp_bundle_tests)
    set_tests_properties(hc.document.hwp_bundle PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    qt_add_executable(hc_hwp_runtime_tests document/tstHwpRuntime.cpp)
    target_link_libraries(hc_hwp_runtime_tests PRIVATE hc_document_broker hc_document_sdi Qt6::Test)
    target_compile_options(hc_hwp_runtime_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    target_compile_definitions(hc_hwp_runtime_tests PRIVATE HC_TEST_RHWP_RUNTIME="$<TARGET_FILE_DIR:HyperEditorWeb>")
    add_dependencies(hc_hwp_runtime_tests HyperEditorWeb)
    hc_enable_qt_deployment(hc_hwp_runtime_tests TEST PLUGINS)
    add_dependencies(hc_check hc_hwp_runtime_tests)
    if(HC_RHWP_APPROVED_MANIFEST_SHA256 STREQUAL "")
        add_test(NAME hc.document.hwp_runtime COMMAND hc_hwp_runtime_tests)
        set_tests_properties(hc.document.hwp_runtime PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    endif()
endif()

qt_add_executable(hc_hwp_export_tests document/tstHwpExport.cpp)
target_link_libraries(hc_hwp_export_tests PRIVATE hc_document_widgets Qt6::Test)
target_compile_options(hc_hwp_export_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc_hwp_export_tests TEST)
add_dependencies(hc_check hc_hwp_export_tests)
add_test(NAME hc.document.hwp_export COMMAND hc_hwp_export_tests)
set_tests_properties(hc.document.hwp_export PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

qt_add_executable(hc_hwp_input_tests document/tstHwpInputInspection.cpp)
target_link_libraries(hc_hwp_input_tests PRIVATE hc_document_broker Qt6::CorePrivate Qt6::Test)
target_compile_options(hc_hwp_input_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc_hwp_input_tests TEST)
add_dependencies(hc_check hc_hwp_input_tests)
add_test(NAME hc.document.hwp_input COMMAND hc_hwp_input_tests)
set_tests_properties(hc.document.hwp_input PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

qt_add_executable(hc_hwp_recovery_tests document/tstHwpRecovery.cpp)
target_link_libraries(hc_hwp_recovery_tests PRIVATE hc_document_widgets Qt6::Test)
target_compile_options(hc_hwp_recovery_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc_hwp_recovery_tests TEST)
add_dependencies(hc_check hc_hwp_recovery_tests)
add_test(NAME hc.document.hwp_recovery COMMAND hc_hwp_recovery_tests)
set_tests_properties(hc.document.hwp_recovery PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

qt_add_executable(hc_hwp_print_tests document/tstHwpPrint.cpp)
target_link_libraries(hc_hwp_print_tests PRIVATE hc_document_broker Qt6::Widgets Qt6::Test)
target_compile_options(hc_hwp_print_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc_hwp_print_tests TEST PLUGINS)
add_dependencies(hc_check hc_hwp_print_tests)
add_test(NAME hc.document.hwp_print COMMAND hc_hwp_print_tests)
set_tests_properties(hc.document.hwp_print PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

# MultiRoot platform and wrap-boundary regression suite (P0).
qt_add_executable(hc_scintilla_platform_tests document/tstScintillaPlatform.cpp)
target_include_directories(hc_scintilla_platform_tests PRIVATE "${PROJECT_SOURCE_DIR}/HyperEditor/src")
target_link_libraries(hc_scintilla_platform_tests PRIVATE hc_scintilla Qt6::Test psapi)
target_compile_options(hc_scintilla_platform_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc_scintilla_platform_tests TEST PLUGINS)
add_dependencies(hc_check hc_scintilla_platform_tests)
add_test(NAME hc.document.scintilla_platform COMMAND hc_scintilla_platform_tests -o "${CMAKE_BINARY_DIR}/scintilla-platform-$<CONFIG>.txt,txt")
set_tests_properties(hc.document.scintilla_platform PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

qt_add_executable(hc_lexilla_compatibility_tests document/tstLexillaCompatibility.cpp)
target_link_libraries(hc_lexilla_compatibility_tests PRIVATE hc_scintilla hc_lexilla Qt6::Test)
target_compile_definitions(hc_lexilla_compatibility_tests PRIVATE HC_LEXER_REGISTRY="${PROJECT_SOURCE_DIR}/HyperEditor/resources/text/lexers.json")
target_compile_options(hc_lexilla_compatibility_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc_lexilla_compatibility_tests TEST PLUGINS)
add_dependencies(hc_check hc_lexilla_compatibility_tests)
add_test(NAME hc.document.lexilla_compatibility COMMAND hc_lexilla_compatibility_tests -o "${CMAKE_BINARY_DIR}/lexilla-compatibility-$<CONFIG>.txt,txt")
set_tests_properties(hc.document.lexilla_compatibility PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

qt_add_executable(hc_document_preferences_tests document/tstDocumentPreferences.cpp)
target_link_libraries(hc_document_preferences_tests PRIVATE hc_document_widgets Qt6::Test)
target_compile_options(hc_document_preferences_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(hc_document_preferences_tests TEST PLUGINS)
add_dependencies(hc_check hc_document_preferences_tests)
add_test(NAME hc.document.preferences COMMAND hc_document_preferences_tests -o "${CMAKE_BINARY_DIR}/document-preferences-$<CONFIG>.txt,txt")
set_tests_properties(hc.document.preferences PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
qt_add_executable(hc_image_reading_tests document/tstImageReading.cpp)
target_link_libraries(hc_image_reading_tests PRIVATE hc_image_archive_broker hc_document_widgets Qt6::Test Qt6::Concurrent Qt6::GuiPrivate)
target_compile_options(hc_image_reading_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_compile_definitions(hc_image_reading_tests PRIVATE HC_IMAGE_CODEC_SOURCES="${HC_IMAGE_CODEC_ROOT}/sources")
add_dependencies(hc_image_reading_tests hc-plugin-host-x64 hc_sevenzip_package)
hc_enable_qt_deployment(hc_image_reading_tests TEST PLUGINS)
hc_deploy_document_engine(hc_image_reading_tests)
add_dependencies(hc_check hc_image_reading_tests)
add_test(NAME hc.document.image_reading COMMAND hc_image_reading_tests -o "${CMAKE_BINARY_DIR}/image-reading-$<CONFIG>.txt,txt")
set_tests_properties(hc.document.image_reading PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
