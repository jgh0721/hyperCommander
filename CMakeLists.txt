cmake_minimum_required(VERSION 4.2)
set(HC_PRODUCT_VERSION "2026.10.28.1")
project(HyperCommander VERSION ${HC_PRODUCT_VERSION} LANGUAGES CXX RC)

include(cmake/build.version.cmake)
hc_split_version("${HC_PRODUCT_VERSION}" HC_VER_MAJOR HC_VER_MINOR HC_VER_PATCH HC_VER_TWEAK)
configure_file("${PROJECT_SOURCE_DIR}/UniqueLibs/solTypeDefs_Ver.hpp.in"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/solTypeDefs_Ver.hpp" @ONLY NEWLINE_STYLE CRLF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTORCC ON)
if(MSVC)
    # Keep matching Release linker symbols separately from the portable runtime.
    add_link_options("$<$<CONFIG:Release>:/DEBUG>" "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
endif()
# qt_add_executable applies the selected kit's Hybrid CRT policy.

option(HC_BUILD_HYPERCOMPARE_ONLY "Configure only the standalone compare utility" OFF)
if(HC_BUILD_HYPERCOMPARE_ONLY)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets Svg Concurrent Network)
    qt_standard_project_setup(REQUIRES 6.12)
    include(CTest)
    set(FMSTYLE_WITH_QTITAN OFF CACHE BOOL "" FORCE)
    foreach(Option IN ITEMS DEMO LABS GALLERY DESIGNER_PLUGIN EXAMPLES TESTS)
        set(FMSTYLE_BUILD_${Option} OFF CACHE BOOL "" FORCE)
    endforeach()
    add_subdirectory(externalLibs/waterColorStyle watercolor EXCLUDE_FROM_ALL)
    include(cmake/TextEditorEngines.cmake)
    include(cmake/QtDeployment.cmake)
    add_subdirectory(HyperCompare)
    hc_finalize_qt_deployment()
    return()
endif()

find_package(Qt6 6.12 REQUIRED COMPONENTS
    Core
    CorePrivate
    Concurrent
    Gui
    GuiPrivate
    Sql
    Svg
    Xml
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
include(cmake/ImageCodecs.cmake)
include(cmake/QtDeployment.cmake)
include(cmake/ProductLayout.cmake)
include(cmake/ProductSigning.cmake)
option(HC_PRODUCT_SIGNING "Require code signatures on distributed Release products" ON)
set(HC_SIGNING_CACHE "${PROJECT_SOURCE_DIR}/_build/signing-cache" CACHE PATH "Verified canonical code signing cache")
set(HC_SIGNING_SCRIPT "${PROJECT_SOURCE_DIR}/tools/update/Sign-ProductFile.ps1" CACHE FILEPATH "Product signer for isolated worktree builds")
set(HC_PACKAGE_SIGN_ARGS "-DCONFIG=$<CONFIG>")
if(HC_PRODUCT_SIGNING)
    list(APPEND HC_PACKAGE_SIGN_ARGS
        "-DHC_SIGNING_SCRIPT=${HC_SIGNING_SCRIPT}"
        "-DHC_SIGNING_CACHE=${HC_SIGNING_CACHE}")
endif()
set(HC_SELECTED_SCRIPT_RUNTIME "sol2")
add_subdirectory(commonLibs/scripting)
add_subdirectory(UniqueLibs/passwords)

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
target_link_libraries(hc_viewer_qt PUBLIC hc_viewer_core hc_plugin_viewer_qt Qt6::Widgets PRIVATE Fm::widgets)
target_compile_options(hc_viewer_qt PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

add_subdirectory(pluginHost)
include(cmake/TextEditorEngines.cmake)
add_subdirectory(HyperEditor)
qt_add_library(hc_document_open STATIC
    UniqueLibs/document/documentOpenController.cpp UniqueLibs/document/documentOpenController.hpp
    UniqueLibs/document/documentHandlerMenu.cpp UniqueLibs/document/documentHandlerMenu.hpp
    UniqueLibs/document/documentHandlerCatalog.cpp UniqueLibs/document/documentHandlerSettings.hpp
    UniqueLibs/nativeMaterializer.cpp)
target_include_directories(hc_document_open PRIVATE
    ${PROJECT_SOURCE_DIR}/HyperCommander ${PROJECT_SOURCE_DIR}/commonLibs
    ${PROJECT_SOURCE_DIR}/commonLibs/fmt-10.2.0/include)
target_link_libraries(hc_document_open PUBLIC hc_document_core hc_viewer_qt PRIVATE Fm::style hc_passwords Qt6::Concurrent advapi32 shell32 ole32)
target_compile_options(hc_document_open PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_subdirectory(HyperCommanderBuiltInPlugin)
add_subdirectory(UniqueLibs/archive)
add_subdirectory(HyperCommanderUpdater)
target_link_libraries(HyperEditor PRIVATE hc_update_core)
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
hc_add_version_resource(hc-search-index "HyperCommander Search Helper")
qt_add_library(hc_file_locks STATIC
    UniqueLibs/nativeFileLocks.cpp
    UniqueLibs/nativeFileLocks.hpp
    UniqueLibs/fileLockService.cpp
    UniqueLibs/fileLockService.hpp
)
target_include_directories(hc_file_locks PRIVATE
    ${PROJECT_SOURCE_DIR}/HyperCommanderAdmin
    ${PROJECT_SOURCE_DIR}/commonLibs
)
target_link_libraries(hc_file_locks PUBLIC Qt6::Core PRIVATE advapi32 shell32 rstrtmgr)
target_compile_definitions(hc_file_locks PRIVATE NOMINMAX UNICODE _UNICODE)
target_compile_options(hc_file_locks PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_subdirectory(HyperCommanderAdmin)
target_link_libraries(HyperCommanderAdmin PRIVATE hc_file_locks)
target_link_libraries(HyperCommanderAdmin PRIVATE hc_update_core)
add_subdirectory(HyperCommander)
foreach(HC_UPDATE_ENTRY IN ITEMS HyperCommander HyperCommanderAdmin HyperEditor)
    target_sources(${HC_UPDATE_ENTRY} PRIVATE "${PROJECT_SOURCE_DIR}/cmake/updateLongPaths.manifest")
endforeach()
add_dependencies(HyperCommander hc-search-index HyperCommanderAdmin)
if(TARGET hc_smb_tests)
    add_dependencies(hc_smb_tests hc-search-index)
endif()

include(CTest)
option(HC_BUILD_EPUB_POC "Build developer-only EPUB P0 validation tools" OFF)
option(HC_BUILD_EPUB_PRODUCT "Build authenticated EPUB product helper candidate" OFF)
if(HC_BUILD_EPUB_POC OR HC_BUILD_EPUB_PRODUCT)
    add_subdirectory(tools/epub)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
add_subdirectory(HyperCompare)
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
        "-DEPUB_SOURCE=${HC_EPUB_PRODUCT_RUNTIME_DIRECTORY}"
        "-DEPUB_PIN=${HC_EPUB_APPROVED_INVENTORY_SHA256}"
        "-DEPUB_PYTHON=${HC_EPUB_PYTHON_EXECUTABLE}"
        "-DHC_VIEWER=$<TARGET_FILE:HyperViewer>"
        "-DPRODUCT_VERSION=${HC_PRODUCT_VERSION}"
        ${HC_PACKAGE_SIGN_ARGS}
        -P "${PROJECT_SOURCE_DIR}/cmake/GenerateProductInventory.cmake"
    DEPENDS hc_deploy VERBATIM)
configure_file(cmake/InstallProductInventory.cmake.in "${CMAKE_BINARY_DIR}/InstallProductInventory.cmake" @ONLY)
install(SCRIPT "${CMAKE_BINARY_DIR}/InstallProductInventory.cmake" COMPONENT Runtime)
add_custom_target(hc_portable_package
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${PROJECT_SOURCE_DIR}"
        "-DROOT=$<TARGET_FILE_DIR:HyperCommander>" "-DOUTPUT=${CMAKE_BINARY_DIR}/packages/$<CONFIG>"
        "-DVERSION=${HC_PRODUCT_VERSION}" "-DSYMBOLS=${CMAKE_BINARY_DIR}/hc-symbols-$<CONFIG>.txt"
        -P "${PROJECT_SOURCE_DIR}/cmake/PackagePortable.cmake"
    DEPENDS hc_product_inventory USES_TERMINAL VERBATIM)
if(MSVC)
    set(HC_SYMBOL_FILES "")
    foreach(HC_SYMBOL_TARGET IN ITEMS HyperCommander HyperCommanderAdmin HyperEditor HyperViewer HyperCompare HyperCompareCli
            hc-plugin-host-x64 hc-plugin-inspect hc-search-index HyperCommanderUpdater hc_iso_core hc_isoimage_wcx hc_sevenzip_core)
        string(APPEND HC_SYMBOL_FILES "$<TARGET_PDB_FILE:${HC_SYMBOL_TARGET}>\n")
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/hc-symbols-$<CONFIG>.txt" CONTENT "${HC_SYMBOL_FILES}")
endif()
