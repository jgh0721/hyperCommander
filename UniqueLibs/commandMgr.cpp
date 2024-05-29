#include "stdafx.h"
#include "commandMgr.hpp"

/// UIs
#include "UIs/dlgMain.hpp"
#include "UIs/cmpPanel.hpp"

/// 공통 라이브러리
#include "cmnBase.hpp"

/// 외부 라이브러리
#include <externalLibs/QtitanDataGrid/src/src/grid/QtnGridBase.h>

#include "builtInFsModel.hpp"

#include "ShlObj.h"

#define GetMainUIPtr() ( ( QMainUI* )MainUI_ )

using namespace nsCmn;
using namespace nsCmn::nsCmnQt;

////////////////////////////////////////////////////////////////////////////////

CCommandMgr::CCommandMgr( QObject* parent )
    : QObject( parent )
{
    SpMapKeyToCMDText = std::make_shared< TyMapShortcutToCMDStr >();
}

void CCommandMgr::Refresh()
{
    for( const auto& Key : GlobalBuiltInCMDs )
        (*SpMapKeyToCMDText)[ Key.Default ] = Key.Name;

    // TODO: 환경설정 부분이 완료되면 이 부분에 환경설정에서 값을 읽어서 채운다.
}

QWidget* CCommandMgr::GetMainUI() const
{
    return MainUI_;
}

void CCommandMgr::SetMainUI( QWidget* MainUI )
{
    MainUI_ = MainUI;
}

bool CCommandMgr::ProcessKeyPressEvent( QKeyEvent* KeyEvent, const QModelIndex& CursorIndex )
{
    auto KtoCMD = retrieve();
    const auto KCombined = KeyEvent->keyCombination();
    if( KtoCMD->contains( KCombined ) == false )
        return false;

    const auto CMD = KtoCMD->value( KCombined );
    Q_ASSERT( CMD.isEmpty() == false );
    KeyEvent->setAccepted( QMetaObject::invokeMethod( this, CMD.data(), Qt::QueuedConnection, CursorIndex ) );

    return KeyEvent->isAccepted();
}

////////////////////////////////////////////////////////////////////////////
/// 명령 핸들러

DEFINE_HC_COMMAND( CCommandMgr, cm_NewTab )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_PrevTab )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_NextTab )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_CloseTab )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_Return )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_SelInverse )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_SwitchPanel )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_ContextMenu )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
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

CCommandMgr::TySpMapKeyToCMDStr CCommandMgr::retrieve()
{
    return SpMapKeyToCMDText;
}
