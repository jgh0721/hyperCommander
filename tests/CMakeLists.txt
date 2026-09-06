find_package(Qt6 6.12 REQUIRED COMPONENTS Test)
find_program(HC_POWERSHELL NAMES powershell REQUIRED)
qt_add_executable(hc_viewer_tests
    tstViewer.cpp
    ../UniqueLibs/storageProviders.cpp
    ../UniqueLibs/wcxStorage.cpp
    ../UniqueLibs/nativeMaterializer.cpp
)
target_include_directories(hc_viewer_tests PRIVATE ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/commonLibs ${PROJECT_SOURCE_DIR}/HyperCommander)
target_compile_options(hc_viewer_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_viewer_tests PRIVATE Qt6::Test hc_viewer_qt)
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
target_link_libraries(hc_wcx_tests PRIVATE Qt6::Test)
add_dependencies(hc_wcx_tests hc_wcx_fixture)
add_test(NAME hc.wcx COMMAND hc_wcx_tests)
set_tests_properties(hc.wcx PROPERTIES TIMEOUT 45 ENVIRONMENT "HC_WCX_PLUGIN=")
add_custom_command(TARGET hc_wcx_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:hc_wcx_fixture>" "$<TARGET_FILE_DIR:hc_wcx_tests>/hc_wcx_fixture.wcx64" VERBATIM)
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

add_test(NAME hc.deployment
    COMMAND "${HC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tools/Test-DeployedRuntime.ps1"
        -Executable "$<TARGET_FILE:HyperCommander>"
        -TestExecutables "$<TARGET_FILE:hc_path_tests>|$<TARGET_FILE:hc_storage_tests>|$<TARGET_FILE:hc_command_tests>|$<TARGET_FILE:hc_column_tests>|$<TARGET_FILE:hc_job_tests>|$<TARGET_FILE:hc_write_tests>|$<TARGET_FILE:hc_materialization_tests>|$<TARGET_FILE:hc_wcx_tests>|$<TARGET_FILE:hc_viewer_tests>|$<TARGET_FILE:hc_viewer_surface_tests>|$<TARGET_FILE:hc_plugin_tests>|$<TARGET_FILE:hc_plugin_process_tests>"
        -OutputRoot "${CMAKE_BINARY_DIR}/deployment/$<CONFIG>"
)
set_tests_properties(hc.deployment PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

# CTest comes from this configure's CMake installation, never an unrelated PATH entry.
add_custom_target(hc_check
    COMMAND "${CMAKE_CTEST_COMMAND}" -C "$<CONFIG>" --output-on-failure --no-tests=error
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS hc_path_tests hc_storage_tests hc_command_tests hc_column_tests hc_job_tests hc_write_tests hc_materialization_tests hc_wcx_tests hc_viewer_tests hc_viewer_surface_tests hc_plugin_tests hc_plugin_process_tests HyperCommander
    USES_TERMINAL
    VERBATIM
)

foreach(HC_TEST_TARGET IN ITEMS hc_path_tests hc_storage_tests hc_command_tests hc_column_tests hc_job_tests hc_write_tests hc_materialization_tests hc_wcx_tests)
    hc_enable_qt_deployment(${HC_TEST_TARGET} TEST)
endforeach()
