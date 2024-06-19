#include "stdafx.h"
#include "dlgFileCopy.hpp"
#include "dlgFileProgress.hpp"

#include "Modules/mdlFileOperation.hpp"

#include "cmnHCUtils.hpp"
#include "cmnHCModelUtils.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

////////////////////////////////////////////////////////////////////////////////
///

QFileCopyUI::QFileCopyUI( QWidget* Parent, Qt::WindowFlags Flags )
    : QDialog( Parent, Flags )
{
    ui.setupUi( this );

    SetTitleIcon( ui.lblTitleBarIcon );

    ui.lblIncludeSize->setText( "" );
    ui.lblFileCount->setText( "" );
    ui.lblFolderCount->setText( "" );

    OleInitialize( 0 );

    do
    {
        SHFILEINFOW SHInfo = { 0, };

        SHGetFileInfoW( L"AAA", FILE_ATTRIBUTE_DIRECTORY, &SHInfo, sizeof( SHInfo ),
                        SHGFI_ADDOVERLAYS | SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES );

        if( SHInfo.hIcon != nullptr )
        {
            dirIcon_ = QPixmap::fromImage( QImage::fromHICON( SHInfo.hIcon ) );
            DestroyIcon( SHInfo.hIcon );
            ui.lblFolderCount->setIcon( dirIcon_ );
        }

        SHGetFileInfoW( L"AAA", FILE_ATTRIBUTE_NORMAL, &SHInfo, sizeof( SHInfo ),
                        SHGFI_ADDOVERLAYS | SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES );

        if( SHInfo.hIcon != nullptr )
        {
            fileIcon_ = QPixmap::fromImage( QImage::fromHICON( SHInfo.hIcon ) );
            DestroyIcon( SHInfo.hIcon );
            ui.lblFileCount->setIcon( fileIcon_ );
        }

    } while( false );

    CoUninitialize();
}

QFileCopyUI::~QFileCopyUI()
{
    if( workerThread_ != nullptr )
    {
        workerThread_->requestInterruption();
        workerThread_->wait();
        workerThread_->deleteLater();
    }
}

void QFileCopyUI::SetSourcePath( const QString& Src )
{
    srcPath_ = Src;
}

void QFileCopyUI::SetSourceModel( const QVector<QModelIndex>& SrcModel )
{
    src_ = SrcModel;
    ui.lblItemCount->setText( QString( "%1개" ).arg( SrcModel.count() ) );

    work();
}

void QFileCopyUI::SetDestinationPath( const QString& Dst )
{
    dst_ = Dst;
    ui.edtDirectory->setDirectoryPath( Dst );
}

void QFileCopyUI::on_btnClose_clicked( bool checked )
{
    done( Rejected );
}

void QFileCopyUI::on_edtIncludeFilter_textChanged( const QString& text )
{
    work();
}

void QFileCopyUI::on_btnOK_clicked( bool checked )
{
    if( workerThread_ != nullptr )
    {
        workerThread_->wait();

        auto UiProgress     = new QFileProgress;
        auto ClsOperation   = new CFileOperation;

        ClsOperation->SetMode( CFileOperation::FILE_OP_COPY );
        ClsOperation->SetBase( workerThread_->base );
        ClsOperation->SetSource( workerThread_->vecItems_, workerThread_->vecItemMiddle_ );
        ClsOperation->SetDestination( dst_ );

        UiProgress->SetMode( false );
        UiProgress->SetOperationName( tr( "파일 복사" ) );
        UiProgress->SetInitialItemCount( 0, workerThread_->vecItems_.size() );
        UiProgress->SetInitialItemSize( 0, workerThread_->totalSize_ );

        connect( ClsOperation, &CFileOperation::NotifyChangedState, UiProgress, &QFileProgress::OnChangedState );
        connect( ClsOperation, &CFileOperation::NotifyChangedItem, UiProgress, &QFileProgress::OnChangedItem );
        connect( ClsOperation, &CFileOperation::NotifyChangedProgress, UiProgress, &QFileProgress::OnChangedProgress );
        connect( ClsOperation, &CFileOperation::NotifyChangedStatus, UiProgress, &QFileProgress::OnChangedStatus );
        connect( UiProgress, &QFileProgress::RequestChangeState, ClsOperation, &CFileOperation::ChangeState );

        ClsOperation->start();
        UiProgress->exec();
    }

    done( Accepted );
}

void QFileCopyUI::on_btnCancel_clicked( bool checked )
{
    done( Rejected );
}

void QFileCopyUI::slt_notifyStats( const QString& Item, bool IsDirectory, qint64 totalSize, qint64 fileCount, qint64 dirCount )
{
    ui.lblFileCount->setText( QString( "%1" ).arg( fileCount ) );
    ui.lblFolderCount->setText( QString( "%1" ).arg( dirCount ) );
    ui.lblIncludeSize->setText( QString( "%1" ).arg( totalSize ) );
}

void QFileCopyUI::work()
{
    if( workerThread_ != nullptr )
    {
        workerThread_->requestInterruption();
        workerThread_->wait();
        delete workerThread_;
        workerThread_ = nullptr;
    }

    workerThread_ = new QTraverseSrcModel;
    workerThread_->src = src_;
    workerThread_->filter = ui.edtIncludeFilter->text();

    connect( workerThread_, &QTraverseSrcModel::notifyStats, this, &QFileCopyUI::slt_notifyStats );
    workerThread_->start();
}
