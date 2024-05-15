#include "stdafx.h"
#include "dlgOpts.hpp"

QMainOpts::QMainOpts( QWidget* parent, Qt::WindowFlags flags )
    : QDialog( parent, flags )
{
    ui.setupUi( this );

    ui.lstOptCate->addItem( tr( "화면" ) );
    ui.lstOptCate->addItem( tr( "\t색상" ) );

}

///////////////////////////////////////////////////////////////////////////////
/// TitleBar

void QMainOpts::on_btnClose_clicked( bool checked /* = false */ )
{
    on_btnCancel_clicked( checked );
}

void QMainOpts::on_btnFileListForground_clicked( bool checked )
{
    const auto Selected = QColorDialog::getColor( Qt::white, this );

    if( Selected.isValid() == true )
    {
        QPixmap px( 40, 40 );
        px.fill( Selected );
        ui.btnFileListForground->setIcon( px );
        ui.btnFileListForground->setAutoFillBackground( true );
        // ui.btnFileListForground->setPalette( QPalette( Selected ) );
        ui.btnFileListForground->setText( Selected.name() );
    }
}

void QMainOpts::on_btnFileListBackground_clicked( bool checked )
{
    const auto Selected = QColorDialog::getColor( Qt::white, this );

    if( Selected.isValid() == true )
    {
        QPixmap px( 40, 40 );
        px.fill( Selected );
        ui.btnFileListBackground->setIcon( px );
        ui.btnFileListBackground->setAutoFillBackground( true );
        // ui.btnFileListBackground->setPalette( QPalette( Selected ) );
        ui.btnFileListBackground->setText( Selected.name() );
    }
}

///////////////////////////////////////////////////////////////////////////////
/// Footer


void QMainOpts::on_btnOK_clicked( bool checked )
{}

void QMainOpts::on_btnCancel_clicked( bool checked )
{
    reject();
}

void QMainOpts::on_btnApply_clicked( bool checked )
{}
