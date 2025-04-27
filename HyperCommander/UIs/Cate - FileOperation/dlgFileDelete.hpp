#pragma once

#include "ui_dlgFileDelete.h"

class QTraverseSrcModel;

class QFileDeleteUI : public QDialog
{
    Q_OBJECT
public:
    QFileDeleteUI( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::FramelessWindowHint );
    ~QFileDeleteUI();

public slots:
    void                                SetSourcePath( const QString& Src );
    void                                SetSourceModel( const QVector< QModelIndex >& SrcModel );

    void                                on_btnClose_clicked( bool checked = false );

    void                                on_btnOK_clicked( bool checked = false );
    void                                on_btnCancel_clicked( bool checked = false );

    void                                slt_notifyStats( const QString& Item, bool IsDirectory, qint64 totalSize, qint64 fileCount, qint64 dirCount );

private:
    void                                work();
    Q_INVOKABLE void                    updateItems();

    QVector< QModelIndex >              src_;
    QString                             srcPath_;

    QThread                             workerThread_;

    //QTimer                              update_;
    //QAtomicInt                          isUpdate_;
    //QStringList                         model_;
    //QTraverseSrcModel*                  workerThread_ = nullptr;

    Ui::dlgDelete                       ui;
};
