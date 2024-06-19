#pragma once

#include "cmnUiUtils.hpp"

#include "ui_dlgFileAttrib.h"

class QTraverseSrcModel;

class QFileAttribUI : public nsHC::QBaseUI
{
    Q_OBJECT
public:
    QFileAttribUI( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::FramelessWindowHint );
    ~QFileAttribUI();

public slots:
    void                                SetSourcePath( const QString& Src );
    void                                SetSourceModel( const QVector< QModelIndex >& SrcModel );

    void                                on_btnClose_clicked( bool checked = false );

    void                                on_btnOK_clicked( bool checked = false );
    void                                on_btnCancel_clicked( bool checked = false );

private:
    void                                work();

    QVector< QModelIndex >              src_;
    QString                             srcPath_;

    QTraverseSrcModel*                  workerThread_ = nullptr;

    Ui::dlgFileAttrib                   ui;
};
