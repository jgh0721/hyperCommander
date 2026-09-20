cmake_minimum_required(VERSION 4.2)
project(HyperCommander LANGUAGES CXX)

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
include(cmake/HyperCommanderQlementine.cmake)
include(cmake/QtDeployment.cmake)
set(HC_SELECTED_SCRIPT_RUNTIME "sol2")
add_subdirectory(commonLibs/scripting)

set(HC_QTITAN_PACKAGE "" CACHE FILEPATH "Licensed QtitanDataGrid 9.3.0 ZIP for local reprovisioning")
set(HC_QTITAN_PRIVATE_PATCH "${PROJECT_SOURCE_DIR}/externalLibs/QtitanDataGrid/.hc-patches/qtitan-9.3-hc.zip"
    CACHE FILEPATH "Private Qtitan extension package; never distribute or commit")
if(HC_QTITAN_PACKAGE)
    find_program(HC_QTITAN_POWERSHELL NAMES powershell REQUIRED)
    add_custom_target(hc_install_qtitan
        COMMAND "${HC_QTITAN_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tools/Install-QtitanDataGrid.ps1"
            -PackagePath "${HC_QTITAN_PACKAGE}" -PrivatePatchPath "${HC_QTITAN_PRIVATE_PATCH}"
        COMMENT "Provision licensed Qtitan source and verified private extensions locally"
        VERBATIM
    )
endif()


qt_add_library(hc_viewer_core STATIC
    commonLibs/viewer/viewer.cpp
    commonLibs/viewer/viewer.hpp
    commonLibs/operations/jobScheduler.cpp
    commonLibs/operations/jobScheduler.hpp
)
# The core uses the kit's CRT settings but has no Qt headers or linkage.
set_target_properties(hc_viewer_core PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(hc_viewer_core PUBLIC ${PROJECT_SOURCE_DIR})
target_compile_options(hc_viewer_core PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
qt_add_library(hc_viewer_qt STATIC
    UniqueLibs/qtJobJournal.cpp
    UniqueLibs/qtJobJournal.hpp
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
add_subdirectory(BuiltInPlugin)
qt_add_executable(hc-search-index tools/searchIndexMain.cpp)
target_link_libraries(hc-search-index PRIVATE Qt6::Core "${PROJECT_SOURCE_DIR}/externalLibs/Everything/src/lib/Everything64.lib")
target_compile_options(hc-search-index PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
add_custom_command(TARGET hc-search-index POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/externalLibs/Everything/src/dll/Everything64.dll" "$<TARGET_FILE_DIR:hc-search-index>"
    VERBATIM
)
hc_enable_qt_deployment(hc-search-index)
add_subdirectory(HyperCommander)
add_dependencies(HyperCommander hc-search-index)
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
add_dependencies(hc_deploy hc_iso_package)
add_custom_command(TARGET hc_deploy POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DHC_QLEMENTINE_ROOT=${PROJECT_SOURCE_DIR}/externalLibs/qlementine"
        "-DHC_QLEMENTINE_DESTINATION=$<TARGET_FILE_DIR:HyperCommander>/licenses/qlementine"
        -P "${PROJECT_SOURCE_DIR}/cmake/DeployQlementine.cmake"
    VERBATIM)
