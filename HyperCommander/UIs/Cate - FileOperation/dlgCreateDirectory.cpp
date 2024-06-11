#include "stdafx.h"
#include "dlgCreateDirectory.hpp"

QDirectoryCreateUI::QDirectoryCreateUI( QWidget* Parent, Qt::WindowFlags Flags )
    : QDialog( Parent, Flags )
{
    ui.setupUi( this );

    ui.edtDirectoryName->setFocus( Qt::MouseFocusReason );
}

QString QDirectoryCreateUI::GetInputText() const
{
    return ui.edtDirectoryName->text();
}

void QDirectoryCreateUI::on_btnOK_clicked( bool checked )
{
    done( Accepted );
}
