#include "stdafx.h"
#include "commandMgr.hpp"

#include <externalLibs/QtitanDataGrid/src/src/grid/QtnGridBase.h>

#include "builtInFsModel.hpp"

void CCommandMgr::Refresh()
{

}

void CCommandMgr::CMD_Return( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex )
{
    const auto FsModel   = static_cast< FSModel* >( View->model() );
    const auto EntryInfo = FsModel->GetFileInfo( SrcIndex );

    if( EntryInfo.Attiributes & FILE_ATTRIBUTE_DIRECTORY )
    {
        FsModel->ChangeDirectory( EntryInfo.Name );
    }

    // TODO: 해당 항목이 내부 진입이 가능한 파일인지 확인한 후 아닐 때 실행한다.

}

void CCommandMgr::CMD_Space( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex )
{
    View->selectRow( View->getRow( SrcIndex ).rowIndex(), Qtitan::Invert );
    View->navigateDown( Qt::NoModifier );
}

void CCommandMgr::CMD_TabSwitch( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex )
{
    // TODO: 더 나은 방법 찾아보기

    QString ObjName;

    QObject* Parent = View;
    while( Parent != nullptr && 
           Parent->objectName().compare( "dlgMain" ) != 0 )
    {
        Parent = Parent->parent();
    }

    if( Parent != nullptr )
    {
        QMetaObject::invokeMethod( Parent, "Tab_SwitchToAnother" );
    }
}
