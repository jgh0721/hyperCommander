#include "stdafx.h"
#include "HyperCommander.hpp"

#include "cmnHCUtils.hpp"
#include "dlgMain.hpp"
#include "dlgMultiRename.hpp"

#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/colorSchemeMgr.hpp"

#include "cmnTypeDefs.hpp"
#include "cmnTypeDefs_Name.hpp"
#include "cmnTypeDefs_Opts.hpp"
#include "externalEditorMgr.hpp"
#include "fileSetMgr.hpp"
#include "QtnPlatform.h"
#include "solTCPluginMgr.hpp"
#include "Modules/mdlFileSystem.hpp"

#include "externalLibs/QtitanDataGrid/src/src/base/QtnCommonStyle.h"

HyperCommanderApp::HyperCommanderApp( int& argc, char** argv )
    : QApplication( argc, argv )
{
    Qtitan::CommonStyle::ensureStyle();
    QSettings::setDefaultFormat( QSettings::IniFormat );

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

    initialize();

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

///////////////////////////////////////////////////////////////////////////////

void HyperCommanderApp::initialize()
{
    const auto StCommandMgr = TyStCommandMgr::GetInstance();
    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();

    TyStExternalEditorMgr::GetInstance()->Refresh();
    TyStPlugInMgr::GetInstance()->Refresh();
    TyStFileSetMgr::GetInstance()->Refresh();

    StCommandMgr->Refresh();
    StColorSchemeMgr->Refresh();
}
