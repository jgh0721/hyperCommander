#include "stdafx.h"
#include "solTCPluginMgr.hpp"

#include "UniqueLibs/cmnHCUtils.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

#define LOAD_FUNC( Name ) \
    TC->##Name = decltype( TC->##Name )( GetProcAddress( TC->hModule, #Name ) )

////////////////////////////////////////////////////////////////////////////////

CPackFSModel::CPackFSModel()
{
    Root_ = new Node;
    Root_->FileName = "";
    Root_->Attributes = FILE_ATTRIBUTE_DIRECTORY;

    Current_ = Root_;
}

CPackFSModel::~CPackFSModel()
{
    for( const auto& Item : allNodes() )
        delete Item;

    Root_ = Current_ = nullptr;
}

bool CPackFSModel::ChDir( const QString& Dir )
{
    const auto cd = search( QString( Dir ).replace( '\\', '/' ).split('/', Qt::SkipEmptyParts), false);
    if( cd == nullptr )
        return false;

    Current_ = cd;
    return true;
}

void CPackFSModel::MakeFile( Node* Item )
{
    Q_ASSERT( Item != nullptr );
    if( Item == nullptr )
        return;

    if( Item->FileName.contains( '/' ) == false && Item->FileName.contains( '\\' ) == false )
    {
        Current_->Children.push_back( Item );
    }
    else
    {
        QString s( Item->FileName );
        s = s.replace( "\\", "/" );

        const auto Parent = ensureDirectoryTree( s );
        Q_ASSERT( Parent != nullptr );
        Item->FileName = s.section( '/', -1 );
        if( !FlagOn( Item->Attributes, FILE_ATTRIBUTE_DIRECTORY ) )
            Parent->Children.push_back( Item );
        else
        {
            // 앞에서 디렉토리 노드만 미리 만들었을 수 있기 때문에 이곳에서 속성을 설정한다. 
            const auto Cur = search( s.split( '/' ), false );
            if( Cur != nullptr )
            {
                Cur->Attributes = Item->Attributes;
                Cur->CRC        = Item->CRC;
                Cur->FileTime   = Item->FileTime;
            }
            else
            {
                Parent->Children.push_back( Item );
            }
        }

        std::ranges::sort( Parent->Children, []( const Node* L, const Node* R ) { return L->FileName < R->FileName; } );
    }

    TotalCount_++;

    if( FlagOn( Item->Attributes, FILE_ATTRIBUTE_DIRECTORY ) )
        TotalFileCount_++;
    else
        TotalDirectoryCount_++;
}

QVector<CPackFSModel::Node*> CPackFSModel::FindFiles( const QString& Filter )
{
    QVector<CPackFSModel::Node*> Ret;

    if( Current_ == nullptr )
        return {};

    for( const auto Node : Current_->Children )
    {
        Ret.push_back( Node );
    }

    return Ret;
}

quint32 CPackFSModel::TotalCount() const
{
    return TotalCount_;
}

quint32 CPackFSModel::TotalFileCount() const
{
    return TotalFileCount_;
}

quint32 CPackFSModel::TotalDirectoryCount() const
{
    return TotalDirectoryCount_;
}

QSet<CPackFSModel::Node*> CPackFSModel::allNodes() const
{
    return retrieveChildren( Root_ );
}

QSet<CPackFSModel::Node*> CPackFSModel::retrieveChildren( Node* Parent ) const
{
    QSet<CPackFSModel::Node*> Nodes;
    qDebug() << __FUNCTION__ << " : " << Parent->FileName;

    for( const auto& Item : Parent->Children )
    {
        if( FlagOn( Item->Attributes, FILE_ATTRIBUTE_DIRECTORY ) )
            Nodes += retrieveChildren( Item );
        else
        {
            Nodes += Item;
            qDebug() << __FUNCTION__ << " : " << Item->FileName;
        }
    }

    Nodes += Parent;
    return Nodes;
}

