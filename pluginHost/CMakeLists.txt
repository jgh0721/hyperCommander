qt_add_library(hc_plugin_contract STATIC
    ../commonLibs/plugins/pluginManifest.cpp
    ../commonLibs/plugins/pluginManifest.hpp
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
