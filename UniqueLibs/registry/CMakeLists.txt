qt_add_library(hc_registry_transactions STATIC registryHistory.cpp registryHistory.hpp)
target_sources(hc_registry_transactions PRIVATE registryProtocol.cpp registryProtocol.hpp)
target_sources(hc_registry_transactions PRIVATE registryMutation.cpp registryMutation.hpp registryCrud.cpp)
set_target_properties(hc_registry_transactions PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(hc_registry_transactions PUBLIC "${PROJECT_SOURCE_DIR}" PRIVATE
    "${PROJECT_SOURCE_DIR}/commonLibs/system/registry" "${PROJECT_SOURCE_DIR}/HyperCommanderAdmin" "${PROJECT_SOURCE_DIR}/commonLibs")
target_precompile_headers(hc_registry_transactions PRIVATE "${PROJECT_SOURCE_DIR}/commonLibs/system/registry/stdafx.h")
target_compile_definitions(hc_registry_transactions PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hc_registry_transactions PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_registry_transactions PUBLIC hc_registry_core Qt6::Core PRIVATE advapi32)

qt_add_library(hc_registry_models STATIC registryService.cpp registryService.hpp registryModels.cpp registryModels.hpp registryRootProxy.cpp registryRootProxy.hpp)
target_sources(hc_registry_models PRIVATE registryNavigation.cpp registryNavigation.hpp
    registryFilterProxy.cpp registryFilterProxy.hpp registryFavorites.cpp registryFavorites.hpp
    registryHistoryModel.cpp registryHistoryModel.hpp)
target_include_directories(hc_registry_models PUBLIC "${PROJECT_SOURCE_DIR}" PRIVATE
    "${PROJECT_SOURCE_DIR}/commonLibs/system/registry" "${PROJECT_SOURCE_DIR}/HyperCommanderAdmin" "${PROJECT_SOURCE_DIR}/commonLibs")
target_precompile_headers(hc_registry_models PRIVATE "${PROJECT_SOURCE_DIR}/commonLibs/system/registry/stdafx.h")
target_compile_definitions(hc_registry_models PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hc_registry_models PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_registry_models PUBLIC hc_registry_transactions Qt6::Concurrent)

qt_add_library(hc_registry_editors STATIC
    ../../HyperCommander/UIs/registryEditors.cpp ../../HyperCommander/UIs/registryEditors.hpp
    ../../HyperCommander/UIs/registryDialogs.cpp ../../HyperCommander/UIs/registryDialogs.hpp
    ../ui/pathListEditor.cpp ../ui/pathListEditor.hpp)
target_include_directories(hc_registry_editors PUBLIC "${PROJECT_SOURCE_DIR}" PRIVATE
    "${PROJECT_SOURCE_DIR}/commonLibs/system/registry" "${PROJECT_SOURCE_DIR}/HyperCommanderAdmin" "${PROJECT_SOURCE_DIR}/commonLibs")
target_precompile_headers(hc_registry_editors PRIVATE "${PROJECT_SOURCE_DIR}/commonLibs/system/registry/stdafx.h")
target_compile_definitions(hc_registry_editors PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hc_registry_editors PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_registry_editors PUBLIC hc_registry_core Fm::widgets Qt6::Concurrent)

qt_add_library(hc_registry_descriptions STATIC registryAdmx.cpp registryAdmx.hpp)
target_include_directories(hc_registry_descriptions PUBLIC "${PROJECT_SOURCE_DIR}" PRIVATE
    "${PROJECT_SOURCE_DIR}/commonLibs/system/registry" "${PROJECT_SOURCE_DIR}/HyperCommanderAdmin" "${PROJECT_SOURCE_DIR}/commonLibs")
target_precompile_headers(hc_registry_descriptions PRIVATE "${PROJECT_SOURCE_DIR}/commonLibs/system/registry/stdafx.h")
target_compile_definitions(hc_registry_descriptions PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hc_registry_descriptions PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_link_libraries(hc_registry_descriptions PUBLIC hc_registry_transactions Qt6::Core)

target_sources(hc_registry_descriptions PRIVATE registryCatalog.cpp registryCatalog.hpp)
qt_add_resources(hc_registry_descriptions registryCatalog
    PREFIX "/hc/registry"
    BASE "${PROJECT_SOURCE_DIR}/HyperCommander/Resources/registry"
    FILES "${PROJECT_SOURCE_DIR}/HyperCommander/Resources/registry/catalog.json"
          "${PROJECT_SOURCE_DIR}/HyperCommander/Resources/registry/en-US.json"
          "${PROJECT_SOURCE_DIR}/HyperCommander/Resources/registry/ko-KR.json")

target_sources(hc_registry_descriptions PRIVATE registryDescriptions.cpp registryDescriptions.hpp)
target_link_libraries(hc_registry_descriptions PUBLIC Qt6::Concurrent)
target_link_libraries(hc_registry_models PUBLIC hc_registry_descriptions)

target_sources(hc_registry_descriptions PRIVATE registryPolicyDefinition.cpp registryPolicyDefinition.hpp)

target_sources(hc_registry_models PRIVATE registryResultModel.cpp registryResultModel.hpp)

target_sources(hc_registry_descriptions PRIVATE registryPolicyState.cpp registryPolicyState.hpp)

target_sources(hc_registry_models PRIVATE registryPolicies.cpp registryPolicies.hpp)

target_sources(hc_registry_descriptions PRIVATE registryPolicyEdit.cpp registryPolicyEdit.hpp)

target_sources(hc_registry_editors PRIVATE ../../HyperCommander/UIs/registryPolicyEditor.cpp ../../HyperCommander/UIs/registryPolicyEditor.hpp)
target_link_libraries(hc_registry_editors PUBLIC hc_registry_models)

target_sources(hc_registry_models PRIVATE registryLinks.cpp registryLinks.hpp)
target_link_libraries(hc_registry_models PRIVATE shlwapi)

target_sources(hc_registry_models PRIVATE registryAutoruns.cpp registryAutoruns.hpp)
target_link_libraries(hc_registry_models PRIVATE hc_system)

target_sources(hc_registry_editors PRIVATE ../../HyperCommander/UIs/registryLinkPane.cpp ../../HyperCommander/UIs/registryLinkPane.hpp)

target_sources(hc_registry_models PRIVATE registryLinkCache.cpp registryLinkCache.hpp)

target_sources(hc_registry_models PRIVATE registrySearch.cpp registrySearch.hpp)

target_sources(hc_registry_models PRIVATE registryBatch.cpp registryBatch.hpp)

target_sources(hc_registry_models PRIVATE registryImport.cpp registryImport.hpp)

target_sources(hc_registry_models PRIVATE registryCompareLease.cpp registryCompareLease.hpp)
