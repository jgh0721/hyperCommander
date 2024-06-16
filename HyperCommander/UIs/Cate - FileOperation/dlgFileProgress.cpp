#include "stdafx.h"
#include "dlgFileProgress.hpp"

#include "Modules/mdlFileOperation.hpp"

QFileProgress::QFileProgress( QWidget* Parent, Qt::WindowFlags Flags )
    : QDialog( Parent, Flags )
{
    ui.setupUi( this );

    ui.lblSource->setText( "" );
    ui.lblDestination->setText( "" );

    ui.lblItemPercent->setText( "" );
    ui.lblItemSpeed->setText( "" );
    ui.lblTotalPercent->setText( "" );
    ui.lblTotalSpeed->setText( "" );

    ui.progItem->setMinimum( 0 );
    ui.progItem->setMaximum( 100 );
    ui.progItem->setValue( 0 );

    ui.progTotal->setMinimum( 0 );
    ui.progTotal->setMaximum( 100 );
    ui.progTotal->setValue( 0 );

    ui.lblFileCount->setText( "0 / 0" );
    ui.lblSizeCount->setText( "0 / 0" );

    ui.btnOKorCancel->setText( tr( "취소" ) );
}

void QFileProgress::SetMode( bool IsSingle )
{
    isSignle_ = IsSingle;

    if( isSignle_ == true )
    {
        ui.frmDestination->setHidden( true );
    }
}

void QFileProgress::SetOperationName( const QString& OpName )
{
    ui.lblTitleBar->setText( OpName );
}

void QFileProgress::SetInitialItemCount( qint64 Current, qint64 Total )
{
    itemTotalCount = Total;

    ui.lblFileCount->setText( QString( "%1 / %2" ).arg( Current ).arg( itemTotalCount ) );
}

void QFileProgress::SetInitialItemSize( qint64 Current, qint64 Total )
{
    itemTotalSize = Total;

    ui.lblSizeCount->setText( QString( "%1 / %2" ).arg( Current ).arg( itemTotalSize ) );
}

void QFileProgress::OnChangedState( int State )
{
    currentState = State;
    if( currentState == 0 /* FILE_STATE_NOT_STARTED */ )
        ui.btnOKorCancel->setText( tr( "확인(&O)" ) );
    else
        ui.btnOKorCancel->setText( tr( "취소(&O)" ) );

    if( currentState == CFileOperation::FILE_STATE_PAUSED )
    {
        // TODO: 일시 중지 되었음을 알리는 창 띄움. 
    }
}

void QFileProgress::OnChangedItem( const QString& Src, const QString& Dst )
{
    ui.lblSource->setText( Src );
    ui.lblDestination->setText( Dst );
}

void QFileProgress::OnChangedProgress( qint16 Item, qint64 Total )
{
    ui.progItem->setValue( Item );
    ui.progTotal->setValue( Total );
}

void QFileProgress::OnChangedStatus( qint64 CurrentItemCount, qint64 CurrentTotalSize )
{
    ui.lblFileCount->setText( QString( "%1 / %2" ).arg( CurrentItemCount ).arg( itemTotalCount ) );
    ui.lblSizeCount->setText( QString( "%1 / %2" ).arg( CurrentTotalSize ).arg( itemTotalSize ) );
}

void QFileProgress::on_btnClose_clicked( bool checked )
{
    on_btnOKorCancel_clicked( checked );
}

void QFileProgress::on_btnPauseOrResume_clicked( bool checked )
{
    emit RequestChangeState( CFileOperation::TyEnOperationState::FILE_STATE_PAUSED );

}

void QFileProgress::on_btnOKorCancel_clicked( bool checked )
{
    if( currentState == CFileOperation::TyEnOperationState::FILE_STATE_NOT_STARTED )
    {
        done( Accepted );
    }

    // TODO: 아직 진행 중임을 알림
}

void QFileProgress::on_btnBackgroundTo_clicked( bool checked )
{
}
