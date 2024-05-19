#pragma once

#include "cmnTypeDefs_Opts.hpp"

#include "ui_dlgOpts.h"

const QPair< int, QString > PAGE_DISPLAY                    = { 0, QObject::tr("화면") };
const QPair< int, QString > PAGE_DISPLAY_FONTCOLOR          = { 1, QObject::tr("  글꼴 및 색상") };
const QPair< int, QString > PAGE_SHORTCUT                   = { 2, QObject::tr("단축키") };

class QMainOpts : public QDialog
{
    Q_OBJECT
public:
    QMainOpts( QWidget* parent = Q_NULLPTR, Qt::WindowFlags flags = Qt::FramelessWindowHint );

    Q_INVOKABLE void                    LoadSettings();
    Q_INVOKABLE void                    SaveSettings();

protected slots:

    ///////////////////////////////////////////////////////////////////////////////
    /// TitleBar

    void                                on_btnClose_clicked( bool checked = false );

    ///////////////////////////////////////////////////////////////////////////////
    /// Left

    void                                on_lstOptCate_currentItemChanged( QListWidgetItem* Current, QListWidgetItem* Previous );


    ///////////////////////////////////////////////////////////////////////////////
    /// Content

    void                                on_btnFileListForground_clicked( bool checked = false );
    void                                on_btnFileListBackground_clicked( bool checked = false );

    ///////////////////////////////////////////////////////////////////////////////
    /// Footer

    void                                on_btnOK_clicked( bool checked = false );
    void                                on_btnCancel_clicked( bool checked = false );
    void                                on_btnApply_clicked( bool checked = false );

private:
    void                                initialize();

    void                                setButtonColor( QPushButton* Button, const QColor& Color );

    Ui::dlgOpts                         ui;
};