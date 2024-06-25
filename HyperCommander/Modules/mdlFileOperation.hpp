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

    ----------------------------------------------------------------------------

    로컬 검퓨터 간의 파일 복사 : CopyFileEx
    로컬 컴퓨터 간의 파일 이동 : MoveFileWithProgress, 디렉토리는 다른 디스크로 이동 불가
    로컬 컴퓨터 간의 파일 삭제 : DeleteFileW
    
    ----------------------------------------------------------------------------

    파일 작업 등에 사용되는 시간 계산

    시간 계산은 500ms 단위로 계산

    항목 => 총 수량, 현재 색인
    크기 => 총 크기, 현재까지 진행된 크기, 현재 항목의 크기

    시작
        모든 변수 초기화
        진행 막대 0 으로 초기화, 최대값 1090
        500ms 단위로 타이머 시작

    진행
        항목 시작 통지

        항목 완료 통지

    완료
        상태 변경 통지( 완료 )


    시간 계산 : Time, Second, Bytes
        S = B / T
        예) 25MB, 2min => 25 * 1048576 / 120 => 2621,4400 / 120 => 21,8453 Bytes / S
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

    enum TyEnOperation { FILE_OP_COPY, FILE_OP_MOVE, FILE_OP_DELETE, FILE_OP_RENAME };
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
    TyOsError                           ResultCode() const;

    ////////////////////////////////////////////////////////////////////////////
    /// 복사 옵션

    ////////////////////////////////////////////////////////////////////////////
    /// 이동 옵션

    ////////////////////////////////////////////////////////////////////////////
    /// 삭제 옵션

    void                                SetDeleteIsDelHiddenSys( bool IsDelHiddenSys );
    void                                SetDeleteIsDelReadOnly( bool IsDelReadOnly );
    void                                SetDeleteIsUseRecycleBin( bool IsUseRecycleBin );
    void                                SetDeleteIsUseAutoAdminRights( bool IsUseAutoAdminRights );

signals:
    /*! 변경된 작동 상태를 통지
     * State => TyEnOperationState
     */
    void                                NotifyChangedState( int State );
    /*! 작업 대상 항목을 통지
     *  삭제 작업의 경우에는 SRC 만 사용
     */
    void NotifyChangedItem( const QString& Src, const QString& Dst );
    /*!
     * 각 항목의 진행도를 통지
     * @param Item 현재 항목의 진행도( 0 ~ 100 )
     * @param Total 전체 항목의 진행도( 0 ~ 100 )
     */
    void NotifyChangedProgress( qint16 Item, qint16 Total );
    /*!
     * 
     * @param CurrentItemCount 전체 항목 수량 중 현재 항목 색인
     * @param CurrentTotalSize 전체 크기 중 현재까지 진행된 크기
     */
    void NotifyChangedStatus( qint64 CurrentItemCount, qint64 CurrentTotalSize );
    void NotifyErrorOccured( quint32 ErrorCode, TyEnUserInteracts Flags, TyUserInteract* User );
     // void NotifyConfirm( const QString& Title, const QString& Content, const QMessageBox::StandardButtons Buttons, QMessageBox::StandardButton* Ret );
    
     
    //// 현재 항목 변경 알림
    //void NotifyItemChanged( const QString& Src, const QString& Dst, qint64 SrcSize, qint64 SrcIndex );
    //void NotifyItemCompleted();
    //void NotifyChangedProgress( qint16 Current, qint16 Total );
    //void NotifyChangedTransfer( qint64 ItemProgressSize, qint64 ItemCompleteSize, qint64 TotalProgressSize, qint64 TotalCompleteSize );

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

    ////////////////////////////////////////////////////////////////////////////
    /// 공통 옵션

    TyEnOperation                       mode_ = FILE_OP_COPY;
    TyEnOperationState                  state_ = FILE_STATE_NOT_STARTED;
    TyOsError                           resultCode_;

    QString                             base_;
    QVector< Node >                     vecSrc_;
    QVector< QString >                  vecSrcMiddle_;            // base 부분을 제외한 경로가 들어간다. 
    qint64                              totalSize_ = 0;

    QString                             dst_;

    ////////////////////////////////////////////////////////////////////////////
    /// 복사 옵션

    ////////////////////////////////////////////////////////////////////////////
    /// 이동 옵션

    ////////////////////////////////////////////////////////////////////////////
    /// 삭제 옵션


    bool                                isDelHiddenSys_ = false;
    bool                                isDelReadonly_ = false;
    bool                                isUseRecycleBin = false;
    bool                                isUseAutoAdminRights = false;
};