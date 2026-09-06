find_package(Qt6 6.12 REQUIRED COMPONENTS Test)
find_program(HC_POWERSHELL NAMES powershell REQUIRED)
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
        -TestExecutables "$<TARGET_FILE:hc_path_tests>|$<TARGET_FILE:hc_storage_tests>|$<TARGET_FILE:hc_command_tests>|$<TARGET_FILE:hc_column_tests>|$<TARGET_FILE:hc_job_tests>|$<TARGET_FILE:hc_write_tests>"
        -OutputRoot "${CMAKE_BINARY_DIR}/deployment/$<CONFIG>"
)
set_tests_properties(hc.deployment PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

# CTest comes from this configure's CMake installation, never an unrelated PATH entry.
add_custom_target(hc_check
    COMMAND "${CMAKE_CTEST_COMMAND}" -C "$<CONFIG>" --output-on-failure --no-tests=error
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS hc_path_tests hc_storage_tests hc_command_tests hc_column_tests hc_job_tests hc_write_tests HyperCommander
    USES_TERMINAL
    VERBATIM
)

foreach(HC_TEST_TARGET IN ITEMS hc_path_tests hc_storage_tests hc_command_tests hc_column_tests hc_job_tests hc_write_tests)
    hc_enable_qt_deployment(${HC_TEST_TARGET} TEST)
endforeach()
