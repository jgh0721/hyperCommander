find_package(Qt6 6.12 REQUIRED COMPONENTS Test)
find_program(HC_POWERSHELL NAMES powershell REQUIRED)

qt_add_executable(hc_storage_tests tstStorage.cpp memoryStorage.hpp
    ../commonLibs/storage/storage.hpp)
target_include_directories(hc_storage_tests PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_options(hc_storage_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>
)
target_link_libraries(hc_storage_tests PRIVATE Qt6::Test)
add_test(NAME hc.storage COMMAND hc_storage_tests)
set_tests_properties(hc.storage PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
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
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
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

# CTest comes from this configure's CMake installation, never an unrelated PATH entry.
add_custom_target(hc_check
    COMMAND "${CMAKE_CTEST_COMMAND}" -C "$<CONFIG>" --output-on-failure --no-tests=error
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS hc_path_tests hc_storage_tests HyperCommander
    USES_TERMINAL
    VERBATIM
)
