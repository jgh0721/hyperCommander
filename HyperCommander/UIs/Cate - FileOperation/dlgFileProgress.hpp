#pragma once

#include "ui_dlgFileProgress.h"

class QFileProgress : public QDialog
{
    Q_OBJECT
public:
    QFileProgress( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::FramelessWindowHint );

    // IsSingle == true 대상과 관련된 UI 숨김
    void                                SetMode( bool IsSingle );
    void                                SetOperationName( const QString& OpName );
    
public slots:
    void                                OnChangedItem( const QString& Src, const QString& Dst );
    void                                OnChangedProgress( qint16 Item, qint64 Total );
    void                                OnChangedStatus( qint64 CurrentItemCount, qint64 CurrentTotalSize );

private:

    Ui::dlgFileProgress                 ui;
};