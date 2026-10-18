cmake_minimum_required(VERSION 4.2)
set(HC_PRODUCT_VERSION "2026.10.18.1")
project(HyperCommander VERSION ${HC_PRODUCT_VERSION} LANGUAGES CXX RC)

include(cmake/build.version.cmake)
hc_split_version("${HC_PRODUCT_VERSION}" HC_VER_MAJOR HC_VER_MINOR HC_VER_PATCH HC_VER_TWEAK)
configure_file("${PROJECT_SOURCE_DIR}/UniqueLibs/solTypeDefs_Ver.hpp.in"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/solTypeDefs_Ver.hpp" @ONLY NEWLINE_STYLE CRLF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTORCC ON)
# qt_add_executable applies the selected kit's Hybrid CRT policy.

find_package(Qt6 6.12 REQUIRED COMPONENTS
    Core
    CorePrivate
    Concurrent
    Gui
    GuiPrivate
    Sql
    Svg
    Network
    UiTools
    PrintSupport
    Widgets
    WidgetsPrivate
)
qt_standard_project_setup(REQUIRES 6.12)

include_directories( ${CMAKE_CURRENT_LIST_DIR} )
include_directories( ${CMAKE_CURRENT_LIST_DIR}/UniqueLibs )
# Product builds consume only the libraries; standalone waterColor tests stay isolated.
set(FMSTYLE_WITH_QTITAN ON CACHE BOOL "" FORCE)
foreach(Option IN ITEMS DEMO LABS GALLERY DESIGNER_PLUGIN EXAMPLES TESTS)
    set(FMSTYLE_BUILD_${Option} OFF CACHE BOOL "" FORCE)
endforeach()
if(QTITAN_SOURCE_DIR)
    message(FATAL_ERROR "Product builds require generated Qtitan sources; clear QTITAN_SOURCE_DIR.")
endif()
add_subdirectory(externalLibs/waterColorStyle watercolor EXCLUDE_FROM_ALL)
include(cmake/QtDeployment.cmake)
include(cmake/ProductLayout.cmake)
set(HC_SELECTED_SCRIPT_RUNTIME "sol2")
add_subdirectory(commonLibs/scripting)

qt_add_library(hc_viewer_core STATIC
    commonLibs/viewer/viewer.cpp
    commonLibs/viewer/viewer.hpp
    commonLibs/operations/jobScheduler.cpp
    commonLibs/operations/jobScheduler.hpp
    commonLibs/operations/providerInteractionTask.hpp
    commonLibs/operations/writeRecovery.hpp
    commonLibs/operations/selectionPlan.hpp
)
# The core uses the kit's CRT settings but has no Qt headers or linkage.
set_target_properties(hc_viewer_core PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(hc_viewer_core PUBLIC ${PROJECT_SOURCE_DIR})
target_compile_options(hc_viewer_core PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
qt_add_library(hc_viewer_qt STATIC
    UniqueLibs/batchRenameJournal.cpp
    UniqueLibs/batchRenameJournal.hpp
    UniqueLibs/qtJobJournal.cpp
    UniqueLibs/qtJobJournal.hpp
    UniqueLibs/qtDurableOutcomes.cpp
    UniqueLibs/qtPagedStorage.cpp
    UniqueLibs/qtPagedStorage.hpp
    UniqueLibs/qtItemOutcomeStore.cpp
    UniqueLibs/qtItemOutcomeStore.hpp
    UniqueLibs/qtViewerProvider.cpp
    UniqueLibs/qtViewerProvider.hpp
    UniqueLibs/qtViewerSurface.cpp
    UniqueLibs/qtViewerSurface.hpp
    UniqueLibs/qtViewerHost.cpp
    UniqueLibs/qtViewerHost.hpp
)
target_link_libraries(hc_viewer_qt PUBLIC hc_viewer_core hc_plugin_viewer_qt Qt6::Widgets)
target_compile_options(hc_viewer_qt PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

add_subdirectory(pluginHost)
add_subdirectory(HyperEditor)
qt_add_library(hc_document_open STATIC
    UniqueLibs/document/documentOpenController.cpp UniqueLibs/document/documentOpenController.hpp
    UniqueLibs/document/documentHandlerMenu.cpp UniqueLibs/document/documentHandlerMenu.hpp
    UniqueLibs/document/documentHandlerCatalog.cpp UniqueLibs/document/documentHandlerSettings.hpp
    UniqueLibs/nativeMaterializer.cpp)
target_include_directories(hc_document_open PRIVATE
    ${PROJECT_SOURCE_DIR}/HyperCommander ${PROJECT_SOURCE_DIR}/commonLibs
    ${PROJECT_SOURCE_DIR}/commonLibs/fmt-10.2.0/include)
target_link_libraries(hc_document_open PUBLIC hc_document_core hc_viewer_qt PRIVATE Qt6::Concurrent advapi32 shell32 ole32)
target_compile_options(hc_document_open PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_subdirectory(HyperCommanderBuiltInPlugin)
# HyperCommanderBuiltInPlugin/HyperViewer validates this pinned package during configure.
# HWP F3 uses the product SDI entry outside the UI-restricted plugin Job.
if(HC_RHWP_PRODUCT_RUNTIME_DIRECTORY AND HC_RHWP_APPROVED_INVENTORY_SHA256)
    target_compile_definitions(hc_document_open PRIVATE HC_RHWP_F3_RUNTIME=1)
    file(READ "${HC_RHWP_PRODUCT_RUNTIME_DIRECTORY}/runtime-inventory.json" HC_RHWP_PRODUCT_INVENTORY)
    string(JSON HC_RHWP_PRODUCT_PROFILE GET "${HC_RHWP_PRODUCT_INVENTORY}" profile)
    if(HC_RHWP_PRODUCT_PROFILE STREQUAL "native-editor")
        target_compile_definitions(hc_document_open PRIVATE HC_RHWP_F4_RUNTIME=1)
    endif()
endif()
qt_add_executable(hc-search-index tools/searchIndexMain.cpp)
target_link_libraries(hc-search-index PRIVATE Qt6::Core "${PROJECT_SOURCE_DIR}/externalLibs/Everything/src/lib/Everything64.lib")
target_compile_options(hc-search-index PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_custom_command(TARGET hc-search-index POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/externalLibs/Everything/src/dll/Everything64.dll" "$<TARGET_FILE_DIR:hc-search-index>"
    VERBATIM
)
hc_enable_qt_deployment(hc-search-index)
add_subdirectory(HyperCommanderAdmin)
add_subdirectory(HyperCommander)
add_dependencies(HyperCommander hc-search-index HyperCommanderAdmin)
if(TARGET hc_smb_tests)
    add_dependencies(hc_smb_tests hc-search-index)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
option(HC_BUILD_SCRIPT_COMPARISON "Build optional runtime comparison executables" OFF)
if(HC_BUILD_SCRIPT_COMPARISON)
    add_subdirectory(tools/runtimeComparison)
endif()
hc_finalize_qt_deployment()
add_dependencies(hc_deploy hc_iso_package hc_hyperviewer_package)

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    # Build the maintained .vcxproj, not the generated CMake project. Keep its
    # objects, output, and CMake-built dependencies separate from normal builds.
    add_custom_target(hc_native_vs_product
        COMMAND "${CMAKE_VS_MSBUILD_COMMAND}" "${PROJECT_SOURCE_DIR}/HyperCommander.sln"
            /t:Build "/p:Configuration=$<CONFIG>" /p:Platform=x64 /m:4 /p:CL_MPCount=4
            "/p:QtInstall=$<TARGET_FILE_DIR:Qt6::Core>/.."
            "/p:OutDir=${CMAKE_BINARY_DIR}/native-vs/$<CONFIG>/"
            "/p:IntDir=${CMAKE_BINARY_DIR}/native-vs-obj/$<CONFIG>/"
            "/p:HcNativePluginBuild=${CMAKE_BINARY_DIR}/native-vs-dependencies/$<CONFIG>"
        USES_TERMINAL VERBATIM)
endif()

# Keep the statically linked style's license beside both deployment entry points.
add_custom_command(TARGET hc_deploy POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:HyperCommander>/Licenses/waterColorStyle"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/externalLibs/waterColorStyle/LICENSE"
        "$<TARGET_FILE_DIR:HyperCommander>/Licenses/waterColorStyle/LICENSE"
    VERBATIM)
# Both deployment entry points consume the same hashed runtime inventory.
add_custom_target(hc_product_inventory ALL
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE=$<TARGET_FILE_DIR:HyperCommander>" "-DCONFIG=$<CONFIG>"
        "-DRHWP_SOURCE=${HC_RHWP_PRODUCT_RUNTIME_DIRECTORY}"
        "-DRHWP_PIN=${HC_RHWP_APPROVED_INVENTORY_SHA256}"
        -P "${PROJECT_SOURCE_DIR}/cmake/GenerateProductInventory.cmake"
    DEPENDS hc_deploy VERBATIM)
configure_file(cmake/InstallProductInventory.cmake.in "${CMAKE_BINARY_DIR}/InstallProductInventory.cmake" @ONLY)
install(SCRIPT "${CMAKE_BINARY_DIR}/InstallProductInventory.cmake" COMPONENT Runtime)
