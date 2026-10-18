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

# Rebuilding and installing a replacement engine is part of this standalone
# project, including when the source archive contains no local Markdown files.
install(TARGETS hc_iso_core
    RUNTIME DESTINATION . COMPONENT IsoEngine
    LIBRARY DESTINATION . COMPONENT IsoEngine
)
install(FILES COPYING THIRD_PARTY_NOTICES.txt DESTINATION . COMPONENT IsoEngine)
message(STATUS "Build engine: cmake --build \"${CMAKE_BINARY_DIR}\" --config Release --target hc_iso_core")
message(STATUS "Install engine: cmake --install \"${CMAKE_BINARY_DIR}\" --config Release --component IsoEngine --prefix <plugin-directory>")
message(STATUS "Close Total Commander before replacing its engine DLL. In HyperCommander, register a separate external plugin with updated manifest hashes.")
