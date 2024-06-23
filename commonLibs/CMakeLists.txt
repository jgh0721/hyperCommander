set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FMT_SRC
        fmt-10.2.0/include/fmt/format.h )

add_library(commonLibs OBJECT
    cmnBase.hpp
    cmnCompilerDetection.hpp
    cmnConcurrent.cpp
    cmnConcurrent.hpp
    cmnDateTime.cpp
    cmnDateTime.hpp
    cmnPath.cpp
    cmnPath.hpp
    cmnProcess.cpp
    cmnProcess.hpp
    cmnSecurity.cpp
    cmnSecurity.hpp
    cmnTextUtils.cpp
    cmnTextUtils.hpp
    win/winLIBMgr.hpp
    ${FMT_SRC}
)

if (MSVC)
    target_compile_definitions(commonLibs PUBLIC UNICODE _UNICODE)
endif()

target_compile_definitions(commonLibs PUBLIC FMT_HEADER_ONLY=1)

target_include_directories( commonLibs PUBLIC . )
target_include_directories( commonLibs PUBLIC ./fmt-10.2.0/include )
