#include "stdafx.h"
#include "dlgFileDelete.hpp"

#include "Cate - FileOperation/dlgFileProgress.hpp"
#include "Modules/mdlFileOperation.hpp"

#include "UniqueLibs/cmnHCModelUtils.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

//////////////////////////////////////////////////////////////////////////////
///

QFileDeleteUI::QFileDeleteUI( QWidget* Parent, Qt::WindowFlags Flags )
    : QDialog( Parent, Flags )
{
    ui.setupUi( this );

    //update_.setSingleShot( true );
    //update_.setInterval( ONE_SECOND_MS );
    //connect( &update_, &QTimer::timeout, this, &QFileDeleteUI::updateItems );

    ui.lstTargetItems->setModel( new QStringListModel );
}

QFileDeleteUI::~QFileDeleteUI()
{
    workerThread_.quit();
    workerThread_.wait();
}

void QFileDeleteUI::SetSourcePath( const QString& Src )
{
    srcPath_ = Src;
}

void QFileDeleteUI::SetSourceModel( const QVector<QModelIndex>& SrcModel )
{
    src_ = SrcModel;
    ui.lblInformText->setText( QString( "%1개의 항목을 삭제합니다." ).arg( SrcModel.count() ) );

    work();
}

void QFileDeleteUI::on_btnClose_clicked( bool checked )
{
    done( Rejected );
}

void QFileDeleteUI::on_btnOK_clicked( bool checked )
{
    //if( workerThread_ != nullptr )
    //{
    //    workerThread_->wait();

    //    const auto UiProgress     = new QFileProgress;
    //    //const auto ClsOperation   = new CFileOperation;

    //    UiProgress->SetMode( true );
    //    UiProgress->SetOperationName( tr( "파일 삭제" ) );
    //    UiProgress->SetInitialItemCount( 0, workerThread_->vecItems_.size() );
    //    UiProgress->SetInitialItemSize( 0, workerThread_->totalSize_ );

    //    //ClsOperation->SetMode( CFileOperation::FILE_OP_DELETE );
    //    //ClsOperation->SetBase( workerThread_->base );
    //    //ClsOperation->SetSource( workerThread_->vecItems_, workerThread_->vecItemMiddle_ );

    //    //ClsOperation->SetDeleteIsDelHiddenSys( ui.chkIsDeleteHiddenSys->isChecked() );
    //    //ClsOperation->SetDeleteIsDelReadOnly( ui.chkIsDelReadOnly->isChecked() );
    //    //ClsOperation->SetDeleteIsUseRecycleBin( ui.chkIsUseAdminRights->isChecked() );
    //    //ClsOperation->SetDeleteIsUseAutoAdminRights( ui.chkIsUseRecycleBin->isChecked() );
    //    //
    //    //connect( ClsOperation, &CFileOperation::NotifyChangedState, UiProgress, &QFileProgress::OnChangedState );
    //    //connect( ClsOperation, &CFileOperation::NotifyChangedItem, UiProgress, &QFileProgress::OnChangedItem );
    //    //connect( ClsOperation, &CFileOperation::NotifyChangedProgress, UiProgress, &QFileProgress::OnChangedProgress );
    //    //connect( ClsOperation, &CFileOperation::NotifyChangedStatus, UiProgress, &QFileProgress::OnChangedStatus );
    //    //connect( UiProgress, &QFileProgress::RequestChangeState, ClsOperation, &CFileOperation::ChangeState );

    //    //ClsOperation->start();
    //    //UiProgress->exec();

    //    //if( ClsOperation->isRunning() == true )
    //    //    ClsOperation->wait();

    //    //UiProgress->deleteLater();
    //    //ClsOperation->deleteLater();

    //    //// 진행상태 Ui 가 닫혔음. 그렇다면, 이제 이곳에서 실제 동작이 성공적으로 끝났는지 확인해야 한다.
    //    //// 해당 값에 따라 Accepted 또는 Rejected 를 설정한다. 
    //    //const auto ErrorCode = ClsOperation->ResultCode();
    //    //if( IsOsError( ErrorCode ) )
    //    //    done( Rejected );
    //    //else
    //    //    done( Accepted );
    //}

    done( Accepted );
}

void QFileDeleteUI::on_btnCancel_clicked( bool checked )
{
    done( Rejected );
}

void QFileDeleteUI::slt_notifyStats( const QString& Item, bool IsDirectory, qint64 totalSize, qint64 fileCount, qint64 dirCount )
{
    if( Item.isEmpty() == true )
        return;

    //if( update_.isActive() == false )
    //    update_.start();

    //while( isUpdate_.loadRelaxed() == 1 )
    //    QApplication::processEvents( QEventLoop::AllEvents );

    //model_ << srcPath_.left( 2 ) + Item;
}

void QFileDeleteUI::work()
{
    auto traverseModel = new QTraverseModel;
    traverseModel->moveToThread( &workerThread_ );
    connect( &workerThread_, &QThread::finished, traverseModel, &QObject::deleteLater );
    workerThread_.start();

    QMetaObject::invokeMethod( traverseModel, &QTraverseModel::Run, src_ );
}

void QFileDeleteUI::updateItems()
{
    //isUpdate_.storeRelease( 1 );

    //const auto model = ui.lstTargetItems->model();
    //auto RowCount = model->rowCount();

    //if( model->insertRows( RowCount, model_.count() ) == true )
    //{
    //    for( const auto& Item : model_ )
    //        model->setData( model->index( RowCount++, 0 ), Item, Qt::DisplayRole );

    //    model_.clear();
    //}

    //isUpdate_.storeRelease( 0 );
}
