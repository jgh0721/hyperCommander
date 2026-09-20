find_package(Qt6 6.12 REQUIRED COMPONENTS Test)
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
target_compile_options(hc_workspace_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.workspace COMMAND hc_workspace_tests)
set_tests_properties(hc.workspace PROPERTIES TIMEOUT 30)
hc_enable_qt_deployment(hc_workspace_tests TEST)
qt_add_executable(hc_iso_engine_tests ../BuiltInPlugin/IsoImage/tests/tstIsoEngine.cpp)
target_link_libraries(hc_iso_engine_tests PRIVATE hc_iso_core Qt6::Test)
target_compile_options(hc_iso_engine_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_test(NAME hc.iso_engine COMMAND hc_iso_engine_tests)
set_tests_properties(hc.iso_engine PROPERTIES TIMEOUT 60)
hc_enable_qt_deployment(hc_iso_engine_tests TEST)
qt_add_executable(hc_iso_wcx_tests ../BuiltInPlugin/IsoImage/tests/tstIsoWcx.cpp)
target_link_libraries(hc_iso_wcx_tests PRIVATE hc_builtin_iso Qt6::Test)
target_compile_options(hc_iso_wcx_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_iso_wcx_tests hc-plugin-host-x64)
add_test(NAME hc.iso_wcx COMMAND hc_iso_wcx_tests)
set_tests_properties(hc.iso_wcx PROPERTIES TIMEOUT 120)
hc_enable_qt_deployment(hc_iso_wcx_tests TEST)
qt_add_executable(hc_iso_job_tests
    ../BuiltInPlugin/IsoImage/tests/tstIsoJob.cpp
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
    target_include_directories(hc_dopus_sdk_abi_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${HC_DOPUS_SDK_DIRECTORY}/headers")
    target_compile_options(hc_dopus_sdk_abi_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    add_test(NAME hc.dopus_sdk_abi COMMAND hc_dopus_sdk_abi_tests)
endif()
qt_add_library(hc_tc_data_fixture SHARED tcDataFixture.cpp)
target_compile_options(hc_tc_data_fixture PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
qt_add_executable(hc_hosted_data_tests tstHostedData.cpp ../UniqueLibs/nativeMaterializer.cpp ../UniqueLibs/storageProviders.cpp ../UniqueLibs/qtJobJournal.cpp)
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
add_test(NAME hc.viewer_surface COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
    -File "${PROJECT_SOURCE_DIR}/tools/Test-ViewerSurface.ps1"
    -Executable "$<TARGET_FILE:hc_viewer_surface_tests>" -Dpr 1)
set_tests_properties(hc.viewer_surface PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
add_test(NAME hc.viewer_surface150 COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
    -File "${PROJECT_SOURCE_DIR}/tools/Test-ViewerSurface.ps1"
    -Executable "$<TARGET_FILE:hc_viewer_surface_tests>" -Dpr 1.5)
set_tests_properties(hc.viewer_surface150 PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
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
add_dependencies(hc_plugin_process_tests hc-plugin-host-x64 hc_plugin_owner_fixture hc_wcx_fixture hc_plugin_fault_crash hc_plugin_fault_hang hc_plugin_fault_child hc_plugin_fault_detach hc_plugin_fault_slowstart hc_plugin_fault_attach)
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
if(TARGET hc_smb_tests)
    add_test(NAME hc.automation_ui
        COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
            -Executable "$<TARGET_FILE:hc_smb_tests>" -AutomationOnly -Dpr 1)
    set_tests_properties(hc.automation_ui PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    foreach(HC_WORKSPACE_DPR IN ITEMS 1 1.5)
        add_test(NAME hc.workspace_ui_${HC_WORKSPACE_DPR}
            COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
                -Executable "$<TARGET_FILE:hc_smb_tests>" -WorkspaceOnly -Dpr ${HC_WORKSPACE_DPR})
        set_tests_properties(hc.workspace_ui_${HC_WORKSPACE_DPR} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
        add_test(NAME hc.qtitan_ui_${HC_WORKSPACE_DPR}
            COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
                -Executable "$<TARGET_FILE:hc_smb_tests>" -QtitanOnly -Dpr ${HC_WORKSPACE_DPR} -TimeoutSeconds 90)
        set_tests_properties(hc.qtitan_ui_${HC_WORKSPACE_DPR} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endforeach()
    add_dependencies(hc_smb_tests hc_wlx_fixture hc_tc_data_fixture hc_dopus_fixture)
endif()
qt_add_executable(hc_hosted_wlx_tests tstHostedWlx.cpp)
target_sources(hc_hosted_wlx_tests PRIVATE ../UniqueLibs/nativeMaterializer.cpp)
target_link_libraries(hc_hosted_wlx_tests PRIVATE hc_viewer_qt Qt6::Test)
target_compile_options(hc_hosted_wlx_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_hosted_wlx_tests hc-plugin-host-x64 hc_wlx_fixture hc_wlx_sdk_fixture)
hc_enable_qt_deployment(hc_hosted_wlx_tests TEST PLUGINS)
foreach(HC_WLX_DPR IN ITEMS 1 1.5)
    add_test(NAME hc.hosted_wlx${HC_WLX_DPR} COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tools/Test-ViewerSurface.ps1"
        -Executable "$<TARGET_FILE:hc_hosted_wlx_tests>" -Dpr ${HC_WLX_DPR})
    set_tests_properties(hc.hosted_wlx${HC_WLX_DPR} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endforeach()
qt_add_executable(hc_wlx_selection_tests tstWlxSelection.cpp ../UniqueLibs/nativeMaterializer.cpp)
target_include_directories(hc_wlx_selection_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hc_wlx_selection_tests PRIVATE hc_plugin_viewer_sessions Qt6::Widgets Qt6::Test)
target_compile_options(hc_wlx_selection_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_wlx_selection_tests hc-plugin-host-x64 hc_wlx_fixture hc_wlx_sdk_fixture)
hc_enable_qt_deployment(hc_wlx_selection_tests TEST PLUGINS)
foreach(HC_SELECTION_DPR IN ITEMS 1 1.5)
    add_test(NAME hc.wlx_selection${HC_SELECTION_DPR} COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tools/Test-ViewerSurface.ps1"
        -Executable "$<TARGET_FILE:hc_wlx_selection_tests>" -Dpr ${HC_SELECTION_DPR})
    set_tests_properties(hc.wlx_selection${HC_SELECTION_DPR} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endforeach()
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
set_target_properties(hc_plugin_helper_fixture PROPERTIES AUTOMOC OFF AUTOUIC OFF)
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
qt_add_executable(hc_write_tests
    tstWrites.cpp
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
target_link_libraries(hc_write_tests PRIVATE Qt6::Test)
add_test(NAME hc.writes COMMAND hc_write_tests)
set_tests_properties(hc.writes PROPERTIES TIMEOUT 45 ENVIRONMENT "HC_MUTATION_ROOT=")
qt_add_executable(hc_job_tests tstJobs.cpp ../commonLibs/operations/jobScheduler.cpp ../commonLibs/operations/jobScheduler.hpp)
target_include_directories(hc_job_tests PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_options(hc_job_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_job_tests PRIVATE Qt6::Test)
add_test(NAME hc.jobs COMMAND hc_job_tests)
set_tests_properties(hc.jobs PROPERTIES TIMEOUT 30)
add_test(NAME hc.qtitan COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${PROJECT_SOURCE_DIR}/tools/Test-QtitanCompatibility.ps1")
set_tests_properties(hc.qtitan PROPERTIES TIMEOUT 30)

qt_add_executable(hc_column_tests tstColumnWidths.cpp ../commonLibs/view/columnWidthPolicy.hpp ../UniqueLibs/columnWidthSettings.hpp)
target_include_directories(hc_column_tests PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_options(hc_column_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_column_tests PRIVATE Qt6::Test)
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

add_test(NAME hc.sources
    COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tools/Test-SourceInventory.ps1"
        -SourceRoot "${PROJECT_SOURCE_DIR}"
        -SourceList "${CMAKE_BINARY_DIR}/hc-product-sources.txt"
)
add_test(NAME hc.startup
    COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tools/Test-AppStartup.ps1"
        -Executable "$<TARGET_FILE:HyperCommander>"
        -QtBin "$<TARGET_FILE_DIR:Qt6::Core>"
        -OutputRoot "${CMAKE_BINARY_DIR}/startup/$<CONFIG>"
)
set_tests_properties(hc.sources PROPERTIES TIMEOUT 30)
set_tests_properties(hc.startup PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

if(TARGET hc_smb_tests)
    foreach(HC_ISO_DPR IN ITEMS 1 1.5)
        add_test(NAME hc.iso_ui_${HC_ISO_DPR}
            COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
                -Executable "$<TARGET_FILE:hc_smb_tests>" -IsoOnly -Dpr ${HC_ISO_DPR})
        set_tests_properties(hc.iso_ui_${HC_ISO_DPR} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    endforeach()
    add_test(NAME hc.archive_ui
        COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Invoke-LocalUiTests.ps1"
            -Executable "$<TARGET_FILE:hc_smb_tests>" -ArchiveOnly -Dpr 1 -TimeoutSeconds 120)
    set_tests_properties(hc.archive_ui PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
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

add_test(NAME hc.deployment
    COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tools/Test-DeployedRuntime.ps1"
        -Executable "$<TARGET_FILE:HyperCommander>"
        -TestExecutables "$<TARGET_FILE:hc_automation_tests>|$<TARGET_FILE:hc_search_tests>|$<TARGET_FILE:hc_workspace_tests>|$<TARGET_FILE:hc_path_tests>|$<TARGET_FILE:hc_storage_tests>|$<TARGET_FILE:hc_command_tests>|$<TARGET_FILE:hc_column_tests>|$<TARGET_FILE:hc_job_tests>|$<TARGET_FILE:hc_write_tests>|$<TARGET_FILE:hc_materialization_tests>|$<TARGET_FILE:hc_wcx_tests>|$<TARGET_FILE:hc_hosted_wcx_tests>|$<TARGET_FILE:hc_viewer_tests>|$<TARGET_FILE:hc_viewer_surface_tests>|$<TARGET_FILE:hc_plugin_tests>|$<TARGET_FILE:hc_plugin_process_tests>|$<TARGET_FILE:hc_hosted_wlx_tests>|$<TARGET_FILE:hc_wlx_detection_tests>|$<TARGET_FILE:hc_wlx_selection_tests>|$<TARGET_FILE:hc_hosted_data_tests>|$<TARGET_FILE:hc_hosted_dopus_tests>|$<TARGET_FILE:hc_dopus_viewer_tests>|$<TARGET_FILE:hc_iso_engine_tests>|$<TARGET_FILE:hc_iso_wcx_tests>|$<TARGET_FILE:hc_iso_job_tests>"
        -OutputRoot "${CMAKE_BINARY_DIR}/deployment/$<CONFIG>"
)
# This runs the product and every deployed test executable sequentially. The
# added DOpus suites bring the measured Debug total close to one minute; keep
# process-level limits while allowing deployment/copy and startup overhead.
set_tests_properties(hc.deployment PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

# CTest comes from this configure's CMake installation, never an unrelated PATH entry.
add_custom_target(hc_check
    COMMAND "${CMAKE_CTEST_COMMAND}" -C "$<CONFIG>" --output-on-failure --no-tests=error
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS hc_dopus_viewer_tests hc_hosted_dopus_tests hc_hosted_data_tests hc_wlx_selection_tests hc_wlx_detection_tests hc_path_tests hc_storage_tests hc_command_tests hc_column_tests hc_job_tests hc_write_tests hc_materialization_tests hc_wcx_tests hc_hosted_wcx_tests hc_hosted_wlx_tests hc_viewer_tests hc_viewer_surface_tests hc_plugin_tests hc_plugin_process_tests HyperCommander
    USES_TERMINAL
    VERBATIM
)
add_dependencies(hc_check hc_iso_engine_tests hc_iso_wcx_tests hc_iso_job_tests)
add_dependencies(hc_check hc_workspace_tests hc_search_tests hc_automation_tests hc_script_api_tests)
add_dependencies(hc_check hc_lua_tests)
if(TARGET hc_dopus_sdk_abi_tests)
    add_dependencies(hc_check hc_dopus_sdk_abi_tests)
endif()

if(TARGET hc_smb_tests)
    add_dependencies(hc_check hc_smb_tests)
endif()

foreach(HC_TEST_TARGET IN ITEMS hc_path_tests hc_storage_tests hc_command_tests hc_column_tests hc_job_tests hc_write_tests hc_materialization_tests hc_wcx_tests)
    hc_enable_qt_deployment(${HC_TEST_TARGET} TEST)
endforeach()