CPackFSModel::Node* CPackFSModel::retrieveParentNode( const QString& FileFullPath )
{
    return search( FileFullPath.split( '/', Qt::SkipEmptyParts ), true );
}

CPackFSModel::Node* CPackFSModel::search( const QStringList& PIDL, bool isSearchParent /* = false */ ) const
{
    // NOTE: 아직 가상드라이브가 생성되지 않았지만, UI 에서 요청하는 일이 있을 수 있음
    if( Root_ == nullptr )
        return nullptr;

    if( PIDL.empty() == true )
    {
        if( isSearchParent == false )
            return Root_;

        return nullptr;
    }

    if( PIDL.size() == 1 )
    {
        if( isSearchParent == true )
            return Root_;
    }

    auto Parent = Root_;
    const auto Count = isSearchParent == true ? PIDL.size() - 1 : PIDL.size();

    for( int idx = 0; idx < Count; ++idx )
    {
        const auto& Name   = PIDL[ idx ];
        const auto& Search = std::ranges::find_if( Parent->Children, [Name]( const Node* Item ) { return Item->FileName == Name; } );
        if( Search != Parent->Children.end() )
            Parent = *Search;
    }

    return Parent;
}

CPackFSModel::Node* CPackFSModel::ensureDirectoryTree( const QString& Path )
{
    auto Parent = Root_;
    const auto PathList = Path.split( '/', Qt::SkipEmptyParts );

    do
    {
        if( PathList.count() <= 1 )
            break;

        for( int idx = 0; idx < PathList.count() - 1; ++idx )
        {
            const auto& Name = PathList[ idx ];
            const auto& Search = std::ranges::find_if( Parent->Children, [Name]( const Node* Item ) { return Item->FileName == Name; } );
            // std::ranges::binary_search( Parent->Children, Name, []( const Node* L, const Node* R ) { } );

            if( Search != Parent->Children.end() )
                Parent = *Search;
            else
            {
                auto Child = new CPackFSModel::Node;
                Child->FileName = Name;
                Child->Attributes |= FILE_ATTRIBUTE_DIRECTORY;
                Parent->Children.push_back( Child );
                Parent = Child;
            }
        }

    } while( false );

    return Parent;
}

CTCPlugInWLX::~CTCPlugInWLX()
{
    CloseWLX();
}

bool CTCPlugInWLX::LoadWLX( const QString& FilePath )
{
    bool IsSuccess = false;

    do
    {
        wlx_.FileFullPath = FilePath;
        wlx_.FileName = QFileInfo( FilePath ).baseName();

        bool IsExist = QFile::exists( wlx_.FileFullPath );
        if( IsExist == false )
        {
            if( wlx_.FileFullPath.endsWith( "64" ) == false )
            {
                IsExist = QFile::exists( ( wlx_.FileFullPath + "64" ) );
                if( IsExist == true )
                    wlx_.FileFullPath += "64";
            }
        }

        if( IsExist == false )
            break;

        CTCPlugInWLX::RetrieveWLXFuncs( wlx_.FileFullPath, &wlx_.Funcs );
        IsSuccess = true;

    } while( false );

    return IsSuccess;
}

void CTCPlugInWLX::SetWinId( HWND ListWin )
{
    wnd_ = ListWin;
}

void CTCPlugInWLX::CloseWLX()
{
    if( wlx_.Funcs.hModule == nullptr )
        return;

    if( wnd_ != nullptr && wlx_.Funcs.ListCloseWindow != nullptr )
    {
        wlx_.Funcs.ListCloseWindow( wnd_ );
        wnd_ = nullptr;
    }

    FreeLibrary( wlx_.Funcs.hModule );
    wlx_.Funcs.hModule = nullptr;
}

