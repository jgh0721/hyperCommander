include("${CMAKE_CURRENT_SOURCE_DIR}/SevenZipCore.cmake")
qt_add_library(hc_sevenzip_backend STATIC sevenZipBackend.cpp sevenZipBackend.hpp sevenZipUpdate.inl sevenZipMetadata.inl sevenZipMethods.inl sevenZipGroups.inl sevenZipMsi.inl)
target_include_directories(hc_sevenzip_backend PRIVATE "${HC_7ZIP_SOURCE}/CPP")
target_link_libraries(hc_sevenzip_backend PUBLIC hc_plugin_contract PRIVATE Qt6::Core oleaut32 msi)
target_compile_options(hc_sevenzip_backend PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc-plugin-host-x64 PRIVATE hc_sevenzip_backend)
set(PIN_HEADER "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/hcSevenZipManifest.hpp")
set(PACKAGE_COMMAND "${CMAKE_COMMAND}"
    "-DCORE=$<TARGET_FILE:hc_sevenzip_core>"
    "-DOUTPUT=$<TARGET_FILE_DIR:hc_sevenzip_core>/${HC_LAYOUT_SevenZipPackage}"
    "-DLICENSES=$<TARGET_FILE_DIR:hc_sevenzip_core>/${HC_LAYOUT_Licenses}/SevenZip"
    "-DHEADER=${PIN_HEADER}" "-DSOURCE=${HC_7ZIP_SOURCE}" "-DARCHIVE=${HC_7ZIP_ARCHIVE}"
    ${HC_PACKAGE_SIGN_ARGS} -P "${CMAKE_CURRENT_SOURCE_DIR}/GeneratePackage.cmake")
add_custom_command(OUTPUT "${PIN_HEADER}" COMMAND ${PACKAGE_COMMAND}
    DEPENDS hc_sevenzip_core GeneratePackage.cmake VERBATIM)
add_custom_target(hc_sevenzip_package COMMAND ${PACKAGE_COMMAND} DEPENDS "${PIN_HEADER}" VERBATIM)
qt_add_library(hc_builtin_sevenzip STATIC builtinSevenZip.cpp builtinSevenZip.hpp sevenZipJob.cpp sevenZipJob.hpp)
target_include_directories(hc_builtin_sevenzip PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
target_link_libraries(hc_builtin_sevenzip PUBLIC hc_plugin_process)
target_compile_options(hc_builtin_sevenzip PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_dependencies(hc_builtin_sevenzip hc_sevenzip_package)
