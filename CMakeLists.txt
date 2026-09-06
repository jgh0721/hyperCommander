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

add_subdirectory(HyperCommander)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
hc_finalize_qt_deployment()