void CTCPlugInWLX::RetrieveWLXFuncs( const QString& PlugIn, TyPlugInWLXFuncs* TC )
{
    Q_ASSERT( TC != nullptr );
    if( TC == nullptr )
        return;
    
    TC->hModule = LoadLibraryW( PlugIn.toStdWString().c_str() );
    if( TC->hModule == nullptr )
    {
        // TODO: 64비트 윈도에서만 실행되도록 할 필요 있음
        if( PlugIn.endsWith( "wlx", Qt::CaseInsensitive ) == true )
        {
            TC->hModule = LoadLibraryW( ( PlugIn + "64" ).toStdWString().c_str() );
            if( TC->hModule == nullptr )
                return;
        }
        else
        {
            return;
        }
    }
    
    LOAD_FUNC( ListLoad );
    LOAD_FUNC( ListLoadW );
    LOAD_FUNC( ListLoadNext );
    LOAD_FUNC( ListLoadNextW );
    LOAD_FUNC( ListCloseWindow );
    LOAD_FUNC( ListGetDetectString );
    LOAD_FUNC( ListSearchText );
    LOAD_FUNC( ListSearchTextW );
    LOAD_FUNC( ListSearchDialog );
    LOAD_FUNC( ListSendCommand );
    LOAD_FUNC( ListPrint );
    LOAD_FUNC( ListPrintW );
    LOAD_FUNC( ListNotificationReceived );
    LOAD_FUNC( ListSetDefaultParams );
    LOAD_FUNC( ListGetPreviewBitmap );
    LOAD_FUNC( ListGetPreviewBitmapW );
}

TyPlugInWLXFuncs CTCPlugInWLX::Funcs() const
{
    return wlx_.Funcs;
}

////////////////////////////////////////////////////////////////////////////////

CTCPlugInWCX::~CTCPlugInWCX()
{
    CloseArchive();
}

TyOsError CTCPlugInWCX::LoadWCX( const TyPlugInWCX& WCX )
{
    TyOsError Err;

    do
    {
        wcx_ = WCX;
        wcx_.FileFullPath = wcx_.FileFullPath.replace( "%COMMANDER_PATH%", QCoreApplication::applicationDirPath(), Qt::CaseInsensitive );
        wcx_.FileName = QFileInfo( wcx_.FileFullPath ).baseName();

#if defined( _M_IX86 )
        bool IsExist = QFile::exists( wcx_.FileFullPath );
#elif defined( _M_X64 )
        bool IsExist = false;
        if( wcx_.FileFullPath.endsWith( "64" ) == false )
        {
            IsExist = QFile::exists( ( wcx_.FileFullPath + "64" ) );
            if( IsExist == true )
                wcx_.FileFullPath += "64";
        }
        else
        {
            IsExist = QFile::exists( wcx_.FileFullPath );
        }
#endif

        if( IsExist == false )
        {
            Err.ErrorCode = ERROR_FILE_NOT_FOUND;
            break;
        }

        RetrieveWCXFuncs( wcx_.FileFullPath, &wcx_.Funcs );

    } while( false );

    return Err;
}

TyOsValue<bool> CTCPlugInWCX::OpenArchive( const QString& FileFullPath, int Mode )
{
    TyOsValue<bool> Ret = { false, { TyOsError::OS_WIN32_ERROR, ERROR_NOT_SUPPORTED } };

    do
    {
        const auto Result = open( FileFullPath, Mode );

        if( Result.Value.has_value() == false || Result.Value.value() == nullptr )
        {
            Ret.ErrorCode = Result.ErrorCode;
            break;
        }

        ArchiveFile = FileFullPath;
        Arc = Result.Value.value();
        Ret.Value = true;

    } while( false );

    return Ret;
}

