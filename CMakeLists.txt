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
    Network
    UiTools
    PrintSupport
    Widgets
    WidgetsPrivate
)
qt_standard_project_setup(REQUIRES 6.12)

include_directories( ${CMAKE_CURRENT_LIST_DIR} )
include_directories( ${CMAKE_CURRENT_LIST_DIR}/UniqueLibs )
include(cmake/QtDeployment.cmake)

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
add_subdirectory(HyperCommander)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
hc_finalize_qt_deployment()
add_dependencies(hc_deploy hc_iso_package)
