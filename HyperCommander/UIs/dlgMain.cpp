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

void QMainUI::on_acShowMainOpts_triggered( bool checked )
{
    QMainOpts Opts;

    Opts.exec();

}

void QMainUI::oo_notifyCurrentDirectory( const QString& CurrentPath )
{
    ui.lblPrompt->setText( CurrentPath + ">" );
}
