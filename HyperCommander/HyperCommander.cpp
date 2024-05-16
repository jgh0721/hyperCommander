#include "stdafx.h"
#include "HyperCommander.hpp"

#include "dlgMain.hpp"
#include "../uniqueLibs/SHChangeNotify.hpp"
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/shortcutMgr.hpp"

HyperCommanderApp::HyperCommanderApp( int& argc, char** argv )
    : QApplication( argc, argv )
{
    QPixmapCache::setCacheLimit( 65535 );

    QCoreApplication::setOrganizationName( "MyHouse" );
    QCoreApplication::setOrganizationDomain( "devtester.com" );
    QCoreApplication::setApplicationName( "HyperCommander" );

    //QSettings::setDefaultFormat( QSettings::IniFormat );
    //QSettings::setPath( QSettings::IniFormat, QSettings::UserScope, applicationDirPath() );

    //const auto StSettings = TyStSettings::GetInstance();
    //

    //QSettings f( QString("%1/111.ini").arg( applicationDirPath() ), QSettings::IniFormat );
    //f.beginGroup( "fsdfdsfds" );
    //f.setValue( "fsdfdsds1", "rew" );
    //f.endGroup();
    //const auto qw  = f.fileName();

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

    shlChangeNotify = new QSHChangeNotify;
    shlChangeNotify->StartWatching();

    auto ui = new QMainUI;
    ui->show();

    TyStCommandMgr::GetInstance()->Refresh();

    TyStShortcutMgr::GetInstance()->SetShortcut( QKeySequence( "Shift+Return", QKeySequence::PortableText ), "ContextMenu" );


}
