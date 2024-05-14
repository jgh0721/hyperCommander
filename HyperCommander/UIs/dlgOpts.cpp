#include "stdafx.h"
#include "dlgOpts.hpp"

QMainOpts::QMainOpts( QWidget* parent, Qt::WindowFlags flags )
    : QDialog( parent, flags )
{
    ui.setupUi( this );

    ui.lstOptCate->addItem( tr( "화면" ) );
    ui.lstOptCate->addItem( tr( "\t색상" ) );

}
