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
#include "fileSetMgr.hpp"
#include "QtnPlatform.h"

HyperCommanderApp::HyperCommanderApp( int& argc, char** argv )
    : QApplication( argc, argv )
{
    auto Font = QApplication::font();
    Font.setStyleStrategy( QFont::PreferAntialias  );
    Font.setHintingPreference( QFont::PreferFullHinting );
    QApplication::setFont( Font );
    
    QApplication::setWindowIcon( QIcon( ":/HyperCommander/Logo" ) );
    QApplication::setStyle( QStyleFactory::create( "windows11" ) );
    QPixmapCache::setCacheLimit( 65535 );

    QCoreApplication::setOrganizationName( "MyHouse" );
    QCoreApplication::setOrganizationDomain( "devtester.com" );
    QCoreApplication::setApplicationName( "HyperCommander" );

    SetEnvironmentVariableW( L"COMMANDER_PATH", QDir::toNativeSeparators( applicationDirPath() ).toStdWString().c_str() );

    TyStCommandMgr::GetInstance()->Refresh();
    TyStFileSetMgr::GetInstance()->Refresh();
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
