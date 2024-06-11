#pragma once

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include "ui_dlgFileCopy.h"

class QFileCopyUI : public QDialog
{
    Q_OBJECT
public:
    QFileCopyUI( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint );

private:
    Ui::dlgFileCopy                     ui;
};