TyOsValue<CPackFSModel*> CTCPlugInWCX::CreatePackModel() const
{
    TyOsValue<CPackFSModel*> Ret;

    do
    {
        if( Arc == nullptr )
        {
            Ret.ErrorCode.Type = TyOsError::OS_WIN32_ERROR;
            Ret.ErrorCode.ErrorCode = ERROR_INVALID_HANDLE;
            break;
        }

        int Err = 0;
        // WCX 압축은 - 한 번에 전체 구조를 읽게 된다.
        CPackFSModel* Model = new CPackFSModel;

        do
        {
            tHeaderDataExW HDR = { 0, };
            Err = wcx_.Funcs.ReadHeaderExW( Arc, &HDR );
            if( Err != 0 )
                break;

            if( Err == 0 )
                Err = wcx_.Funcs.ProcessFileW( Arc, PK_SKIP, nullptr, nullptr );

            const auto Item = new CPackFSModel::Node;
            Item->FileName = "/" + QString::fromWCharArray( HDR.FileName );
            Item->Attributes = HDR.FileAttr;
            Item->CRC = HDR.FileCRC;
            Item->CompressedSize = ( static_cast< int64_t >( HDR.PackSizeHigh ) << 32 ) | ( HDR.PackSize );
            Item->UncompressedSize = ( static_cast< int64_t >( HDR.UnpSizeHigh ) << 32 ) | ( HDR.UnpSize);

            Model->MakeFile( Item );

            qDebug() << __FUNCTION__ << " : " << QString::fromWCharArray( HDR.FileName );

        } while( Err == 0 );

        if( Err != E_END_ARCHIVE )
        {
            // Error Occured!!
            if( Model != nullptr )
                delete Model;

            Ret.ErrorCode.Type = TyOsError::OS_ETC_ERROR;
            Ret.ErrorCode.ErrorCode = Err;
            break;
        }

        Ret.Value = Model;

    } while( false );

    return Ret;
}

void CTCPlugInWCX::ExtractFile( const QString& Src, const QString& Dst )
{
    // NOTE: WCX 는 압축을 풀려면 전체에 대한 ReadHeader 를 다시 호출해야 한다. 
    const auto Ret = open( ArchiveFile, PK_OM_EXTRACT );
    if( Ret.Value.has_value() == false || Ret.Value.value() == nullptr )
        return;

    int Err = 0;
    const auto V = Ret.Value.value();

    if( wcx_.Funcs.SetProcessDataProcW != nullptr )
    {
        wcx_.Funcs.SetProcessDataProcW( V, &CTCPlugInWCX::onProgressW );
    }
    else if( wcx_.Funcs.SetProcessDataProc != nullptr )
    {
        wcx_.Funcs.SetProcessDataProc( V, &CTCPlugInWCX::onProgressA );
    }

    // TODO: 향후 반복을 별도 함수로 분리하여 중복을 제거해야 한다.
    tHeaderDataExW HDR = { 0, };

    do
    {
        memset( &HDR, '\0', sizeof( HDR ) );
        Err = wcx_.Funcs.ReadHeaderExW( V, &HDR );
        if( Err != 0 )
            break;

        // TODO: Dst 의 볼륨에 공간이 충분하지 확인
        // TODO: Dst 의 볼륨이 쓰기 가능한지 확인
        // TODO: Dst 의 볼륨에 같은 파일이 있는지 확인

        const auto s = ( "/" + QString::fromWCharArray( HDR.FileName ) ).replace( '\\', '/' );

        if( Err == 0 )
        {
            if( s.compare( Src ) != 0 )
            {
                Err = wcx_.Funcs.ProcessFileW( V, PK_SKIP, nullptr, nullptr );
                continue;
            }

            const auto d = Dst.toStdWString();
            Err = wcx_.Funcs.ProcessFileW( V, PK_EXTRACT, nullptr, const_cast< WCHAR* >( d.c_str() ) );
            break;
        }

    } while( Err == 0 );

    close( V );
}

void CTCPlugInWCX::CloseArchive()
{
    close( Arc );

    Arc = nullptr;
}

void CTCPlugInWCX::CloseWCX()
{
    CloseArchive();

    if( wcx_.Funcs.hModule != nullptr )
    {
        FreeLibrary( wcx_.Funcs.hModule );
        wcx_.Funcs.hModule = nullptr;
    }
}

