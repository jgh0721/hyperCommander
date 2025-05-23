﻿#pragma once

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include "ui_dlgFileCopy.h"

#include "UniqueLibs/builtInFsModel.hpp"

class QTraverseSrcModel;

class QFileCopyUI : public QDialog
{
    Q_OBJECT
public:
    QFileCopyUI( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::FramelessWindowHint );
    ~QFileCopyUI();

public slots:

    void                                SetSourcePath( const QString& Src );
    void                                SetSourceModel( const QVector< QModelIndex >& SrcModel );
    void                                SetDestinationPath( const QString& Dst );

    void                                on_btnClose_clicked( bool checked = false );

    void                                on_edtIncludeFilter_textChanged( const QString& text );

    void                                on_btnOK_clicked( bool checked = false );
    void                                on_btnCancel_clicked( bool checked = false );

    void                                slt_notifyStats( const QString& Item, bool IsDirectory, qint64 totalSize, qint64 fileCount, qint64 dirCount );

private:
    void                                work();

    QVector< QModelIndex >              src_;
    QString                             srcPath_;
    QString                             dst_;
    QThread                             worker_;

    QPixmap                             dirIcon_;
    QPixmap                             fileIcon_;

    //QTraverseSrcModel*                  workerThread_ = nullptr;

    Ui::dlgFileCopy                     ui;
};