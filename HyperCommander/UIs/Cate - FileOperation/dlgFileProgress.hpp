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
    void                                SetInitialItemCount( qint64 Current, qint64 Total );
    void                                SetInitialItemSize( qint64 Current, qint64 Total );

signals:
    // 해당하는 상태로 변경요청
    void                                RequestChangeState( int State );

public slots:
    void                                OnChangedState( int State );
    void                                OnChangedItem( const QString& Src, const QString& Dst );
    void                                OnChangedProgress( qint16 Item, qint16 Total );
    void                                OnChangedStatus( qint64 CurrentItemCount, qint64 CurrentTotalSize );

protected slots:
    void                                on_btnClose_clicked( bool checked = false );
    void                                on_btnPauseOrResume_clicked( bool checked = false );
    void                                on_btnOKorCancel_clicked( bool checked = false );
    void                                on_btnBackgroundTo_clicked( bool checked = false );

private:
    bool                                isSignle_ = false;
    int                                 currentState = 0;

    qint64                              itemTotalCount = 0;
    qint64                              itemTotalSize = 0;

    Ui::dlgFileProgress                 ui;
};