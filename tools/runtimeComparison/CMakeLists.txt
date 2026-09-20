include(FetchContent)
# Same engine and exception model isolate binding cost. SOURCE_SUBDIR avoids
# configuring upstream examples/tests or changing the selected Qt toolchain.
FetchContent_Declare(hc_lua
    URL https://www.lua.org/ftp/lua-5.4.9.tar.gz
    URL_HASH SHA256=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR hc-not-a-subproject)
FetchContent_Declare(hc_sol2
    URL https://codeload.github.com/ThePhD/sol2/tar.gz/refs/tags/v3.3.0
    URL_HASH SHA256=b82c5de030e18cb2bcbcefcd5f45afd526920c517a96413f0b59b4332d752a1e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR hc-not-a-subproject)
FetchContent_MakeAvailable(hc_lua hc_sol2)
set(LuaNames lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo ldump lfunc lgc linit liolib llex lmathlib lmem loadlib lobject lopcodes loslib lparser lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio)
set(LuaSources)
foreach(Name IN LISTS LuaNames)
    list(APPEND LuaSources "${hc_lua_SOURCE_DIR}/src/${Name}.c")
endforeach()
set_source_files_properties(${LuaSources} PROPERTIES LANGUAGE CXX)
qt_add_library(hc_comparison_lua STATIC ${LuaSources})
set_target_properties(hc_comparison_lua PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(hc_comparison_lua PUBLIC "${hc_lua_SOURCE_DIR}/src")
target_compile_definitions(hc_comparison_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
find_package(Qt6 6.12 REQUIRED COMPONENTS Qml)
foreach(Candidate IN ITEMS lua sol2 qjs)
    qt_add_executable(hc_runtime_${Candidate} main.cpp runtime.cpp wire.cpp wire.hpp runtime.hpp
        ../../commonLibs/scripting/scriptApi.cpp)
    set_target_properties(hc_runtime_${Candidate} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${Candidate}")
    target_link_libraries(hc_runtime_${Candidate} PRIVATE Qt6::Core)
    if(WIN32)
        target_link_libraries(hc_runtime_${Candidate} PRIVATE Psapi)
    endif()
    target_compile_options(hc_runtime_${Candidate} PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    if(Candidate STREQUAL "qjs")
        target_compile_definitions(hc_runtime_${Candidate} PRIVATE HC_RUNTIME_QJS)
        target_link_libraries(hc_runtime_${Candidate} PRIVATE Qt6::Qml)
    else()
        target_link_libraries(hc_runtime_${Candidate} PRIVATE hc_comparison_lua)
        if(Candidate STREQUAL "sol2")
            target_include_directories(hc_runtime_${Candidate} PRIVATE "${hc_sol2_SOURCE_DIR}/include")
            target_compile_definitions(hc_runtime_${Candidate} PRIVATE HC_RUNTIME_SOL2 SOL_ALL_SAFETIES_ON=1 SOL_USING_CXX_LUA=1)
        endif()
    endif()
    if(WIN32)
        add_custom_command(TARGET hc_runtime_${Candidate} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different $<TARGET_RUNTIME_DLLS:hc_runtime_${Candidate}> "$<TARGET_FILE_DIR:hc_runtime_${Candidate}>"
            COMMAND_EXPAND_LISTS VERBATIM)
    endif()
    add_custom_command(TARGET hc_runtime_${Candidate} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/ThirdPartyNotices.txt" "$<TARGET_FILE_DIR:hc_runtime_${Candidate}>/ThirdPartyNotices.txt"
        VERBATIM)
    if(BUILD_TESTING)
        add_test(NAME hc.runtime_${Candidate} COMMAND hc_runtime_${Candidate} --verify)
        set_tests_properties(hc.runtime_${Candidate} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
        if(Candidate STREQUAL HC_SELECTED_SCRIPT_RUNTIME)
            set_tests_properties(hc.runtime_${Candidate} PROPERTIES LABELS "script;selected-runtime")
        else()
            set_tests_properties(hc.runtime_${Candidate} PROPERTIES LABELS "script;comparison-runtime")
        endif()
        add_dependencies(hc_check hc_runtime_${Candidate})
    endif()
endforeach()
find_program(HC_RUNTIME_POWERSHELL NAMES powershell REQUIRED)
add_custom_target(hc_compare_script_runtimes
    COMMAND "${HC_RUNTIME_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/Compare-Runtimes.ps1"
        -Lua "$<TARGET_FILE:hc_runtime_lua>" -Sol2 "$<TARGET_FILE:hc_runtime_sol2>" -Qjs "$<TARGET_FILE:hc_runtime_qjs>"
        -Output "${CMAKE_CURRENT_BINARY_DIR}/comparison-$<CONFIG>.json"
    DEPENDS hc_runtime_lua hc_runtime_sol2 hc_runtime_qjs
    USES_TERMINAL VERBATIM)
