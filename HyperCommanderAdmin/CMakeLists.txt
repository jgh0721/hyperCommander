set(CMAKE_CXX_STANDARD 20)

add_executable(HyperCommanderAdmin main.cpp
        stdafx.h
        stdafx.cpp
        cmnConfig.hpp
        main.hpp)

if (MSVC)
    target_compile_definitions(HyperCommanderAdmin PUBLIC UNICODE _UNICODE)
endif()

include_directories( ${CMAKE_CURRENT_SOURCE_DIR} )

target_precompile_headers(HyperCommanderAdmin PUBLIC stdafx.h)

add_subdirectory( ../commonLibs ${CMAKE_BINARY_DIR}/commonLibs )
target_link_libraries(HyperCommanderAdmin PRIVATE commonLibs)

