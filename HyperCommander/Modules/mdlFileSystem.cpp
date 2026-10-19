#include "stdafx.h"
#include "wcxStorage.hpp"
#include "wcxBackend.hpp"
#include "mdlFileSystem.hpp"

#include "cmnHCUtils.hpp"
#include "cmnTypeDefs_Name.hpp"
#include "storageProviders.hpp"
#include "HyperCommanderBuiltInPlugin/IsoImage/builtinIso.hpp"
#include "HyperCommanderBuiltInPlugin/IsoImage/isoProvider.hpp"
#include <QFileInfo>

#include <lm.h>

DECLARE_CMNLIBSV2_NAMESPACE

////////////////////////////////////////////////////////////////////////////////
///

namespace nsHC
{
    CFileSystemT::CFileSystemT( const QString& Root )
    {
        root_ = Root;
    }

    CFileSystemT* CFileSystemT::MakeFileSystem( const QString& Root, TyOsError* Error )
    {
        CFileSystemT* Ret = nullptr;
        if( Error != nullptr )
            *Error = TyOsError();

        // Root 경로명을 분석하여 LOCAL, REMOTE 등을 판정한다.

        QString s = Root;
        s = s.replace( "/", "\\" );

        bool IsNTPath = false;
        bool IsUNCPath = false;
        bool IsVFS = false;
        qsizetype Base = 0;

        if( s.startsWith( NTPATH_UNC, Qt::CaseInsensitive ) == true )
        {
            IsNTPath = true;
            IsUNCPath = true;
            Base = NTPATH_UNC.length();
        }
        else if( s.startsWith( NTPATH ) == true )
        {
            IsNTPath = true;
            IsUNCPath = false;
            Base = NTPATH.length();
        }
        else
        {
            if( s.startsWith( VFSPATH ) == true )
            {
                // VFS
                IsVFS = true;
            }
            else
            {
                // LOCAL, REMOTE

                if( s.startsWith( R"(\\)" ) == true )
                {
                    IsUNCPath = true;
                    Base = 2;
                }
            }
        }

        if( IsVFS == true )
        {

            return nullptr;
        }

        if( IsUNCPath == true )
        {
            return new nsHC::CFSSmb( "\\" );
        }

        if( IsUNCPath == false )
        {
            // LOCAL
            const auto Pos = s.indexOf( ':' );

            if( Pos < 0 )
            {
                if( Error != nullptr )
                    *Error = TyOsError( TyOsError::OS_WIN32_ERROR, ERROR_BAD_PATHNAME );

                return nullptr;
            }

            QString RootPath;
            if( IsNTPath == true )
            {
                if( IsUNCPath == true )
                    RootPath = NTPATH_UNC;
                else
                    RootPath = NTPATH;
            }
            else
            {
                if( IsUNCPath == true )
                    RootPath = R"(\\)";
            }

            RootPath += s.mid( Base, 3 );
            if( RootPath.endsWith( "\\" ) == false )
                RootPath += "\\";

            // 반드시 \\ 로 끝나야 한다. 

            // Volume metadata can reconnect an unavailable mapped drive and
            // block for the redirector's timeout. Both panels call this factory
            // on the GUI thread at startup. Classify first, without probing the
            // server; actual access belongs to the asynchronous read provider.
            // GetDriveType can report NO_ROOT_DIR for \\?\X:\ mappings even
            // when X:\ is DRIVE_REMOTE. Classify the ordinary drive root.
            const auto DriveRoot = s.mid(Base, 2);
            if (GetDriveTypeW((DriveRoot + '\\').toStdWString().c_str()) == DRIVE_REMOTE)
                return new CFSNetworkDrive(DriveRoot);

            WCHAR FsName[ MAX_PATH ] = { 0, };
            if( GetVolumeInformationW( RootPath.toStdWString().c_str(), nullptr, 0, nullptr, nullptr, nullptr, FsName, MAX_PATH ) == FALSE )
            {
                if( Error != nullptr )
                    *Error = TyOsError( TyOsError::OS_WIN32_ERROR, ::GetLastError() );

                return nullptr;
            }

            if( stricmp( FsName, L"NTFS" ) == 0 )
                Ret = new nsHC::CFSNtfs( RootPath.left(2) );
            else
                Ret = new nsHC::CFSLocal( RootPath.left(2) );
        }

        return Ret;
    }

