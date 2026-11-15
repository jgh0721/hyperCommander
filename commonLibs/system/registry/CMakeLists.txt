qt_add_library(hc_registry_core STATIC
    ../registryAccess.cpp ../registryAccess.hpp
    ../registryFile.cpp ../registryFile.hpp
    ../registryPolicyFile.cpp ../registryPolicyFile.hpp
    ../registryWatch.cpp ../registryWatch.hpp
    ../registryValue.cpp ../registryValue.hpp
    ../registryEnvironment.cpp ../registryEnvironment.hpp
    ../environmentNotification.cpp ../environmentNotification.hpp
)
set_target_properties(hc_registry_core PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(hc_registry_core PUBLIC "${PROJECT_SOURCE_DIR}" PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/HyperCommanderAdmin" "${PROJECT_SOURCE_DIR}/commonLibs")
target_precompile_headers(hc_registry_core PRIVATE stdafx.h)
target_compile_definitions(hc_registry_core PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hc_registry_core PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_registry_core PUBLIC Qt6::Core PRIVATE advapi32 user32)

target_sources(hc_registry_core PRIVATE ../registryLocalPolicy.cpp ../registryLocalPolicy.hpp)
target_link_libraries(hc_registry_core PRIVATE ole32)
