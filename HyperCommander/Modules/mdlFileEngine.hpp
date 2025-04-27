#pragma once

#include <QtCore>

#include "mdlFileSystem.hpp"

namespace nsHC
{
    enum TyEnFEMode
    {
        FE_MODE_NONE,
        FE_MODE_COPY, 
        FE_MODE_MOVE, 
        FE_MODE_DELETE,             // Src = 삭제 목록
        FE_MODE_RENAME,
        FE_MODE_ATTRIB,
        FE_MODE_MKDIR,              // Src = 부모 디렉토리, Dst = , Input = 생성할 디렉토리 이름( 구분자를 이용하여 다단계 생성 가능 )
    };

    enum TyEnFEState
    {
        FE_STATE_NOT_STARTED, 
        FE_STATE_STARTING, 
        FE_STATE_STARTED, 
        FE_STATE_PAUSING,
        FE_STATE_PAUSED,
        FE_STATE_WAIT_FOR_UI,
        FE_STATE_STOPPING, 
        FE_STATE_STOPPED,
    };

    enum TyEnFEUserInteraction
    {
        FE_USER_CANCEL, 
        FE_USER_RENAME, 
        FE_USER_RETRY, 
        FE_USER_ADMIN_SINGLE, 
        FE_USER_ADMIN_ALL,
        FE_USER_OVERWRITE_SINGLE, 
        FE_USER_OVERWRITE_ALL,
        FE_USER_SKIP_SINGLE, 
        FE_USER_SKIP_ALL,
    };

    enum TyEnFEOptions : uint64_t
    {
        FE_OPT_CMN_ADMIN_RIGHTS     = 0x1,

        FE_OPT_DEL_RECYCLE_BIN      = 0x1000,
        FE_OPT_DEL_READ_ONLY        = 0x2000,
        FE_OPT_DEL_HIDDEN_SYSTEM    = 0x4000, 
    };

    enum TyEnFEResult
    {
        FE_RESULT_SUCCEED, 
        FE_RESULT_FAILED,
        FE_RESULT_CANCELED,
    };

    struct CFETaskItemSource
    {
        nsHC::TySpFileSystem                Fs;
        QVector< nsHC::TySpFileSource >     Items;
    };

    using TySpFETaskItemSource = QSharedPointer< CFETaskItemSource >;

    /*!
     * 지정한 작업의 상태 전환 알림
     * TyEnFEState 의 값 순서대로 상태 전환이 이루어짐
     *
     * @return  0 == 계속 진행
     *          1 == 처리 중지( 사용자 취소 )
     */
    typedef int ( *TyFnFEChangeState )( PVOID Context, TyEnFEState Current, TyEnFEState Previous );
    /*!
     * 지정한 작업의 진행 상태 알림
     * 총 몇 개 중에 몇 번째 항목 진행 중
     * 총 크기 ~~ 바이트, 현재 항목의 크기 ~~ 바이트, 현재 진행 ~~ 바이트
     * @return  0 == 계속 진행
     */
    typedef int ( *TyFnFENotifyProgress )( PVOID Context, uint32_t CurrentIdx, uint32_t TotalCount, uint64_t CurrentSize, uint64_t Progress, uint64_t TotalSize );
    /*!
     * 오류 상황을 제외하고 사용자와 상호작용을 해야할 때 알림
     *
     * @return 
     */
    typedef int ( *TyFnFEUserInteraction )( PVOID Context, const QString& Caption, const QString& Content );
    /*!
     * 오류가 발생하였음을 알림
     *
     * @return 
     */
    typedef int ( *TyFnFEErrorOccured )( PVOID Context, const QString& Caption, const QString& Content, const TyOsError& ErrorValue );

    struct CFETaskItem
    {
        GUID                                Id;
        TyEnFEMode                          Type = FE_MODE_NONE;
        TySpFETaskItemSource                Src;
        TySpFETaskItemSource                Dst;
        QString                             Input;
        uint64_t                            Options;        // TyEnFEOptions Bitwise

        // 필요한 콜백 함수
        PVOID                               Context = nullptr;

        // 상태 전환 함수
        TyFnFEChangeState                   pfnChangeState = nullptr;

        // 진행도 알림 함수
        // 상호작용 함수
        TyFnFEUserInteraction               pfnUserInteraction = nullptr;
        TyFnFEErrorOccured                  pfnErrorOccured = nullptr;
    };

    using TySpFETaskItem = QSharedPointer< CFETaskItem >;

    class CFileEngine : public QThread
    {
        Q_OBJECT
    public:
        bool                                QueueTask( const TySpFETaskItem& Item );

        bool                                ExecuteTask( const TySpFETaskItem& Task, TyOsValue<TyEnFEResult>& Result );

    private:
            
        void                                processDelete( const TySpFETaskItem& Task, TyOsValue<TyEnFEResult>& Result );
        void                                processMkDir( const TySpFETaskItem& Task, TyOsValue<TyEnFEResult>& Result );

        int                                 notifyChangeState( const TySpFETaskItem& Task, TyEnFEState Curr, TyEnFEState Prev );

        QVector< TySpFETaskItem >           Items;
    };

    // 파일 엔진 클래스는 CFileSource 를 입력으로 받음
} // nsHC

using TyStFileEngine = nsCmn::nsConcurrent::TSingleton< nsHC::CFileEngine >;

