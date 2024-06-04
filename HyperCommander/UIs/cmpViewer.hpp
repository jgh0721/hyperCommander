#pragma once

#include "ui_cmpViewer.h"

class QDlgViewer;

class CmpViewer : public QWidget
{
    Q_OBJECT
public:
    CmpViewer( QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

private:
    friend class QDlgViewer;

    Ui::cmpViewer                        ui;
};