void CTCPlugInWCX::RetrieveWCXFuncs( const QString& PlugIn, TyPlugInWCXFuncs* TC )
{
    Q_ASSERT( TC != nullptr );
    if( TC == nullptr )
        return;

    TC->hModule = LoadLibraryW( PlugIn.toStdWString().c_str() );
    if( TC->hModule == nullptr )
    {
        // TODO: 64비트 윈도에서만 실행되도록 할 필요 있음
        if( PlugIn.endsWith( "wlx", Qt::CaseInsensitive ) == true )
        {
            TC->hModule = LoadLibraryW( ( PlugIn + "64" ).toStdWString().c_str() );
            if( TC->hModule == nullptr )
                return;
        }
        else
        {
            return;
        }
    }

    // 필수
    LOAD_FUNC( OpenArchive );
    LOAD_FUNC( OpenArchiveW );
    LOAD_FUNC( ReadHeader );
    LOAD_FUNC( ProcessFile );
    LOAD_FUNC( ProcessFileW );
    LOAD_FUNC( CloseArchive );
    LOAD_FUNC( SetChangeVolProc );
    LOAD_FUNC( SetChangeVolProcW );
    LOAD_FUNC( SetProcessDataProc );
    LOAD_FUNC( SetProcessDataProcW );

    // 옵션
    LOAD_FUNC( PackFiles );
    LOAD_FUNC( PackFilesW );
    LOAD_FUNC( DeleteFiles );
    LOAD_FUNC( DeleteFilesW );
    LOAD_FUNC( GetPackerCaps );
    LOAD_FUNC( ConfigurePacker );
    LOAD_FUNC( StartMemPack );
    LOAD_FUNC( StartMemPackW );
    LOAD_FUNC( PackToMem );
    LOAD_FUNC( DoneMemPack );
    LOAD_FUNC( CanYouHandleThisFile );
    LOAD_FUNC( CanYouHandleThisFileW );
    LOAD_FUNC( PackSetDefaultParams );
    LOAD_FUNC( PkSetCryptCallback );
    LOAD_FUNC( ReadHeaderEx );
    LOAD_FUNC( ReadHeaderExW );
    LOAD_FUNC( GetBackgroundFlags );
}

TyOsValue<HANDLE> CTCPlugInWCX::open( const QString& FileFullPath, int Mode )
{
    TyOsValue<HANDLE> Ret = { nullptr, { TyOsError::OS_WIN32_ERROR, ERROR_NOT_SUPPORTED } };

    if( wcx_.Funcs.hModule == nullptr )
        return Ret;

    do
    {
        if( wcx_.Funcs.OpenArchiveW != nullptr )
        {
            tOpenArchiveDataW Open;
            const auto ArcName = FileFullPath.toStdWString();
            Open.ArcName = const_cast< WCHAR* >( ArcName.c_str() );
            Open.OpenMode = Mode;
            Open.OpenResult = 0;
            Open.CmtBuf = nullptr;
            Open.CmtBufSize = 0;
            Open.CmtSize = 0;
            Open.CmtState = 0;

            Ret.Value = wcx_.Funcs.OpenArchiveW( &Open );

            if( Ret.Value.value() == NULL )
            {
                Ret.ErrorCode.Type = TyOsError::OS_ETC_ERROR;
                Ret.ErrorCode.ErrorCode = Open.OpenResult;

                break;
            }
        }

        if( Ret.Value.value() == NULL && wcx_.Funcs.OpenArchive != nullptr )
        {
            tOpenArchiveData Open;
            const auto ArcName = FileFullPath.toStdString();
            Open.ArcName = const_cast< char* >( ArcName.c_str() );
            Open.OpenMode = Mode;
            Open.OpenResult = 0;
            Open.CmtBuf = nullptr;
            Open.CmtBufSize = 0;
            Open.CmtSize = 0;
            Open.CmtState = 0;

            Ret.Value = wcx_.Funcs.OpenArchive( &Open );

            if( Ret.Value.value() == NULL )
            {
                Ret.ErrorCode.Type = TyOsError::OS_ETC_ERROR;
                Ret.ErrorCode.ErrorCode = Open.OpenResult;

                break;
            }
        }

    } while( false );

    return Ret;
}

