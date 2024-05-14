#pragma once

#include "ui_dlgOpts.h"

class QMainOpts : public QDialog
{
    Q_OBJECT
public:
    QMainOpts( QWidget* parent = Q_NULLPTR, Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::Popup );

private:

    Ui::dlgOpts                         ui;
};