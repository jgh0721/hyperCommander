#pragma once

#include "cmnTypeDefs.hpp"

/*!
    파일 복사 / 이동 / 삭제를 수행하는 클래스
        주로 QFileProgress 와 같이 진행상태를 나타내는 클래스와 상호작용을 통해 일시중지/재시작 등을 수행한다.

    동작 상태에 따라 NotifyChangedState 시그널을 통지한다.


    예) C:\AMD(\GPIO\Radeon.sys) => C:\Apps
        결과 : C:\Apps\AMD\GPIO\Radeon.sys


    C:\AMD, 
        Parent = C:\
    C:\AMD\GPIO\Radeon.sys
        Parent = C:\, Path = AMD\GPIO\Radeon.sys
        Dst = D:\Apps, Result = D:\Apps\AMD\GPIO\Radeon.sys
 */

struct TyUserInteract
{
    QMutex          Lock;
    QWaitCondition  Wait;
    QVariant        Result;
};

class CFileOperation : public QThread
{
    Q_OBJECT
public:

    enum TyEnOperation { FILE_OP_COPY, FILE_OP_MOVE, FILE_OP_DELETE };
    enum TyEnOperationState { FILE_STATE_NOT_STARTED, FILE_STATE_RUNNING, FILE_STATE_PAUSED, FILE_STATE_STOPPING };

    Q_ENUM( TyEnOperation );
    Q_ENUM( TyEnOperationState );

    enum TyEnUserInteract
    {
        USER_SELECT_CANCEL          = 0x1,
        USER_SELECT_RENAME          = 0x2,
        USER_SELECT_RETRY           = 0x2,
        USER_SELECT_ADMIN_SINGLE    = 0x4,
        USER_SELECT_ADMIN_ALL       = 0x8,
        USER_SELECT_SKIP_SINGLE     = 0x10,
        USER_SELECT_SKIP_ALL        = 0x20,
    };

    Q_DECLARE_FLAGS( TyEnUserInteracts, TyEnUserInteract );
    Q_FLAG( TyEnUserInteracts );

    void                                SetMode( TyEnOperation Mode );
    void                                SetBase( const QString& Parent );
    void                                SetSource( const QVector< Node >& Src, const QVector< QString >& SrcMiddle );
    void                                SetDestination( const QString& Dst );

signals:
    // int = TyEnOperationState
    void                                NotifyChangedState( int State );
    void                                NotifyChangedItem( const QString& Src, const QString& Dst );
    void                                NotifyChangedProgress( qint16 Item, qint64 Total );
    void                                NotifyChangedStatus( qint64 CurrentItemCount, qint64 CurrentTotalSize );
    void                                NotifyErrorOccured( quint32 ErrorCode, TyEnUserInteracts Flags, TyUserInteract* User );

public slots:
    void                                ChangeState( int State );

protected:
    void                                run() override;
    void                                workCopy();
    void                                workMove();
    void                                workDelete();

private:
    bool                                isErrorOccuredConnected() const;

    QString                             retrieveDstItem( const QString& Dst, const QString& Middle, const QString& Name );
    // true = 계속 작업 수행, false = 외부에 의해 작업 중지해야 함( 인터럽트 요청 )
    // PAUSED, STOPPING 상태에서는 대기한다. 
    bool                                waitRunningState();

    DWORD CALLBACK CopyProgressRoutine(
  __in LARGE_INTEGER TotalFileSize,
  __in LARGE_INTEGER TotalBytesTransferred,
  __in LARGE_INTEGER StreamSize,
  __in LARGE_INTEGER StreamBytesTransferred,
  __in DWORD dwStreamNumber,
  __in DWORD dwCallbackReason,
  __in HANDLE hSourceFile,
  __in HANDLE hDestinationFile,
  __in_opt LPVOID lpData
    );


    TyEnOperation                       mode_ = FILE_OP_COPY;
    TyEnOperationState                  state_ = FILE_STATE_NOT_STARTED;
    
    QString                             base_;
    QVector< Node >                     vecSrc_;
    QVector< QString >                  vecSrcMiddle_;            // base 부분을 제외한 경로가 들어간다. 
    qint64                              totalSize_ = 0;

    QString                             dst_;
};