int CTCPlugInWCX::close( HANDLE Archive )
{
    if( Archive == nullptr )
        return 0;

    if( wcx_.Funcs.CloseArchive == nullptr )
        return 0;

    return wcx_.Funcs.CloseArchive( Archive );
}

int CTCPlugInWCX::onProgressA( char* ArcName, int Mode )
{
    // NOTE: 사용자가 취소를 누르면 0 반환
    return 1;
}

int CTCPlugInWCX::onProgressW( WCHAR* ArcName, int Mode )
{
    // NOTE: 사용자가 취소를 누르면 0 반환
    qDebug() << __FUNCTION__ << " : " << QString::fromWCharArray( ArcName ) << " Mode => " << Mode;
    return 1;
}

////////////////////////////////////////////////////////////////////////////////

CTCPlugInMgr::CTCPlugInMgr( QObject* Parent )
    : QObject( Parent )
{}

//
//////////////////////////////////////////////////////////////////////////////
///// 
//
//
//void CInternalViewerMgr::cleanupViewerFuncs( TyInternalViewer_TC* TC )
//{
//    if( TC == nullptr )
//        return;
//
//    if( TC->hModule != nullptr )
//        FreeLibrary( TC->hModule );
//    TC->hModule = nullptr;
//
//    TC->ListLoad = nullptr;
//    TC->ListLoadW = nullptr;
//    TC->ListLoadNext = nullptr;
//    TC->ListLoadNextW = nullptr;
//    TC->ListCloseWindow = nullptr;
//    TC->ListGetDetectString = nullptr;
//    TC->ListSearchText = nullptr;
//    TC->ListSearchTextW = nullptr;
//    TC->ListSearchDialog = nullptr;
//    TC->ListSendCommand = nullptr;
//    TC->ListPrint = nullptr;
//    TC->ListPrintW = nullptr;
//    TC->ListNotificationReceived = nullptr;
//    TC->ListSetDefaultParams = nullptr;
//    TC->ListGetPreviewBitmap = nullptr;
//    TC->ListGetPreviewBitmapW = nullptr;
//}

void CTCPlugInMgr::Refresh()
{
    QVector< TyPlugInWLX > VecWLX;
    QVector< TyPlugInWCX > VecWCX;

    StSettings->beginGroup( "ListerPlugins" );
    const auto WLXCount = StSettings->value( "Count" ).toUInt();
    for( uint32_t Idx = 0; Idx < WLXCount; ++Idx )
    {
        TyPlugInWLX Plugin;
        Plugin.Index            = Idx;
        Plugin.FileFullPath     = StSettings->value( QString( "%1_Path" ).arg( Idx ) ).toString();
        Plugin.FileName         = QFileInfo( Plugin.FileFullPath ).baseName();
        Plugin.Detect           = StSettings->value( QString( "%1_Detect" ).arg( Idx ) ).toString();
        Plugin.IsContain64Bit   = StSettings->value( QString( "%1_Support64Bit" ).arg( Idx ) ).toBool();
        VecWLX.push_back( Plugin );
    }
    StSettings->endGroup();

    StSettings->beginGroup( "PackerPlugins" );
    const auto WCXCount = StSettings->value( "Count" ).toUInt();
    for( uint32_t Idx = 0; Idx < WCXCount; ++Idx )
    {
        TyPlugInWCX Plugin;
        Plugin.FileFullPath     = StSettings->value( QString( "%1_Path" ).arg( Idx ) ).toString();
        Plugin.PackerCaps       = StSettings->value( QString( "%1_Caps" ).arg( Idx ) ).toInt();
        Plugin.Exts             = StSettings->value( QString( "%1_Exts" ).arg( Idx ) ).toString().split( '|' );
        Plugin.IsContain64Bit   = StSettings->value( QString( "%1_Support64Bit" ).arg( Idx ) ).toBool();
        VecWCX.push_back( Plugin );
    }
    StSettings->endGroup();

    qSwap( VecWLX, vecWLX );
    qSwap( VecWCX, vecWCX );
}

void CTCPlugInMgr::SaveSettings()
{
    Save_WLX();
}

