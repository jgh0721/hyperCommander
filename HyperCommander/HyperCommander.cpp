#include "stdafx.h"
#include "HyperCommander.hpp"

#include "cmnHCUtils.hpp"
#include "dlgMain.hpp"
#include "dlgMultiRename.hpp"

#include "../uniqueLibs/SHChangeNotify.hpp"
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/colorSchemeMgr.hpp"

#include "cmnTypeDefs.hpp"
#include "cmnTypeDefs_Name.hpp"
#include "cmnTypeDefs_Opts.hpp"
#include "QtnPlatform.h"

HyperCommanderApp::HyperCommanderApp( int& argc, char** argv )
    : QApplication( argc, argv )
{
    QApplication::setStyle( QStyleFactory::create( "windows11" ) );
    QPixmapCache::setCacheLimit( 65535 );

    QCoreApplication::setOrganizationName( "MyHouse" );
    QCoreApplication::setOrganizationDomain( "devtester.com" );
    QCoreApplication::setApplicationName( "HyperCommander" );

    SetEnvironmentVariableW( L"COMMANDER_PATH", QDir::toNativeSeparators( applicationDirPath() ).toStdWString().c_str() );

    do
    {
        SHFILEINFO SHInfo = { 0, };

        if( SHGetFileInfoW( L"C:\\", 0, &SHInfo, sizeof( SHFILEINFOW ), SHGFI_ADDOVERLAYS | SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES ) != FALSE )
        {
            QPixmapCache::insert( "[Drive]", QPixmap::fromImage( QImage::fromHICON( SHInfo.hIcon ) ) );
            DestroyIcon( SHInfo.hIcon );
        }

    } while( false );


    do
    {
        SHFILEINFO SHInfo = { 0, };

        if( SHGetFileInfoW( L"C:\\ABCD", FILE_ATTRIBUTE_DIRECTORY, &SHInfo, sizeof( SHFILEINFOW ), SHGFI_ADDOVERLAYS | SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES ) != FALSE )
        {
            if( QPixmapCache::insert( "Directory", QPixmap::fromImage( QImage::fromHICON( SHInfo.hIcon ) ) ) == false )
            {
                int a = 0;
            }
            QPixmap pm;
            if( QPixmapCache::find( "Directory", &pm ) == false )
            {
                int a = 0;
            }

            DestroyIcon( SHInfo.hIcon );
        }

    } while( false );

    TyStCommandMgr::GetInstance()->Refresh();
    TyStColorSchemeMgr::GetInstance()->Refresh();

    mainUI = new QMainUI;
    StSettings->beginGroup( OPT_SEC_WINDOW );
    mainUI->restoreGeometry( StSettings->value( "geometry" ).toByteArray() );
    StSettings->endGroup();
    mainUI->show();
}

void HyperCommanderApp::ShowMultiRename( const QVector< QString >& VecFiles )
{
    QMultiRenameUI MultiRename;
    MultiRename.SetSourceFiles( VecFiles );
    MultiRename.exec();
}

///////////////////////////////////////////////////////////////////////////////

void HyperCommanderApp::OnDriveAdd( const QString& Root )
{

}

void HyperCommanderApp::OnDriveRemoved( const QString& Root )
{

}

void HyperCommanderApp::OnMediaInserted( const QString& ItemIDDisplayName )
{

}

void HyperCommanderApp::OnMediaRemoved( const QString& Root )
{

}

///////////////////////////////////////////////////////////////////////////////

void HyperCommanderApp::initialize()
{
    shlChangeNotify = new QSHChangeNotify;
    shlChangeNotify->StartWatching();

    connect( shlChangeNotify, &QSHChangeNotify::OnDriveAdd, this, &HyperCommanderApp::OnDriveAdd );
    connect( shlChangeNotify, &QSHChangeNotify::OnDriveRemoved, this, &HyperCommanderApp::OnDriveRemoved );
    connect( shlChangeNotify, &QSHChangeNotify::OnMediaInserted, this, &HyperCommanderApp::OnMediaInserted );
    connect( shlChangeNotify, &QSHChangeNotify::OnMediaRemoved, this, &HyperCommanderApp::OnMediaRemoved );


}
