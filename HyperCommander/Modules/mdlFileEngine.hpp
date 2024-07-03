#pragma once

#include <QtCore>

namespace nsHC
{
    enum TyEnFEMode
    {
        FE_MODE_COPY, 
        FE_MODE_MOVE, 
        FE_MODE_DELETE, 
        FE_MODE_RENAME,
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

    enum TyEnFEResult
    {
        FE_RESULT_SUCCEED, 
        FS_RESULT_FAILED,
        FE_RESULT_CANCELED,
    };

    class CFileEngine : public QThread
    {
        Q_OBJECT
    public:

    public:

        void SetMode();
        void SetSrcModel();
        void SetDstModel();
        void SetDefaultAction();

    signals:
        void NotifyStateChanged( TyEnFEState State );
        void NotifyItemChanged();
        void NotifyProgressChanged();
        void NotifyTransferChanged();
    };

    // 파일 엔진 클래스는 CFileSource 를 입력으로 받음
} // nsHC

