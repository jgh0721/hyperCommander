#include "stdafx.h"
#include "dlgFileAttrib.hpp"

#include "UniqueLibs/cmnHCModelUtils.hpp"

QFileAttribUI::QFileAttribUI( QWidget* Parent, Qt::WindowFlags Flags )
    : QBaseUI( Parent, Flags )
{
    ui.setupUi( this );
}

QFileAttribUI::~QFileAttribUI()
{
    if( workerThread_ != nullptr )
    {
        workerThread_->requestInterruption();
        workerThread_->wait();
        workerThread_->deleteLater();
    }
}

void QFileAttribUI::SetSourcePath( const QString& Src )
{
    srcPath_ = Src;
}

void QFileAttribUI::SetSourceModel( const QVector<QModelIndex>& SrcModel )
{
    src_ = SrcModel;
    // ui.lblInformText->setText( QString( "%1개의 파일을 삭제합니다." ).arg( SrcModel.count() ) );

    work();
}

void QFileAttribUI::on_btnClose_clicked( bool checked )
{
    done( Rejected );
}

void QFileAttribUI::on_btnOK_clicked( bool checked )
{
    if( workerThread_ != nullptr )
    {
        workerThread_->wait();

        //const auto UiProgress = new QFileProgress;
        //const auto ClsOperation = new CFileOperation;

        //UiProgress->SetMode( true );
        //UiProgress->SetOperationName( tr( "파일 삭제" ) );
        //UiProgress->SetInitialItemCount( 0, workerThread_->vecItems_.size() );
        //UiProgress->SetInitialItemSize( 0, workerThread_->totalSize_ );

        //ClsOperation->SetMode( CFileOperation::FILE_OP_DELETE );
        //ClsOperation->SetBase( workerThread_->base );
        //ClsOperation->SetSource( workerThread_->vecItems_, workerThread_->vecItemMiddle_ );

        //ClsOperation->SetDeleteIsDelHiddenSys( ui.chkIsDeleteHiddenSys->isChecked() );
        //ClsOperation->SetDeleteIsDelReadOnly( ui.chkIsDelReadOnly->isChecked() );
        //ClsOperation->SetDeleteIsUseRecycleBin( ui.chkIsUseAdminRights->isChecked() );
        //ClsOperation->SetDeleteIsUseAutoAdminRights( ui.chkIsUseRecycleBin->isChecked() );

        //connect( ClsOperation, &CFileOperation::NotifyChangedState, UiProgress, &QFileProgress::OnChangedState );
        //connect( ClsOperation, &CFileOperation::NotifyChangedItem, UiProgress, &QFileProgress::OnChangedItem );
        //connect( ClsOperation, &CFileOperation::NotifyChangedProgress, UiProgress, &QFileProgress::OnChangedProgress );
        //connect( ClsOperation, &CFileOperation::NotifyChangedStatus, UiProgress, &QFileProgress::OnChangedStatus );
        //connect( UiProgress, &QFileProgress::RequestChangeState, ClsOperation, &CFileOperation::ChangeState );

        //ClsOperation->start();
        //UiProgress->exec();

        //if( ClsOperation->isRunning() == true )
        //    ClsOperation->wait();

        //UiProgress->deleteLater();
        //ClsOperation->deleteLater();

        //// 진행상태 Ui 가 닫혔음. 그렇다면, 이제 이곳에서 실제 동작이 성공적으로 끝났는지 확인해야 한다.
        //// 해당 값에 따라 Accepted 또는 Rejected 를 설정한다. 
        //const auto ErrorCode = ClsOperation->ResultCode();
        //if( IsOsError( ErrorCode ) )
        //    done( Rejected );
        //else
        //    done( Accepted );
    }

    done( Accepted );
}

void QFileAttribUI::on_btnCancel_clicked( bool checked )
{
    done( Rejected );
}

void QFileAttribUI::work()
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
    workerThread_->filter = "";

    //connect( workerThread_, &QTraverseSrcModel::notifyStats, this, &QFileDeleteUI::slt_notifyStats );
    workerThread_->start();
}
