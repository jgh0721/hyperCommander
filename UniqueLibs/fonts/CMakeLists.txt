qt_add_library(hc_startup_fonts STATIC startupFontWarmup.cpp startupFontWarmup.hpp stdafx.h)
target_precompile_headers(hc_startup_fonts PRIVATE stdafx.h)
target_include_directories(hc_startup_fonts PUBLIC "${PROJECT_SOURCE_DIR}")
target_include_directories(hc_startup_fonts PRIVATE "${PROJECT_SOURCE_DIR}/HyperCommander"
    "${PROJECT_SOURCE_DIR}/commonLibs")
target_link_libraries(hc_startup_fonts PRIVATE Qt6::Widgets dwrite ole32)
target_compile_definitions(hc_startup_fonts PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(hc_startup_fonts PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
