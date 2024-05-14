#pragma once

#include "ui_dlgMain.h"

class QMainUI : public QMainWindow
{
    Q_OBJECT
public:
    QMainUI( QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags() );

protected slots:

    void                                on_acShowMainOpts_triggered( bool checked = false );

    void                                oo_notifyCurrentDirectory( const QString& CurrentPath );

private:

    Ui::dlgMain                         ui;
}; 