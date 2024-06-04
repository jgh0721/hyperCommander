#include "stdafx.h"
#include "internalViewerMgr.hpp"

void CInternalViewerMgr::Refresh()
{

}

quint32 CInternalViewerMgr::ConstructInternalMenu( QMenu* Menu, const QString& FileFullPath )
{
    if( vecInternalItems.isEmpty() == true )
    {
        vecInternalItems.push_back( {
            0, "%COMMANDER_PATH%\\Addons\\WLX\\PEViewer\\PEViewer.wlx",
            "",
            true,

            "%COMMANDER_PATH%\\Addons\\WLX", "PEViewer.wlx"
        } );
    }

    wchar_t Buffer[ MAX_PATH ] = { 0, };
    GetEnvironmentVariableW( L"COMMANDER_PATH", Buffer, MAX_PATH );

    for( auto& Item : vecInternalItems )
    {
        const auto WLX = QString( Item.FileFullPath ).replace( "%COMMANDER_PATH%", QString::fromWCharArray( Buffer ) );

        if( QFile::exists( WLX + "64" ) == false )
            continue;

        searchViewerFuncs( WLX + "64", &Item.Funcs );

    }

    for( const auto& Item : vecInternalItems )
    {
        const auto WLX = QString(Item.FileFullPath).replace( "%COMMANDER_PATH%", QString::fromWCharArray( Buffer ) );

        if( QFile::exists( WLX ) == false )
            continue;

        const auto ac = new QAction;
        ac->setText( Item.FileName );
        ac->setData( QVariant::fromValue( Item ) );
        Menu->addAction( ac );
    }

    return Menu->actions().count();
}

////////////////////////////////////////////////////////////////////////////
/// 

void CInternalViewerMgr::searchViewerFuncs( const QString& PlugIn, TyInternalViewer_TC* TC )
{
    Q_ASSERT( TC != nullptr );
    if( TC == nullptr )
        return;

    TC->hModule = LoadLibraryW( PlugIn.toStdWString().c_str() );
    if( TC->hModule == nullptr )
        return;

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

void CInternalViewerMgr::cleanupViewerFuncs( TyInternalViewer_TC* TC )
{
    if( TC == nullptr )
        return;

    if( TC->hModule != nullptr )
        FreeLibrary( TC->hModule );
    TC->hModule = nullptr;

    TC->ListLoad = nullptr;
    TC->ListLoadW = nullptr;
    TC->ListLoadNext = nullptr;
    TC->ListLoadNextW = nullptr;
    TC->ListCloseWindow = nullptr;
    TC->ListGetDetectString = nullptr;
    TC->ListSearchText = nullptr;
    TC->ListSearchTextW = nullptr;
    TC->ListSearchDialog = nullptr;
    TC->ListSendCommand = nullptr;
    TC->ListPrint = nullptr;
    TC->ListPrintW = nullptr;
    TC->ListNotificationReceived = nullptr;
    TC->ListSetDefaultParams = nullptr;
    TC->ListGetPreviewBitmap = nullptr;
    TC->ListGetPreviewBitmapW = nullptr;
}
