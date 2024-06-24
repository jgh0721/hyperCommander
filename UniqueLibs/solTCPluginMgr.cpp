#include "stdafx.h"
#include "solTCPluginMgr.hpp"

#include "UniqueLibs/cmnHCUtils.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

////////////////////////////////////////////////////////////////////////////////

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
    
#define LOAD_FUNC( Name ) \
    TC->##Name = decltype( TC->##Name )( GetProcAddress( TC->hModule, #Name ) )
    
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
#undef LOAD_FUNC
}

TyPlugInWLXFuncs CTCPlugInWLX::Funcs() const
{
    return wlx_.Funcs;
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

    StSettings->beginGroup( "ListerPlugins" );
    const auto Count = StSettings->value( "Count" ).toUInt();
    for( uint32_t Idx = 0; Idx < Count; ++Idx )
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

    qSwap( VecWLX, vecWLX );
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

