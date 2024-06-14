#include "stdafx.h"
#include "dlgFileProgress.hpp"

QFileProgress::QFileProgress( QWidget* Parent, Qt::WindowFlags Flags )
    : QDialog( Parent, Flags )
{
    ui.setupUi( this );

}

void QFileProgress::SetMode( bool IsSingle )
{
}

void QFileProgress::SetOperationName( const QString& OpName )
{
    ui.lblTitleBar->setText( OpName );
}

void QFileProgress::OnChangedItem( const QString& Src, const QString& Dst )
{
}

void QFileProgress::OnChangedProgress( qint16 Item, qint64 Total )
{
}

void QFileProgress::OnChangedStatus( qint64 CurrentItemCount, qint64 CurrentTotalSize )
{
}