    TyEnFSCate CFileSystemT::GetCate() const
    {
        return cate_;
    }

    TyEnFSFeatures CFileSystemT::GetFeatures() const
    {
        return features_;
    }

    TyOsError CFileSystemT::SetVolumeName( const QString& VolumeLabel )
    {
        return { TyOsError::OS_WIN32_ERROR, ERROR_SUCCESS };
    }

    void CFileSystemT::readFsInformation()
    {
    }

    ////////////////////////////////////////////////////////////////////////////////
    ///

    CFSLocal::CFSLocal( const QString& Root )
        : CFileSystemT( Root )
    {
        cate_ = FS_CATE_LOCAL;
        readFsInformation();
    }

    bool CFSLocal::IsRemovable()
    {
        return GetDriveTypeW( GetRoot().toStdWString().c_str() ) == DRIVE_REMOVABLE;
    }

    quint64 CFSLocal::GetTotalSize()
    {
        ULARGE_INTEGER Li = { 0 };
        if( GetDiskFreeSpaceExW( GetRoot().toStdWString().c_str(), nullptr, &Li, nullptr ) == FALSE )
            return 0;

        return Li.QuadPart;
    }

    quint64 CFSLocal::GetFreeSize()
    {
        ULARGE_INTEGER Li = { 0 };
        if( GetDiskFreeSpaceExW( GetRoot().toStdWString().c_str(), &Li, nullptr, nullptr ) == FALSE )
            return 0;

        return Li.QuadPart;
    }

    void CFSLocal::readFsInformation()
    {
        DWORD FsFlags = 0;
        if( GetVolumeInformationW( GetRoot().toStdWString().c_str(), nullptr, 0, nullptr, &maxComponentLength_, &FsFlags, nullptr, 0 ) == FALSE )
            return;

        features_ |= FS_FEA_LIST;
        features_ |= FS_FEA_READABLE;

        if( !FlagOn( FsFlags, FILE_READ_ONLY_VOLUME ) )
            features_ = static_cast< TyEnFSFeatures >( features_.toInt() | (
                                                           FS_FEA_WRITABLE | FS_FEA_MOVABLE | FS_FEA_DELETABLE | FS_FEA_RENAMABLE ) );

        if( FlagOn( FsFlags, FILE_PERSISTENT_ACLS ) )
            features_ |= FS_FEA_SECURITY;

        if( FlagOn( FsFlags, FILE_NAMED_STREAMS ) )
            features_ |= FS_FEA_STREAMS;
    }

    ////////////////////////////////////////////////////////////////////////////////
    ///

    CFSNtfs::CFSNtfs( const QString& Root )
        : CFSLocal( Root )
    {

    }

    QString CFSNtfs::GetVolumeName()
    {
        QVector< wchar_t > VecBuffer( MAX_PATH );

        if( GetVolumeInformationW( GetRoot().toStdWString().c_str(), VecBuffer.data(), VecBuffer.size(), nullptr, nullptr, nullptr, nullptr, 0 ) == FALSE )
            return "";

        return QString::fromWCharArray( VecBuffer.data() );
    }

    ////////////////////////////////////////////////////////////////////////////////
    ///

    CFSNetworkDrive::CFSNetworkDrive(const QString& Root)
        : CFileSystemT(Root)
    {
        // These are operation capabilities, not a claim about share permissions
        // or filesystem-specific ACL/stream support. Access is checked by I/O.
        features_ = TyEnFSFeatures(FS_FEA_LIST) | FS_FEA_READABLE | FS_FEA_WRITABLE
            | FS_FEA_MOVABLE | FS_FEA_DELETABLE | FS_FEA_RENAMABLE;
    }

    CFSShell::CFSShell( const LPITEMIDLIST Root )
        : CFileSystemT( "" )
    {
        cate_ = FS_CATE_VIRTUAL;
        features_ = FS_FEA_LIST;
        this->Root = ILCloneFull(Root);
    }

