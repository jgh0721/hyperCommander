set(HC_UNEGG_USE "NONE" CACHE STRING "UnEGG use rights: NONE, NONCOMMERCIAL, or VENDOR_APPROVED")
set_property(CACHE HC_UNEGG_USE PROPERTY STRINGS NONE NONCOMMERCIAL VENDOR_APPROVED)
if(NOT HC_UNEGG_USE MATCHES "^(NONE|NONCOMMERCIAL|VENDOR_APPROVED)$")
    message(FATAL_ERROR "Invalid HC_UNEGG_USE")
endif()
set(HC_UNEGG_ARCHIVE "${CMAKE_BINARY_DIR}/_deps/UnEGG.zip")
set(HC_UNEGG_SOURCE "${CMAKE_BINARY_DIR}/_deps/UnEGG")
if(NOT HC_UNEGG_USE STREQUAL "NONE")
    # Rights are a build input, not inferred from the module being downloadable.
    if(NOT EXISTS "${HC_UNEGG_ARCHIVE}")
        file(DOWNLOAD "https://cf-aldn.altools.co.kr/setup/UnEGG.zip" "${HC_UNEGG_ARCHIVE}"
            EXPECTED_HASH "SHA256=90e8cc6d5df08eec11b4408b79f525a836e522ed94a8eac13f37e07f786c654f" TLS_VERIFY ON)
    endif()
    file(SHA256 "${HC_UNEGG_ARCHIVE}" PACKAGE_HASH)
    if(NOT PACKAGE_HASH STREQUAL "90e8cc6d5df08eec11b4408b79f525a836e522ed94a8eac13f37e07f786c654f")
        message(FATAL_ERROR "UnEGG SDK identity mismatch")
    endif()
    # Reconfiguration must not replace SDK DLLs that another build may have
    # loaded. Packaging independently verifies the x64 DLL's pinned SHA-256.
    if(NOT EXISTS "${HC_UNEGG_SOURCE}/bin/UnEGG64.dll" OR NOT EXISTS "${HC_UNEGG_SOURCE}/doc/UnEGG.txt")
        file(ARCHIVE_EXTRACT INPUT "${HC_UNEGG_ARCHIVE}" DESTINATION "${HC_UNEGG_SOURCE}")
    endif()
endif()
qt_add_library(hc_unegg_backend STATIC unEggBackend.cpp unEggBackend.hpp unEggAbi.hpp)
target_link_libraries(hc_unegg_backend PUBLIC hc_plugin_contract PRIVATE Qt6::Core)
target_compile_options(hc_unegg_backend PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc-plugin-host-x64 PRIVATE hc_unegg_backend)
set(PIN_HEADER "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/hcUnEggManifest.hpp")
set(PACKAGE_COMMAND "${CMAKE_COMMAND}" "-DUSE=${HC_UNEGG_USE}"
    "-DOUTPUT=$<TARGET_FILE_DIR:hc-plugin-host-x64>/${HC_LAYOUT_AlzEggPackage}"
    "-DLICENSES=$<TARGET_FILE_DIR:hc-plugin-host-x64>/${HC_LAYOUT_Licenses}/AlzEgg"
    "-DHEADER=${PIN_HEADER}" "-DSOURCE=${HC_UNEGG_SOURCE}" "-DARCHIVE=${HC_UNEGG_ARCHIVE}"
    ${HC_PACKAGE_SIGN_ARGS} -P "${CMAKE_CURRENT_SOURCE_DIR}/GeneratePackage.cmake")
add_custom_target(hc_unegg_package COMMAND ${PACKAGE_COMMAND} BYPRODUCTS "${PIN_HEADER}" VERBATIM)
qt_add_library(hc_builtin_unegg STATIC builtinUnEgg.cpp builtinUnEgg.hpp)
target_include_directories(hc_builtin_unegg PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
target_link_libraries(hc_builtin_unegg PUBLIC hc_plugin_process)
target_compile_options(hc_builtin_unegg PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_builtin_unegg hc_unegg_package)
