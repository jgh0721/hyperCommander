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

    enum TyEnFSCate
    {
        FS_CATE_LOCAL, 
        FS_CATE_REMOVABLE,
        FS_CATE_REMOTE, 
        FS_CATE_VFS, 
        FS_CATE_PACKER,
    };

    enum TyEnFSFeature
    {
        FS_FEA_LIST, 
        FS_FEA_READABLE, 
        FS_FEA_WRITABLE,
        FS_FEA_MOVABLE,
        FS_FEA_DELETABLE, 
        FS_FEA_RENAMABLE,
        FS_FEA_PERMISSION,      // POSIX 
        FS_FEA_SECURITY,        // NTFS ACL
    };
    
    class CFileSystem
    {
        // 파일시스템
        // UNC(SMB) 여부
        // Packer 여부
        // VFS 여부
        // 파일시스템 능력, 
        //      읽기 가능, 쓰기 가능, 이동 가능, 이름 변경 가능, 삭제 가능, 열거 가능, 

        qint64 GetFreeSpaceW();
        qint64 GetTotalSpace();
        quint64 GetTotalSize();

        QString GetRoot();              // UNC 라면 \\ServerName\Share
                                        // Packer 라면 기반이 되는 압축파일의 경로

        void SetRoot();

        int Features();
    private:
        void readFileSystemFeatures();

        TyEnFSCate Cate;
        int Features_;
    };

    // 패널에 CFileSystem 또는 그 자식 클래스를 가진다.  
    
    class CFileSource
    {
    public:
        qint64 GetCount();
        QString GetName( qint64 Idx );
        QString GetRoot( qint64 Idx );      // UNC 라면 \\ServerName\Path 
        QString GetPath( qint64 Idx );
        qint64 GetSize( qint64 Idx );

    private:

        // FileSystem 클래스
    };

    // 파일 엔진 클래스는 CFileSource 를 입력으로 받음
} // nsHC

