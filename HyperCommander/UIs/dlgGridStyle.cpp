#include "stdafx.h"
#include "dlgGridStyle.hpp"

QDlgGridStyle::QDlgGridStyle( QWidget* parent, Qt::WindowFlags f )
    : QDialog( parent, f )
{
    ui.setupUi( this );
}

QString QDlgGridStyle::styleText() const
{
    return ui.textEdit->toPlainText();
}

void QDlgGridStyle::setStyleText( const QString& text )
{
    ui.textEdit->setText( text );
}
