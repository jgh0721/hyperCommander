qt_add_library(hc_iso_core SHARED
    src/api.cpp
    src/codec.cpp
    src/reader.cpp
    src/writer.cpp
    src/udfWriter.cpp
    src/image.hpp
    include/hcIso/isoImage.h
)
set_target_properties(hc_iso_core PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    CXX_STANDARD 23
)
target_compile_definitions(hc_iso_core PRIVATE HC_ISO_BUILD)
target_include_directories(hc_iso_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(hc_iso_core PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

qt_add_library(hc_isoimage_wcx SHARED adapters/wcx/wcxExports.cpp)
set_target_properties(hc_isoimage_wcx PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    CXX_STANDARD 23
    SUFFIX ".wcx64"
)
target_link_libraries(hc_isoimage_wcx PRIVATE hc_iso_core)
target_compile_options(hc_isoimage_wcx PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

# TC probes legacy exports before choosing the Unicode WCX interface. Keep that
# compatibility surface separate from the built-in, Unicode-only host adapter.
qt_add_library(hc_isoimage_tc_wcx SHARED EXCLUDE_FROM_ALL
    adapters/wcx/wcxExports.cpp
    adapters/wcx/tcExports.cpp
)
set_target_properties(hc_isoimage_tc_wcx PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    CXX_STANDARD 23
    OUTPUT_NAME "hc_isoimage_tc"
    SUFFIX ".wcx"
)
target_link_libraries(hc_isoimage_tc_wcx PRIVATE hc_iso_core)
target_compile_options(hc_isoimage_tc_wcx PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

add_custom_target(hc_iso_tc_package
    COMMAND "${CMAKE_COMMAND}"
        "-DCORE=$<TARGET_FILE:hc_iso_core>"
        "-DWCX=$<TARGET_FILE:hc_isoimage_tc_wcx>"
        "-DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/tc-package/$<CONFIG>"
        "-DZIP=${CMAKE_BINARY_DIR}/packages/$<CONFIG>/hc-isoimage-tc-x64.zip"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/packaging/GenerateTcPackage.cmake"
    DEPENDS hc_iso_core hc_isoimage_tc_wcx
    BYPRODUCTS "${CMAKE_BINARY_DIR}/packages/$<CONFIG>/hc-isoimage-tc-x64.zip"
    VERBATIM
)

file(GLOB_RECURSE HC_ISO_PACKAGE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/adapters/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/*"
)
set(HC_ISO_PIN_HEADER "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/hcIsoManifest.hpp")
set(HC_ISO_PACKAGE_COMMAND "${CMAKE_COMMAND}"
        "-DCORE=$<TARGET_FILE:hc_iso_core>"
        "-DWCX=$<TARGET_FILE:hc_isoimage_wcx>"
        "-DOUTPUT=$<TARGET_FILE_DIR:hc_iso_core>/BuiltInPlugin/IsoImage"
        "-DHEADER=${HC_ISO_PIN_HEADER}"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/packaging/GeneratePackage.cmake")
add_custom_command(OUTPUT "${HC_ISO_PIN_HEADER}"
    COMMAND ${HC_ISO_PACKAGE_COMMAND}
    DEPENDS hc_iso_core hc_isoimage_wcx ${HC_ISO_PACKAGE_SOURCES}
    VERBATIM
)
add_custom_target(hc_iso_package
    COMMAND ${HC_ISO_PACKAGE_COMMAND}
    DEPENDS "${HC_ISO_PIN_HEADER}"
    VERBATIM
)
qt_add_library(hc_builtin_iso STATIC builtinIso.cpp builtinIso.hpp isoJob.cpp isoJob.hpp)
target_include_directories(hc_builtin_iso PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
target_link_libraries(hc_builtin_iso PUBLIC hc_plugin_process hc_viewer_core)
target_compile_options(hc_builtin_iso PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_builtin_iso hc_iso_package)
