include_directories( ${CMAKE_CURRENT_LIST_DIR} )

set(EXTERNAL_SRC "")

# Everything SDK
include_directories( ${CMAKE_CURRENT_LIST_DIR}/Everything/src/include )
include_directories( ${CMAKE_CURRENT_LIST_DIR}/Everything/src/ipc )

link_directories( ${CMAKE_CURRENT_LIST_DIR}/Everything/src/lib )
link_libraries( Everything64 )

# QDirectoryEdit
set( EXTERNAL_SRC ${EXTERNAL_SRC}
        ../externalLibs/QDirectoryEdit/QDirectoryEdit.cpp
        ../externalLibs/QDirectoryEdit/QDirectoryEdit.hpp
)

# QTitanDataGrid
add_compile_definitions( QTN_NAMESPACE )
add_compile_definitions( QTITAN_LIBRARY_STATIC )
add_compile_definitions( QTN_PRIVATE_HEADERS )

include_directories( ${CMAKE_CURRENT_LIST_DIR}/QtitanDataGrid/src/include )
include_directories( ${CMAKE_CURRENT_LIST_DIR}/QtitanDataGrid/src/src/base )

set( EXTERNAL_SRC ${EXTERNAL_SRC}
        ../externalLibs/QtitanDataGrid/src/src/base/QtitanDef.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtitanMSVSDebug.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnAbstractScrollArea.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnAbstractScrollArea.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnAbstractTabBar.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnAbstractTabBar.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnBackstageWidget.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnBackstageWidget.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnBaseResources.qrc
        ../externalLibs/QtitanDataGrid/src/src/base/QtnColorButton.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnColorButton.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnCommonStyle.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnCommonStyle.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnCommonStyle_win.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnCommonStylePrivate.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnCustomLayout.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnCustomLayout.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnMaterialWidget.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnMaterialWidget.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnPlatform.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnPlatform.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnPlatform_win.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnPlatform_win.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnPopupMenu.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnPopupMenu.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnPopupMenuPrivate.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnScrollWidgetBar.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnScrollWidgetBar.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnSegoeMDL2Assets.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnSegoeMDL2Assets.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnStyleHelper.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnStyleHelperPrivate.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnStyleOption.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnStyleOption.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnToggleSwitch.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnToggleSwitch.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnToolTip.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnToolTip.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnWindowsColor.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnWindowsColor.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnWindowTitleBar.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnWindowTitleBar.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnWindowTitleBar_win.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnWindowTitleBar_win.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnWindowTitleBarGlow_win.cpp
        ../externalLibs/QtitanDataGrid/src/src/base/QtnWindowTitleBarGlow_win.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnWindowTitleBarPrivate.h
        ../externalLibs/QtitanDataGrid/src/src/base/QtnXML.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnCardGrid.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnCardGrid.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGrid.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGrid.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridBandedTableView.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridBandedTableView.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridBase.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridBase.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridCardView.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridCardView.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridCardViewPrivate.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridCardViewPrivate.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridColumnHeaderLayout.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridColumnHeaderLayout.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridControls.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridControls.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridDef.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridDef.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridDragObjects.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridDragObjects.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridEditors.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridEditors.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridFilter.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridFilter.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridFilterWindow.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridFilterWindow.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridGraphics.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridGraphics.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridModelController.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridModelController.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridModelControllerPrivate.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridResources.qrc
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridRowsLayout.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridRowsLayout.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridSQL.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridSQL.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridStyleSheet.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridStyleSheet.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridTableView.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridTableView.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridTableViewPrivate.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridTableViewPrivate.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridTranslations.qrc
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridTreeView.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridTreeView.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridTreeViewPrivate.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridViewGraphics.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnGridViewGraphics.h
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnTreeGrid.cpp
        ../externalLibs/QtitanDataGrid/src/src/grid/QtnTreeGrid.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnAdobePhotoshopStyle.cpp
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnAdobePhotoshopStyle.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnAdobePhotoshopStyle.qrc
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnFluentDesignStyle.cpp
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnFluentDesignStyle.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnFluentDesignStylePrivate.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2007Style.cpp
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2007Style.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2007Style.qrc
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2007StylePrivate.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2010Style.cpp
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2010Style.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2010Style.qrc
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2010StylePrivate.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2013Style.cpp
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2013Style.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2013Style.qrc
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2013StylePrivate.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2016Style.cpp
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2016Style.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOffice2016Style.qrc
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOfficeStyle.cpp
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOfficeStyle.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnOfficeStylePrivate.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnStyleDef.cpp
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnStyleDef.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnVisualStudio2019Style.cpp
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnVisualStudio2019Style.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnVisualStudio2019Style.qrc
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnVisualStudio2019StylePrivate.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnWindows7ScenicStyle.cpp
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnWindows7ScenicStyle.h
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnWindows7ScenicStyle.qrc
        ../externalLibs/QtitanDataGrid/src/src/styles/QtnWindows7ScenicStylePrivate.h
)

# TotalCommander
include_directories( ${CMAKE_CURRENT_LIST_DIR}/TotalCommander )
set( EXTERNAL_SRC ${EXTERNAL_SRC}
        ../externalLibs/TotalCommander/tc_listplug.h
)