quint32 CTCPlugInMgr::GetWLXCount() const
{
    return vecWLX.size();
}

QString CTCPlugInMgr::GetWLXFilePath( quint32 Idx ) const
{
    if( vecWLX.isEmpty() == true || Idx >= vecWLX.size() )
        return "";

    return vecWLX.at( Idx ).FileFullPath;
}

QString CTCPlugInMgr::GetWLXDetect( quint32 Idx ) const
{
    if( vecWLX.isEmpty() == true || Idx >= vecWLX.size() )
        return "";

    return vecWLX.at( Idx ).Detect;
}

void CTCPlugInMgr::SetWLXOpts( quint32 Idx, const TyPlugInWLX& WLX )
{
    if( Idx >= vecWLX.size() && Idx != UINT_MAX )
        return;

    if( vecWLX.isEmpty() == true && Idx == 0 )
    {
        vecWLX.push_back( WLX );
        return;
    }

    if( Idx == UINT_MAX )
        vecWLX.push_back( WLX );
    else
        vecWLX[ Idx ] = WLX;
    
    Save_WLX();
}

void CTCPlugInMgr::DelWLXOpts( quint32 Idx )
{
    if( vecWLX.isEmpty() == true || Idx >= vecWLX.size() )
        return;

    vecWLX.removeAt( Idx );

    Save_WLX();
    Refresh();
}

quint32 CTCPlugInMgr::ConstructWLXMenu( QMenu* Menu, const QString& FileFullPath )
{
    if( Menu == nullptr )
        return 0;

    CDetectParser DetectParser;
    wchar_t Buffer[ MAX_PATH ] = { 0, };
    GetEnvironmentVariableW( L"COMMANDER_PATH", Buffer, MAX_PATH );

    for( auto& Item : vecWLX )
    {
        const auto File = QString( Item.FileFullPath ).replace( "%COMMANDER_PATH%", QString::fromWCharArray( Buffer ) );

        if( File.endsWith( "64" ) == false )
        {
            if( QFile::exists( File + "64" ) == false )
                continue;
        }
        else
        {
            if( QFile::exists( File ) == false )
                continue;
        }

        TySpWLX SpPlugInWLX( new CTCPlugInWLX );
        if( SpPlugInWLX->LoadWLX( File ) == false )
            continue;

        if( Item.Detect.isEmpty() == false )
        {
            DetectParser.SetDetectString( Item.Detect );
            if( DetectParser.TestFile( FileFullPath ) == false )
                continue;
        }

        const auto ac = new QAction;
        ac->setText( Item.FileName );
        ac->setData( QVariant::fromValue( SpPlugInWLX ) );
        Menu->addAction( ac );
        if( Item.Index == 0 )
            Menu->setDefaultAction( ac );
    }

    return Menu->actions().count();
}

QVector<TyPlugInWCX> CTCPlugInMgr::RetrieveWCXByExts( const QString& Ext ) const
{
    if( Ext.isEmpty() == true )
        return {};

    QVector< TyPlugInWCX > Ret;

    for( const auto& WCX : vecWCX )
    {
        if( WCX.Exts.contains( Ext, Qt::CaseInsensitive ) == false )
            continue;

        Ret.push_back( WCX );
    }

    return Ret;
}

void CTCPlugInMgr::Save_WLX()
{
    StSettings->beginGroup( "ListerPlugins" );
    StSettings->setValue( "Count", vecWLX.size() );
    for( uint32_t Idx = 0; Idx < vecWLX.size(); ++Idx )
    {
        const auto& WLX = vecWLX[ Idx ];
        StSettings->setValue( QString( "%1_Path" ).arg( Idx ), WLX.FileFullPath );
        StSettings->setValue( QString( "%1_Detect" ).arg( Idx ), WLX.Detect );
        StSettings->setValue( QString( "%1_Support64Bit" ).arg( Idx ), WLX.IsContain64Bit );
    }
    StSettings->endGroup();
}

#undef LOAD_FUNC