    CFSShell::CFSShell( const KNOWNFOLDERID& FolderId )
        : CFileSystemT( "" )
    {
        cate_ = FS_CATE_VIRTUAL;
        features_ = FS_FEA_LIST;
        RootId = FolderId;
        const auto Ret = SHGetKnownFolderIDList( RootId, KF_FLAG_CREATE, nullptr, &Root );
    }

    CFSShell::~CFSShell()
    {
        if( Root != nullptr )
            ILFree( Root );
        Root = nullptr;

        if( Desktop != nullptr )
            Desktop->Release();
        Desktop = nullptr;
    }

    QVector<nsHC::TySpFileSource> CFSShell::GetChildItems( LPITEMIDLIST Child, bool IncludeDotDot )
    {
        CoInitializeEx( 0, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE );
        QVector<nsHC::TySpFileSource> Items;
        HRESULT Ret = S_OK;

        LPSHELLFOLDER sfFolder = nullptr;
        LPENUMIDLIST enumIDL = nullptr;

        do
        {
            if( Desktop == nullptr )
            {
                Ret = SHGetDesktopFolder( &Desktop );
                if( FAILED( Ret ) )
                    break;
            }

            if( Root == nullptr )
                break;

            Ret = Desktop->BindToObject( Child == nullptr ? Root : Child, nullptr, IID_IShellFolder, reinterpret_cast< void** >( &sfFolder ) );
            if( FAILED( Ret ) )
                break;
            
            Ret = sfFolder->EnumObjects( nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN | SHCONTF_SHAREABLE | SHCONTF_STORAGE, &enumIDL );
            if( FAILED( Ret ) )
                break;
            
            while( true )
            {
                LPITEMIDLIST PIDL = nullptr;
                Ret = enumIDL->Next( 1, &PIDL, nullptr );
                if( Ret == NOERROR )
                {
                    STRRET Name = { STRRET_OFFSET, 0 };
                    sfFolder->GetDisplayNameOf( PIDL, SHGDN_INFOLDER | SHGDN_FORPARSING, &Name );
                    const auto Fs = TySpFileSource( new nsHC::CFileSourceT, []( nsHC::CFileSourceT* Ptr ) {
                        if( Ptr != nullptr )
                        {
                            if( Ptr->PIDL != nullptr )
                                ILFree( Ptr->PIDL );
                            Ptr->PIDL = nullptr;
                            delete Ptr;
                        }
                    } );

                    // NOTE: 미리 값을 세팅하면 해당 값의 여부만 검사하므로 더욱 빠르다.
                    DWORD dwAttributes = SFGAO_FOLDER | SFGAO_HIDDEN | SFGAO_SYSTEM;
                    sfFolder->GetAttributesOf( 1, (LPCITEMIDLIST*)&PIDL, &dwAttributes);
                    
                    Fs->Name_       = ConvertSTRRETTo( &Name );
                    if( FlagOn( dwAttributes, SFGAO_FOLDER ) || Child == nullptr )
                        Fs->Attributes_ |= FILE_ATTRIBUTE_DIRECTORY;
                    if( FlagOn( dwAttributes, SFGAO_HIDDEN ) )
                        Fs->Attributes_ |= FILE_ATTRIBUTE_HIDDEN;
                    if( FlagOn( dwAttributes, SFGAO_SYSTEM ) )
                        Fs->Attributes_ |= FILE_ATTRIBUTE_SYSTEM;
                    Fs->Size_       = 0;
                    Fs->Flags_      = CFileSourceT::FS_FLAG_PIDL;
                    if( Child == nullptr && RootId == FOLDERID_ComputerFolder )
                        Fs->Flags_ |= CFileSourceT::FS_FLAG_DRIVE;
                    Fs->Reserved0_  = 0;
                    Fs->Created_    = QDateTime::currentDateTime();
                    Fs->Modified_   = QDateTime::currentDateTime();

                    Fs->PIDL        = PIDL;

                    Items.push_back( Fs );
                }
                else
                {
                    break;
                }
            }

        } while( false );

        if( enumIDL != nullptr )
            enumIDL->Release();

        if( sfFolder != nullptr )
            sfFolder->Release();

        CoUninitialize();
        return Items;

            //int  a = 0;

            //do
            //{
            //    
            //    while( true )
            //    {
            //        LPITEMIDLIST pidl = nullptr;
            //        auto hr = EnumIDL->Next( 1, &pidl, nullptr );
            //        if( hr == NOERROR )
            //        {
            //            WIN32_FIND_DATAW ffw;
            //            SHGetDataFromIDListW( sfFolder, pidl, SHGDFIL_FINDDATA, &ffw, sizeof( WIN32_FIND_DATAW ) );
            //            const auto Fs = std::make_shared< nsHC::CFileSourceT >();
            //            Fs->Name_ = QString::fromWCharArray( ffw.cFileName );

            //            VecItems.push_back( Fs );

            //            ILFree( pidl );
            //            int a = 0;
            //        }
            //        else
            //        {
            //            break;
            //        }
            //    }

            //    sfFolder->Release();
            //} while( false );

            //ShellDesktop->Release();
    }

