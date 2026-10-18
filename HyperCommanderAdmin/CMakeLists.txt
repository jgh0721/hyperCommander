qt_add_executable(HyperCommanderAdmin main.cpp
    ../UniqueLibs/nativeFileElevation.cpp
    ../UniqueLibs/storageProviders.cpp
    ../UniqueLibs/nativeWritableStorage.cpp
    ../commonLibs/operations/fileOperation.cpp
    ../commonLibs/operations/jobScheduler.cpp
)
hc_add_version_resource(HyperCommanderAdmin "HyperCommander Administrator")
target_include_directories(HyperCommanderAdmin PRIVATE "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/commonLibs")
target_link_libraries(HyperCommanderAdmin PRIVATE Qt6::Core Advapi32 Shell32 Ole32)
target_compile_definitions(HyperCommanderAdmin PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(HyperCommanderAdmin PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
hc_enable_qt_deployment(HyperCommanderAdmin)
