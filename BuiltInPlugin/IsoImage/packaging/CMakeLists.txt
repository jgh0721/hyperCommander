cmake_minimum_required(VERSION 3.25)
project(HyperCommanderIsoEngine LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Standalone LGPL source-package rebuild. No Qt or product source is required.
add_library(hc_iso_core SHARED
    ../src/api.cpp
    ../src/codec.cpp
    ../src/reader.cpp
    ../src/writer.cpp
    ../src/udfWriter.cpp
)
target_compile_definitions(hc_iso_core PRIVATE HC_ISO_BUILD)
target_include_directories(hc_iso_core PUBLIC ../include)
target_compile_options(hc_iso_core PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
