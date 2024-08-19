#pragma once

#include <QtCore>

#include "cmnBase.hpp"
#include "mdlFileSystem.hpp"
#include "solTCPluginMgr.hpp"

////////////////////////////////////////////////////////////////////////////////
///

namespace nsHC
{
    Q_NAMESPACE

    class CFileSourceT;
    class CFileSystemT;

    enum TyEnFSFeature
    {
        FS_FEA_NONE                 = 0x0,
        FS_FEA_LIST                 = 0x1,
        FS_FEA_READABLE             = 0x2,
        FS_FEA_WRITABLE             = 0x4,
        FS_FEA_MOVABLE              = 0x8,
        FS_FEA_DELETABLE            = 0x10,
        FS_FEA_RENAMABLE            = 0x20,
        FS_FEA_PERMISSION           = 0x40,         // POSIX 
        FS_FEA_SECURITY             = 0x80,         // NTFS ACL
        FS_FEA_STREAMS              = 0x100,        // NTFS STREAMS
        FS_FEA_PACK                 = 0x200,        // PACKER PLUGIN
    };

    Q_ENUM_NS( TyEnFSFeature );
    Q_DECLARE_FLAGS( TyEnFSFeatures, TyEnFSFeature );
    Q_FLAG_NS( TyEnFSFeatures );

    enum TyEnFSCate
    {
        FS_CATE_LOCAL,
        FS_CATE_REMOTE,
        FS_CATE_VIRTUAL,
        FS_CATE_VFS,
    };

    Q_ENUM_NS( TyEnFSCate );
    Q_DECLARE_FLAGS( TyEnFSCates, TyEnFSCate );
    Q_FLAG_NS( TyEnFSCates );

    /*!
     * 윈도에서의 이름 정의
     *
     * 1. 파일 또는 디렉토리에서 이름과 확장자를 구분할 때는 '.' 를 사용한다.
     * 2. 경로상에서 각 구성요소간을 구분할 때는 '\' 를 사용한다.
     * 3. 볼륨 이름을 지칭할 때는 '\' 문자로 끝나야 한다.
     *      예) C:\, \\Server\Share 등
     * 4. MAX_PATH 를 넘어서는 경로를 지칭할 때는 \\?\ 를 접두사로 덧붙인다.
     * 5. 이름에 <, >, :, ", /, \, ?, * 를 사용할 수 없다. 또한, ASCII Code 0 ~ 31 까지의 코드도 사용할 수 없다.
     * 6. 디렉토리 구성요소에서 현재 디렉토리는 '.' 으로 표현한다.
     * 7. 디렉토리 구성요소에서 현재 디렉토리의 부모 디렉토리는 '..' 으로 표현한다.
     * 8. 다음과 같은 예약된 이름 사용금지. CON, PRN, AUX, NUL, COM1, COM2, COM3, COM4, COM5, COM6, COM7, COM8, COM9, LPT1, LPT2, LPT3, LPT4, LPT5, LPT6, LPT7, LPT8, LPT9
     * 9. 파일 또는 디렉토리의 이름을 '.' 또는 ' ' 으로 끝맺지 말 것.
     * 10. 상대경로에는 \\?\ 를 사용할 수 없다. 상대경로는 MAX_PATH 로 길이가 제한된다. 
     */

    class CFileSourceT
    {
    public:
        enum TyEnFsFlag
        {
            FS_FLAG_NONE            = 0x0,
            FS_FLAG_DRIVE           = 0x1,                      // Name 에는 각 드라이브 문자 
            FS_FLAG_UNICODE_NFD     = 0x2,
            FS_FLAG_VIRTUAL         = 0x4,
            FS_FLAG_PIDL            = 0x8,
        };

        QString                 Name_;
        QString                 Ext_;
        quint32                 Flags_ = FS_FLAG_NONE;          // TyEnFsFlag 의 Bitwise OR

        DWORD                   Attributes_ = 0;
        DWORD                   Reserved0_ = 0;
        qint64                  Size_ = 0;
        // TyUpItemIDList          UpItemIDList;                   // FS_FLAG_VIRTUAL, FS_FLAG_PIDL

        QDateTime               Created_;
        QDateTime               Modified_;

        ////////////////////////////////////////////////////////////////////////
        ///

        QPixmap                 Icon;
        QVector< QVariant >     VecContent;
        LPITEMIDLIST            PIDL = nullptr;
    };

    Q_DECLARE_METATYPE( CFileSourceT );

    using TySpFileSource = std::shared_ptr< CFileSourceT >;

    class CFileSystemT : public QObject
    {
        Q_OBJECT
    public:
        explicit CFileSystemT( const QString& Root );

        static CFileSystemT*    MakeFileSystem( const QString& Root, TyOsError* Error = nullptr );

        // LOCAL => X:, REMOTE => \\MyServer 또는 \\MyServer\\Av
        QString                 GetRoot() const { return root_; };
        //virtual TyOsError       SetRoot( const QString& Root );

        TyEnFSCate              GetCate() const;
        TyEnFSFeatures          GetFeatures() const;
        virtual bool            IsRemovable() { return false; }

        virtual QString         GetVolumeName() { return ""; }
        virtual TyOsError       SetVolumeName( const QString& VolumeLabel );

        virtual quint64         GetTotalSize() { return 0; }
        virtual quint64         GetFreeSize() { return 0; }

    protected:
        virtual void            readFsInformation();

    protected:
        QString                 root_;
        TyEnFSCate              cate_ = FS_CATE_LOCAL;
        TyEnFSFeatures          features_ = FS_FEA_NONE;
    };

    Q_DECLARE_METATYPE( CFileSystemT );

    using TySpFileSystem = QSharedPointer< CFileSystemT >;

    class CFSLocal : public CFileSystemT
    {
    public:
        explicit CFSLocal( const QString& Root );

        bool                    IsRemovable() override;
        
        quint64                 GetTotalSize() override;
        quint64                 GetFreeSize() override;

    protected:
        void                    readFsInformation() override;

    protected:

        DWORD                   maxComponentLength_ = 0;
    };

    class CFSNtfs : public CFSLocal
    {
    public:
        explicit CFSNtfs( const QString& Root );

        QString                 GetVolumeName() override;
    };

    class CFSShell : public CFileSystemT
    {
    public:
        explicit CFSShell( const LPITEMIDLIST Root );
        explicit CFSShell( const KNOWNFOLDERID& FolderId );
        ~CFSShell() override;

        LPITEMIDLIST            GetRoot() const { return Root; }
        QVector< nsHC::TySpFileSource > GetChildItems( LPITEMIDLIST Child, bool IncludeDotDot = false );

    private:

        LPITEMIDLIST            Root = nullptr;
        KNOWNFOLDERID           RootId;
        LPSHELLFOLDER           Desktop = nullptr;
    };

    class CFSSmb : public CFileSystemT
    {
    public:
        explicit CFSSmb( const QString& Root );

        QVector< nsHC::TySpFileSource > GetChildItems( const QString& Server, bool IncludeDotDot = false );

    private:
        CFSShell                shl_net;
    };

    class CFSPack : public CFileSystemT
    {
    public:
        explicit CFSPack( const QString& Root, const TySpWCX& WCX );
        ~CFSPack() override;

        TySpWCX                 GetWCX() const;
        QString                 GetParent() const;
        void                    SetParent( const QString& Parent );

        CPackFSModel*           Model();
    private:

        TySpWCX                 WCX_;
        QString                 Parent_;
        CPackFSModel*           FsModel_ = nullptr;
    };

    class CFSVFS : public CFileSystemT
    {
    public:

    };

    
} // nsHC
