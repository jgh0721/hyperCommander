#include "stdafx.h"
#include "dlgMain.hpp"

#include "UIs/dlgOpts.hpp"

#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/cmnTypeDefs.hpp"
#include "UniqueLibs/externalEditorMgr.hpp"

#include "UniqueLibs/builtInFsModel.hpp"

QMainUI::QMainUI( QWidget* parent, Qt::WindowFlags flags )
    : QMainWindow( parent, flags )
{
    ui.setupUi( this );

    setWindowTitle( tr( "HyperCommander" ) );
    
    QMetaObject::invokeMethod( this, "initialize", Qt::QueuedConnection );
}

////////////////////////////////////////////////////////////////////////////
/// 명령 핸들러

DEFINE_HC_COMMAND( QMainUI, cm_NewTab )
{
    // 탭을 추가하면서 Qt 가 자동적으로 엉뚱한 곳으로 포커스를 옮기는 행동이 일어날 때가 있다. 이에 대해 보정한다. 
    const auto Prev = currentPanelIndex;
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->AddTab();
    currentPanelIndex = Prev;
}

DEFINE_HC_COMMAND( QMainUI, cm_PrevTab )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->PrevTab();
}

DEFINE_HC_COMMAND( QMainUI, cm_NextTab )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->NextTab();
}

DEFINE_HC_COMMAND( QMainUI, cm_CloseTab )
{
    const auto Prev = currentPanelIndex;
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->CloseTab();
    currentPanelIndex = Prev;
}

DEFINE_HC_COMMAND( QMainUI, cm_CopyOtherPanel )
{
    const auto Src = retrieveSrcPanel();
    const auto Dst = retrieveDstPanel();
    Q_ASSERT( Src != nullptr );
    Q_ASSERT( Dst != nullptr );

    if( Src == nullptr || Dst == nullptr )
        return;

    Src->FileCopyToOtherPanel( Dst );
}

DEFINE_HC_COMMAND( QMainUI, cm_RenameSingleFile )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->RenameFileName( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_List )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->ViewOnLister( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_Return )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->ReturnOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_MkDir )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->NewFolderOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_Delete )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->FileDeleteOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_NameNormalization )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->FileNormalization( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_SelInverse )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;
    
    Pane->SelectRowOnCurrentTab( CursorIndex, true );
}

DEFINE_HC_COMMAND( QMainUI, cm_MultiRenameFiles )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    const auto View = Pane->retrieveFocusView();
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

DEFINE_HC_COMMAND( QMainUI, cm_SwitchHidSys )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    const auto State = Pane->retrieveFocusState();
    bool IsShow = false;

    QMetaObject::invokeMethod( ( QObject* )State->ProxyModel, "GetHiddenSystem", qReturnArg( IsShow ) );
    QMetaObject::invokeMethod( ( QObject* )State->ProxyModel, "SetHiddenSystem", !IsShow );
}

DEFINE_HC_COMMAND( QMainUI, cm_RereadSource )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->RefreshSource( Pane->CurrentTabIndex() );
}

DEFINE_HC_COMMAND( QMainUI, cm_SwitchPanel )
{
    if( currentPanelIndex == 0 )
    {
        ui.cmpRightPanel->setFocus( Qt::MouseFocusReason );
        ui.cmpRightPanel->SetFocusView( ui.cmpRightPanel->CurrentTabIndex() );
        currentPanelIndex = 1;
    }
    else if( currentPanelIndex == 1 )
    {
        ui.cmpLeftPanel->setFocus( Qt::MouseFocusReason );
        ui.cmpLeftPanel->SetFocusView( ui.cmpLeftPanel->CurrentTabIndex() );
        currentPanelIndex = 0;
    }
}

DEFINE_HC_COMMAND( QMainUI, cm_ContextMenu )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->ContextMenuOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_ExternalEditorMenu )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->ExternalEditorMenu( CursorIndex );
}

