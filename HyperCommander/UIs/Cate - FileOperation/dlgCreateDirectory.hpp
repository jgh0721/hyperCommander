#pragma once

#pragma once

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include "ui_dlgCreateDirectory.h"

class QDirectoryCreateUI : public QDialog
{
    Q_OBJECT
public:
    QDirectoryCreateUI( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint );

    QString                             GetInputText() const;

protected slots:
    void                                on_btnOK_clicked( bool checked = false );

private:
    Ui::dlgNewFolder                     ui;
};