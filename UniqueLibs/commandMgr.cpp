#include "stdafx.h"
#include "commandMgr.hpp"

/// UIs
#include "UIs/dlgMain.hpp"
#include "UIs/cmpPanel.hpp"

/// 외부 라이브러리
#include <externalLibs/QtitanDataGrid/src/src/grid/QtnGridBase.h>

#include "builtInFsModel.hpp"

#include "ShlObj.h"

#define GetMainUIPtr() ( ( QMainUI* )MainUI_ )

void CCommandMgr::Refresh()
{

}

QWidget* CCommandMgr::GetMainUI() const
{
    return MainUI_;
}

void CCommandMgr::SetMainUI( QWidget* MainUI )
{
    MainUI_ = MainUI;
}

bool CCommandMgr::ProcessKeyPressEvent( QKeyEvent* KeyEvent )
{
    return KeyEvent->isAccepted();
}

void CCommandMgr::CMD_Return( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex )
{
    const auto ProxyModel   = qobject_cast< FSProxyModel* >( View->model() );
    const auto FsModel      = qobject_cast< FSModel* >( ProxyModel->sourceModel() );
    
    const auto EntryInfo = FsModel->GetFileInfo( ProxyModel->mapToSource( SrcIndex ) );

    if( EntryInfo.Attiributes & FILE_ATTRIBUTE_DIRECTORY )
    {
        FsModel->ChangeDirectory( EntryInfo.Name );
    }
    else
    {
        // TODO: 해당 항목이 내부 진입이 가능한 파일인지 확인한 후 아닐 때 실행한다.

        CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE );
        std::wstring Src = FsModel->GetFileFullPath( ProxyModel->mapToSource( SrcIndex ) ).toStdWString();
        
        do
        {
            SHELLEXECUTEINFOW shinfo = { 0, };
            shinfo.cbSize = sizeof( shinfo );
            shinfo.nShow = SW_NORMAL;
            shinfo.lpVerb = L"open";
            shinfo.lpFile = Src.c_str();

            ShellExecuteExW( &shinfo );

        } while( false );

        CoUninitialize();
    }
}

void CCommandMgr::CMD_Space( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex )
{
    GetMainUIPtr()->SelectRowOnCurrentPane( SrcIndex, true );
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

void CCommandMgr::CMD_HidSys( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex )
{
    QMetaObject::invokeMethod( qApp, "ToggleHiddenSystem" );
}

void CCommandMgr::CMD_MultiRename( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex )
{
    const auto Selection = View->modelController()->selection();
    if( Selection->isEmpty() == true )
    {
        QMessageBox::information( nullptr, tr( "HyperCommander" ), tr( "이름을 변경할 파일을 선택해 주세요." ) );
        return;
    }

    QVector< QString > VecFiles;
    const auto ProxyModel = qobject_cast< FSProxyModel* >( View->model() );
    const auto FsModel = qobject_cast< FSModel* >( ProxyModel->sourceModel() );

    for( const auto& Item : Selection->selectedRowIndexes() )
    {
        qDebug() << FsModel->GetFileFullPath( ProxyModel->mapToSource( Item ) );
        VecFiles.push_back( FsModel->GetFileFullPath( ProxyModel->mapToSource( Item ) ) );
    }
    
    QMetaObject::invokeMethod( qApp, "ShowMultiRename", Q_ARG( const QVector< QString >&, VecFiles ) );
}
