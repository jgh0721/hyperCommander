#include "stdafx.h"
#include "dlgFileCopy.hpp"

QFileCopyUI::QFileCopyUI( QWidget* Parent, Qt::WindowFlags Flags )
    : QDialog( Parent, Flags )
{
    ui.setupUi( this );
}

void QFileCopyUI::SetSourcePath( const QString& Src )
{
    srcPath_ = Src;
}

void QFileCopyUI::SetSourceModel( const QVector<QModelIndex>& SrcModel )
{
    src_ = SrcModel;
    ui.lblItemCount->setText( QString( "%1개" ).arg( SrcModel.count() ) );

    auto workerThread = new QTraverseSrcModel;
    workerThread->src = src_;

    connect( workerThread, &QTraverseSrcModel::notifyStats, this, &QFileCopyUI::slt_notifyStats );
    connect( workerThread, &QTraverseSrcModel::finished, workerThread, &QObject::deleteLater );
    workerThread->start();
}

void QFileCopyUI::SetDestinationPath( const QString& Dst )
{
    ui.edtDirectory->setDirectoryPath( Dst );
}

void QFileCopyUI::on_btnClose_clicked( bool checked )
{
    done( Rejected );
}

void QFileCopyUI::on_edtIncludeFilter_textChanged( const QString& text )
{

}

void QFileCopyUI::on_btnOK_clicked( bool checked )
{
    done( Accepted );
}

void QFileCopyUI::on_btnCancel_clicked( bool checked )
{
    done( Rejected );
}

void QFileCopyUI::slt_notifyStats( qint64 totalSize, qint64 fileCount, qint64 dirCount )
{
    ui.lblFileCount->setText( QString( "%1" ).arg( fileCount ) );
    ui.lblFolderCount->setText( QString( "%1" ).arg( dirCount ) );
    ui.lblIncludeSize->setText( QString( "%1" ).arg( totalSize ) );
}
