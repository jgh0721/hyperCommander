#pragma once

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include "ui_dlgFileCopy.h"

#include "UniqueLibs/builtInFsModel.hpp"

class QTraverseSrcModel : public QThread
{
    Q_OBJECT

public:
    QVector< QModelIndex >              src;

protected:
    void run() override
    {
        emit notifyStats( 0, 0, 0 );

        FSModel* Model = nullptr;
        if( src.isEmpty() == false )
            Model = (FSModel*)( src[ 0 ].model());

        qint64 totalSize = 0, fileCount = 0, dirCount = 0;

        for( int idx = 0; idx < src.size(); ++idx )
        {
            const auto& Node = Model->GetFileInfo( src[ idx ] );
            if( !FlagOn( Node.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
            {
                totalSize += Node.Size;
                fileCount += 1;
            }
            else
            {
                dirCount += 1;
            }

            emit notifyStats( totalSize, fileCount, dirCount );
        }

    }

signals:
    void notifyStats( qint64 totalSize, qint64 fileCount, qint64 dirCount );

};

class QFileCopyUI : public QDialog
{
    Q_OBJECT
public:
    QFileCopyUI( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint );

public slots:

    void                                SetSourcePath( const QString& Src );
    void                                SetSourceModel( const QVector< QModelIndex >& SrcModel );
    void                                SetDestinationPath( const QString& Dst );

    void                                on_btnClose_clicked( bool checked = false );

    void                                on_edtIncludeFilter_textChanged( const QString& text );

    void                                on_btnOK_clicked( bool checked = false );
    void                                on_btnCancel_clicked( bool checked = false );

    void                                slt_notifyStats( qint64 totalSize, qint64 fileCount, qint64 dirCount );

private:

    QVector< QModelIndex >              src_;
    QString                             srcPath_;
    QString                             dst_;
    QThread                             worker_;
    
    Ui::dlgFileCopy                     ui;
};