void QMainUI::on_acShowMainOpts_triggered( bool checked )
{
    QMainOpts Opts;

    Opts.exec();

}

void QMainUI::oo_notifyCurrentDirectory( const QString& CurrentPath )
{
    ui.lblPrompt->setText( CurrentPath + ">" );
}

void QMainUI::oo_notifyPanelActivated()
{
    const auto Sender = qobject_cast< CmpPanel* >( sender() );

    if( Sender == ui.cmpLeftPanel )
    {
        qDebug() << "왼쪽 패널 활성화";
        ui.cmpLeftPanel->EnsureKeyboardFocusOnView( ui.cmpLeftPanel->GetFocusView() );

        currentPanelIndex = 0;
    }
    else if( Sender == ui.cmpRightPanel )
    {
        qDebug() << "오른쪽 패널 활성화";
        ui.cmpRightPanel->EnsureKeyboardFocusOnView( ui.cmpRightPanel->GetFocusView() );

        currentPanelIndex = 1;
    }
}

bool QMainUI::eventFilter( QObject* Object, QEvent* Event )
{
    if( Event->type() != QEvent::FocusAboutToChange )
        return false;

    const auto FocusEvent = ( QFocusEvent* )Event;
    if( FocusEvent->gotFocus() == false )
        return false;

    const auto Grid = qobject_cast< Qtitan::Grid* >( Object );
    if( Grid != nullptr )
    {

    }

    return false;
}

void QMainUI::closeEvent( QCloseEvent* Event )
{
    StSettings->beginGroup( OPT_SEC_WINDOW );
    StSettings->setValue( "geometry", saveGeometry() );
    StSettings->endGroup();

    QMainWindow::closeEvent( Event );
}

void QMainUI::initialize()
{
    auto HelpBar = new QMenuBar;
    auto HelpBar_Root = new QMenu( tr( "도움말(&H)" ), HelpBar );
    HelpBar->addMenu( HelpBar_Root );
    ui.menubar->setCornerWidget( HelpBar );

    TyStCommandMgr::GetInstance()->SetMainUI( this );
    TyStExternalEditorMgr::GetInstance()->Refresh();

    connect( ui.cmpLeftPanel, &CmpPanel::sig_NotifyCurrentDirectory, this, &QMainUI::oo_notifyCurrentDirectory );
    connect( ui.cmpRightPanel, &CmpPanel::sig_NotifyCurrentDirectory, this, &QMainUI::oo_notifyCurrentDirectory );
    connect( ui.cmpLeftPanel, &CmpPanel::sig_NotifyPanelActivated, this, &QMainUI::oo_notifyPanelActivated );
    connect( ui.cmpRightPanel, &CmpPanel::sig_NotifyPanelActivated, this, &QMainUI::oo_notifyPanelActivated );

    ui.cmpLeftPanel->installEventFilter( this );
    ui.cmpRightPanel->installEventFilter( this );

    // NOTE: 시작하면 왼쪽에 커서가 위치할 수 있도록 오른쪽부터 초기화한다.
    QMetaObject::invokeMethod( ui.cmpRightPanel, "Initialize", Qt::QueuedConnection );
    QMetaObject::invokeMethod( ui.cmpLeftPanel, "Initialize", Qt::QueuedConnection );
}

CmpPanel* QMainUI::retrieveSrcPanel() const
{
    qDebug() << __FUNCTION__ << " : 현재 패널 번호 " << currentPanelIndex;

    if( currentPanelIndex == 0 )
        return ui.cmpLeftPanel;

    if( currentPanelIndex == 1 )
        return ui.cmpRightPanel;

    return nullptr;
}

CmpPanel* QMainUI::retrieveDstPanel() const
{
    qDebug() << __FUNCTION__ << " : 현재 패널 번호 " << currentPanelIndex;

    if( currentPanelIndex == 0 )
        return ui.cmpRightPanel;

    if( currentPanelIndex == 1 )
        return ui.cmpLeftPanel;

    return nullptr;
}
