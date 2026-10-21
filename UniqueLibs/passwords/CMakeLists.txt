qt_add_library(hc_passwords STATIC
    passwordStore.cpp passwordStore.hpp
    passwordUi.cpp passwordUi.hpp
    stdafx.h
)
target_include_directories(hc_passwords PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(hc_passwords PUBLIC Qt6::Widgets PRIVATE Fm::widgets crypt32 bcrypt)
target_compile_options(hc_passwords PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_precompile_headers(hc_passwords PRIVATE stdafx.h)
