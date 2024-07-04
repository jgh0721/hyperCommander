cmake_minimum_required(VERSION 3.16)
project(HyperCommander LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find includes in the build directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_PREFIX_PATH D:/Qt/6.8.0/msvc2022_64)

add_compile_definitions( QT_NO_DEPRECATED_WARNINGS )

include( qt.cmake )
include( ../UniqueLibs/build.common.cmake )

qt_standard_project_setup()

find_package(QT NAMES Qt6 REQUIRED COMPONENTS Core)

find_package(Qt${QT_VERSION_MAJOR}
        COMPONENTS
        Core
        Concurrent
        Gui
        Sql
        Network
        UiTools
        PrintSupport
        Widgets
)

include_directories(${Qt${QT_VERSION_MAJOR}Core_PRIVATE_INCLUDE_DIRS})
include_directories(${Qt${QT_VERSION_MAJOR}Gui_PRIVATE_INCLUDE_DIRS})
include_directories(${Qt${QT_VERSION_MAJOR}Widgets_PRIVATE_INCLUDE_DIRS})

include( ../commonLibs/commonLibs.cmake )
include( ../externalLibs/externalLibs.cmake )

include_directories( ${CMAKE_CURRENT_LIST_DIR}/Modules )
include_directories( ${CMAKE_CURRENT_LIST_DIR}/UIs )

set(PROJECT_SOURCES
        cmnConfig.hpp
        HyperCommander.cpp
        HyperCommander.hpp
        HyperCommander.qrc
        HyperCommander.rc
        main.cpp
        resource.h
        stdafx.h
)

set(PROJECT_MODULE_SRC
        Modules/mdlFileEngine.cpp
        Modules/mdlFileEngine.hpp
        Modules/mdlFileOperation.cpp
        Modules/mdlFileOperation.hpp
)

set(PROJECT_UI_SRC
        UIs/cmnUiUtils.cpp
        UIs/cmnUiUtils.hpp
        UIs/cmpPanel.cpp
        UIs/cmpPanel.hpp
        UIs/cmpPanel.ui
        UIs/cmpViewer.cpp
        UIs/cmpViewer.hpp
        UIs/cmpViewer.ui
        UIs/dlgDefaultCMD.cpp
        UIs/dlgDefaultCMD.hpp
        UIs/dlgDefaultCMD.ui
        UIs/dlgExternalConfigure.cpp
        UIs/dlgExternalConfigure.hpp
        UIs/dlgExternalConfigure.ui
        UIs/dlgExternalConfigure_Item.cpp
        UIs/dlgExternalConfigure_Item.hpp
        UIs/dlgExternalConfigure_Item.ui
        UIs/dlgGridStyle.cpp
        UIs/dlgGridStyle.hpp
        UIs/dlgGridStyle.ui
        UIs/dlgMain.cpp
        UIs/dlgMain.hpp
        UIs/dlgMain.ui
        UIs/dlgMultiRename.cpp
        UIs/dlgMultiRename.hpp
        UIs/dlgMultiRename.ui
        UIs/dlgMultiRename_Model.cpp
        UIs/dlgMultiRename_Model.hpp
        UIs/dlgOpts.cpp
        UIs/dlgOpts.hpp
        UIs/dlgOpts.ui
        UIs/dlgViewer.cpp
        UIs/dlgViewer.hpp
        UIs/dlgViewer.ui
        "UIs/Cate - FileOperation/dlgCreateDirectory.cpp"
        "UIs/Cate - FileOperation/dlgCreateDirectory.hpp"
        "UIs/Cate - FileOperation/dlgCreateDirectory.ui"
        "UIs/Cate - FileOperation/dlgFileAttrib.cpp"
        "UIs/Cate - FileOperation/dlgFileAttrib.hpp"
        "UIs/Cate - FileOperation/dlgFileAttrib.ui"
        "UIs/Cate - FileOperation/dlgFileCopy.cpp"
        "UIs/Cate - FileOperation/dlgFileCopy.hpp"
        "UIs/Cate - FileOperation/dlgFileCopy.ui"
        "UIs/Cate - FileOperation/dlgFileDelete.cpp"
        "UIs/Cate - FileOperation/dlgFileDelete.hpp"
        "UIs/Cate - FileOperation/dlgFileDelete.ui"
        "UIs/Cate - FileOperation/dlgFileMove.cpp"
        "UIs/Cate - FileOperation/dlgFileMove.hpp"
        "UIs/Cate - FileOperation/dlgFileMove.ui"
        "UIs/Cate - FileOperation/dlgFileProgress.cpp"
        "UIs/Cate - FileOperation/dlgFileProgress.hpp"
        "UIs/Cate - FileOperation/dlgFileProgress.ui"
)

set(UNIQUELIBS_SRC
        ../UniqueLibs/backgroundJobMgr.cpp
        ../UniqueLibs/backgroundJobMgr.hpp
        ../UniqueLibs/builtInFsModel.cpp
        ../UniqueLibs/builtInFsModel.hpp
        ../UniqueLibs/cmnHCModelUtils.cpp
        ../UniqueLibs/cmnHCModelUtils.hpp
        ../UniqueLibs/cmnHCUtils.cpp
        ../UniqueLibs/cmnHCUtils.hpp
        ../UniqueLibs/cmnTypeDefs.hpp
        ../UniqueLibs/cmnTypeDefs_Name.hpp
        ../UniqueLibs/cmnTypeDefs_Opts.cpp
        ../UniqueLibs/cmnTypeDefs_Opts.hpp
        ../UniqueLibs/colorSchemeMgr.cpp
        ../UniqueLibs/colorSchemeMgr.hpp
        ../UniqueLibs/column_Defs.hpp
        ../UniqueLibs/columnMgr.cpp
        ../UniqueLibs/columnMgr.hpp
        ../UniqueLibs/commandMgr.cpp
        ../UniqueLibs/commandMgr.hpp
        ../UniqueLibs/commandMgr_Defs.hpp
        ../UniqueLibs/externalEditorMgr.cpp
        ../UniqueLibs/externalEditorMgr.hpp
        ../UniqueLibs/fileSetMgr.cpp
        ../UniqueLibs/fileSetMgr.hpp
        ../UniqueLibs/globalCtx.hpp
        ../UniqueLibs/model_defs.cpp
        ../UniqueLibs/model_defs.hpp
        ../UniqueLibs/pixmapCacheEx.cpp
        ../UniqueLibs/pixmapCacheEx.hpp
        ../UniqueLibs/SHChangeNotify.cpp
        ../UniqueLibs/SHChangeNotify.hpp
        ../UniqueLibs/solAdminWIthIPC.hpp
        ../UniqueLibs/solMultiLang_Defs.hpp
        ../UniqueLibs/solTCPluginMgr.cpp
        ../UniqueLibs/solTCPluginMgr.hpp
)
qt_add_executable(HyperCommander
        ${PROJECT_SOURCES}
        ${COMMONLIBS_SRC} ${EXTERNAL_SRC}
        ${PROJECT_MODULE_SRC}
        ${PROJECT_UI_SRC}
        ${UNIQUELIBS_SRC}
)

target_precompile_headers( HyperCommander PUBLIC stdafx.h )

set_target_properties(HyperCommander
        PROPERTIES
        WIN32_EXECUTABLE TRUE
)

target_link_libraries(HyperCommander
        PUBLIC
        Qt::Core
        Qt::Concurrent
        Qt::Gui
        Qt::Sql
        Qt::Network
        Qt::UiTools
        Qt::PrintSupport
        Qt::Widgets
)
