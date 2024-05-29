#include "stdafx.h"
#include "dlgMain.hpp"

#include "UIs/dlgOpts.hpp"

#include "UniqueLibs/commandMgr.hpp"

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
    const auto Pane = currentFocusPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->AddTab();
}

DEFINE_HC_COMMAND( QMainUI, cm_PrevTab )
{
    const auto Pane = currentFocusPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->PrevTab();
}

DEFINE_HC_COMMAND( QMainUI, cm_NextTab )
{
    const auto Pane = currentFocusPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->NextTab();
}

DEFINE_HC_COMMAND( QMainUI, cm_CloseTab )
{
    const auto Pane = currentFocusPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->CloseTab();
}

DEFINE_HC_COMMAND( QMainUI, cm_Return )
{
    const auto Pane = currentFocusPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->ReturnOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_SelInverse )
{
    const auto Pane = currentFocusPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;
    
    Pane->SelectRowOnCurrentTab( CursorIndex, true );
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
    const auto Pane = currentFocusPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->SelectRowOnCurrentTab( CursorIndex, true );
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

void QMainUI::initialize()
{
    TyStCommandMgr::GetInstance()->SetMainUI( this );

    connect( ui.cmpLeftPanel, &CmpPanel::sig_NotifyCurrentDirectory, this, &QMainUI::oo_notifyCurrentDirectory );
    connect( ui.cmpRightPanel, &CmpPanel::sig_NotifyCurrentDirectory, this, &QMainUI::oo_notifyCurrentDirectory );

    ui.cmpLeftPanel->installEventFilter( this );
    ui.cmpRightPanel->installEventFilter( this );

    // NOTE: 시작하면 왼쪽에 커서가 위치할 수 있도록 오른쪽부터 초기화한다. 
    ui.cmpRightPanel->Initialize();
    ui.cmpLeftPanel->Initialize();
}

CmpPanel* QMainUI::currentFocusPanel() const
{
    if( currentPanelIndex == 0 )
        return ui.cmpLeftPanel;

    if( currentPanelIndex == 1 )
        return ui.cmpRightPanel;

    return nullptr;
}
