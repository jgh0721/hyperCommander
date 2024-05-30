﻿#include "stdafx.h"
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
    // 탭을 추가하면서 Qt 가 자동적으로 엉뚱한 곳으로 포커스를 옮기는 행동이 일어날 때가 있다. 이에 대해 보정한다. 
    const auto Prev = currentPanelIndex;
    const auto Pane = currentFocusPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->AddTab();
    currentPanelIndex = Prev;
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
    const auto Prev = currentPanelIndex;
    const auto Pane = currentFocusPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->CloseTab();
    currentPanelIndex = Prev;
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

    Pane->ContextMenuOnCurrentTab( CursorIndex );
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

void QMainUI::initialize()
{
    TyStCommandMgr::GetInstance()->SetMainUI( this );

    connect( ui.cmpLeftPanel, &CmpPanel::sig_NotifyCurrentDirectory, this, &QMainUI::oo_notifyCurrentDirectory );
    connect( ui.cmpRightPanel, &CmpPanel::sig_NotifyCurrentDirectory, this, &QMainUI::oo_notifyCurrentDirectory );
    connect( ui.cmpLeftPanel, &CmpPanel::sig_NotifyPanelActivated, this, &QMainUI::oo_notifyPanelActivated );
    connect( ui.cmpRightPanel, &CmpPanel::sig_NotifyPanelActivated, this, &QMainUI::oo_notifyPanelActivated );

    ui.cmpLeftPanel->installEventFilter( this );
    ui.cmpRightPanel->installEventFilter( this );

    // NOTE: 시작하면 왼쪽에 커서가 위치할 수 있도록 오른쪽부터 초기화한다. 
    ui.cmpRightPanel->Initialize();
    ui.cmpLeftPanel->Initialize();
}

CmpPanel* QMainUI::currentFocusPanel() const
{
    qDebug() << "현재 패널 번호 " << currentPanelIndex;

    if( currentPanelIndex == 0 )
        return ui.cmpLeftPanel;

    if( currentPanelIndex == 1 )
        return ui.cmpRightPanel;

    return nullptr;
}
