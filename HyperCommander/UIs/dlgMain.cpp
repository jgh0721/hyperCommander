#include "stdafx.h"
#include "dlgMain.hpp"

#include "UIs/dlgOpts.hpp"

QMainUI::QMainUI( QWidget* parent, Qt::WindowFlags flags )
    : QMainWindow( parent, flags )
{
    ui.setupUi( this );

    setWindowTitle( tr( "HyperCommander" ) );

    connect( ui.cmpLeftPanel, &CmpPanel::sig_NotifyCurrentDirectory, this, &QMainUI::oo_notifyCurrentDirectory );
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