    ////////////////////////////////////////////////////////////////////////////////
    ///

    CFSSmb::CFSSmb( const QString& Root )
        : CFileSystemT( Root ), shl_net( FOLDERID_NetworkFolder )
    {
        cate_ = FS_CATE_REMOTE;
        features_ |= FS_FEA_LIST;
    }

#pragma comment( lib, "netapi32" )

    QVector<nsHC::TySpFileSource> CFSSmb::GetChildItems( const QString& Server, bool IncludeDotDot )
    {
        QVector<nsHC::TySpFileSource> Ret;
        if (Server.compare("\\\\tsclient", Qt::CaseInsensitive) == 0)
        {
            const auto Provider = hc::storage::MakeNativeReadProvider(hc::storage::LocatorKind::Smb);
            auto Cursor = Provider->Enumerate({hc::storage::LocatorKind::Smb, Server.toStdString()}, {}).get();
            if (!Cursor.HasValue()) return Ret;
            for (;;) {
                auto Page = Cursor.Value()->Next(128, {}).get();
                if (!Page.HasValue()) return {};
                for (const auto& Entry : Page.Value().entries_) {
                    auto Item = std::make_shared<nsHC::CFileSourceT>();
                    Item->Name_ = QString::fromStdString(Entry.name_);
                    Item->Attributes_ = FILE_ATTRIBUTE_DIRECTORY;
                    Ret.push_back(std::move(Item));
                }
                if (Page.Value().end_) return Ret;
            }
        }
        PSHARE_INFO_1 Buffer = nullptr, Cur = nullptr;
        NET_API_STATUS Res;
        DWORD er = 0, tr = 0, resume = 0;

        do
        {
            Res = NetShareEnum( (LPWSTR) Server.toStdWString().c_str(), 1, reinterpret_cast< LPBYTE* >( &Buffer ), MAX_PREFERRED_LENGTH, &er, &tr, &resume );

            if( Res == ERROR_SUCCESS || Res == ERROR_MORE_DATA )
            {
                Cur = Buffer;

                for( DWORD i = 1; i <= er; ++i )
                {
                    if( FlagOn( Cur->shi1_type, STYPE_SPECIAL ) )
                    {
                        Cur++;
                        continue;
                    }

                    const auto Fs = std::make_shared_for_overwrite< nsHC::CFileSourceT >();

                    Fs->Name_ = QString::fromWCharArray( Cur->shi1_netname );
                    Fs->Attributes_ |= FILE_ATTRIBUTE_DIRECTORY;
                    
                    Cur++;
                    Ret.push_back( Fs );
                }

                NetApiBufferFree( Buffer );
            }

        } while( Res == ERROR_MORE_DATA );

        return Ret;
    }

    ////////////////////////////////////////////////////////////////////////////////
    ///

    CFSPack::CFSPack( const QString& Root, const TySpWCX& WCX )
        : CFileSystemT( Root ), WCX_( WCX )
    {
        cate_       = FS_CATE_LOCAL;
        features_   |= FS_FEA_PACK;
    }

    CFSPack::~CFSPack()
    {
        if( FsModel_ != nullptr )
            delete FsModel_;
    }

    TySpWCX CFSPack::GetWCX() const
    {
        return WCX_;
    }

    QString CFSPack::GetParent() const
    {
        return Parent_;
    }

