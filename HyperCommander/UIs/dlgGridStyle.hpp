#pragma once

#include "ui_dlgGridStyle.h"

class QDlgGridStyle : public QDialog
{
    Q_OBJECT
public:
    QDlgGridStyle( QWidget* parent = nullptr, Qt::WindowFlags f = Qt::Dialog );

    QString                             styleText() const;
    void                                setStyleText( const QString& text );

private:
    Ui::Dialog                          ui;
};