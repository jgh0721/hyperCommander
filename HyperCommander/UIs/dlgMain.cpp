#include "stdafx.h"
#include "dlgMain.hpp"

#include "UIs/dlgOpts.hpp"

QMainUI::QMainUI( QWidget* parent, Qt::WindowFlags flags )
    : QMainWindow( parent, flags )
{
    ui.setupUi( this );

    setWindowTitle( tr( "HyperCommander" ) );

    QMetaObject::invokeMethod( this, "initialize", Qt::QueuedConnection );

    connect( ui.cmpLeftPanel, &CmpPanel::sig_NotifyCurrentDirectory, this, &QMainUI::oo_notifyCurrentDirectory );

    // TODO: CmpPanel 에서 항목이 모두 새로고침이 끝났음을 알아내서 포커스를 부여해야 한다. 
    // 최초 실행되었을 때 커서가 준비되도록 하여 편의성을 향상시킨다. 
    QTimer::singleShot( 1000, [this]() {

        ui.cmpLeftPanel->setFocus( Qt::MouseFocusReason );
        QMetaObject::invokeMethod( ui.cmpLeftPanel, "SetFocusView", Q_ARG( int, ui.cmpLeftPanel->CurrentTabIndex() ) );

                        } );

}

void QMainUI::Tab_SwitchToAnother()
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
    
    const auto q = ui.cmpLeftPanel->hasFocus();
    const auto s = ui.cmpRightPanel->hasFocus();

    int a = 0; 
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

void QMainUI::initialize()
{

}

CmpPanel* QMainUI::currentFocusPanel() const
{
    if( currentPanelIndex == 0 )
        return ui.cmpLeftPanel;

    if( currentPanelIndex == 1 )
        return ui.cmpRightPanel;

    return nullptr;
}