    void CFSPack::SetParent( const QString& Parent )
    {
        Parent_ = Parent;
    }

    CPackFSModel* CFSPack::Model()
    {
        if( FsModel_ != nullptr )
            return FsModel_;

        const auto Result = GetWCX()->CreatePackModel();
        if (!Result.Value.has_value()) return nullptr;
        FsModel_ = Result.Value.value();
        return FsModel_;
    }

    void CFSPack::RefreshReadProvider()
    {
        readProvider_.reset();
        delete FsModel_;
        FsModel_ = nullptr;
    }

    std::shared_ptr<hc::storage::IStorageProvider> CFileSystemT::ReadProvider()
    {
        if (!readProvider_)
            readProvider_ = hc::storage::MakeNativeReadProvider(cate_ == FS_CATE_REMOTE ? hc::storage::LocatorKind::Smb : hc::storage::LocatorKind::Local);
        return readProvider_;
    }

    hc::storage::Locator CFileSystemT::ReadLocation(const QString& Path) const
    {
        // The SMB root is stored as one slash and its root path may add a
        // second. Both spellings identify discovery, never an empty server.
        if (cate_ == FS_CATE_REMOTE && (Path == "\\" || Path == "\\\\"))
            return {hc::storage::LocatorKind::Smb, "\\"};
        return {cate_ == FS_CATE_REMOTE ? hc::storage::LocatorKind::Smb : hc::storage::LocatorKind::Local, Path.toStdString()};
    }

    hc::storage::Locator CFSShell::ReadLocation(const QString&) const
    {
        PWSTR Parsing = nullptr;
        std::string Name;
        if (Root && SUCCEEDED(SHGetNameFromIDList(Root, SIGDN_DESKTOPABSOLUTEPARSING, &Parsing)))
        {
            Name = QString::fromWCharArray(Parsing).toStdString();
            CoTaskMemFree(Parsing);
        }
        return {hc::storage::LocatorKind::Shell, std::move(Name)};
    }

    std::shared_ptr<hc::storage::IStorageProvider> CFSShell::ReadProvider()
    {
        if (!readProvider_) readProvider_ = hc::storage::MakeShellReadProvider(ReadLocation({}).root_);
        return readProvider_;
    }

    hc::storage::Locator CFSPack::ReadLocation(const QString& Path) const
    {
        auto Entry = QDir::fromNativeSeparators(Path);
        if (!Entry.startsWith('/')) Entry.prepend('/');
        auto Location = archiveLocation_.nesting_.empty() ? hc::storage::Locator{hc::storage::LocatorKind::Archive, root_.toStdString(), {{"wcx", "/"}}} : archiveLocation_;
        Location.nesting_.back().entry_ = Entry.toStdString(); return Location;
    }

    void CFSPack::SetArchiveChain(hc::storage::Locator Location, std::vector<std::shared_ptr<hc::plugins::IArchiveBackend>> Ancestors)
    { archiveLocation_ = std::move(Location); ancestors_ = std::move(Ancestors); RefreshReadProvider(); }

    std::vector<std::shared_ptr<hc::plugins::IArchiveBackend>> CFSPack::ArchiveBackends() const
    { auto Backends = ancestors_; Backends.push_back(WCX_->Backend() ? WCX_->Backend() : hc::plugins::MakeInProcessWcxBackend(WCX_->PluginPath().toStdString())); return Backends; }

    std::shared_ptr<hc::storage::IStorageProvider> CFSPack::ReadProvider()
    {
        if (!readProvider_ && WCX_)
        {
            const auto Root = ReadLocation("/");
            const auto Builtin = hc::plugins::BuiltinIsoOptions().binaryPath_;
            if (Root.nesting_.size() == 1 && QFileInfo(WCX_->PluginPath()).absoluteFilePath().compare(
                QFileInfo(Builtin).absoluteFilePath(), Qt::CaseInsensitive) == 0)
                readProvider_ = hc::iso::MakeReadProvider(Root);
            else
                readProvider_ = hc::storage::MakeWcxReadProvider(WCX_->PluginPath().toStdString(), Root, {}, WCX_->Backend(), ancestors_);
        }
        return readProvider_;
    }
} // nsHC
