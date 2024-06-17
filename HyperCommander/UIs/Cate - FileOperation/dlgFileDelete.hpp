#pragma once

#include "ui_dlgFileDelete.h"

class QFileDeleteUI : public QDialog
{
    Q_OBJECT
public:
    QFileDeleteUI( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::FramelessWindowHint );

private:

    Ui::dlgDelete                       ui;
};