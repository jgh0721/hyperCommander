include(FetchContent)
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
# C++ unwinding keeps binding temporaries alive across Lua errors with Hybrid CRT.
set_source_files_properties(${LuaSources} PROPERTIES LANGUAGE CXX)
qt_add_library(hc_lua_engine STATIC ${LuaSources})
set_target_properties(hc_lua_engine PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(hc_lua_engine PUBLIC "${hc_lua_SOURCE_DIR}/src")
target_compile_definitions(hc_lua_engine PRIVATE _CRT_SECURE_NO_WARNINGS)
qt_add_library(hc_script_runtime STATIC
    luaRuntime.cpp luaRuntime.hpp scriptExecution.cpp scriptExecution.hpp scriptApi.cpp scriptApi.hpp)
set_target_properties(hc_script_runtime PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(hc_script_runtime PRIVATE "${hc_sol2_SOURCE_DIR}/include")
target_link_libraries(hc_script_runtime PRIVATE hc_lua_engine)
target_compile_definitions(hc_script_runtime PRIVATE SOL_ALL_SAFETIES_ON=1 SOL_USING_CXX_LUA=1)
target_compile_options(hc_script_runtime PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
# Comparison bindings use precisely the same pinned engine and sol2 headers.
set(HC_SOL2_INCLUDE_DIR "${hc_sol2_SOURCE_DIR}/include" CACHE INTERNAL "Pinned sol2 headers" FORCE)
